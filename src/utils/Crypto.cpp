#include "parsecdb/utils/Crypto.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace parsecdb {

static inline void store_u64_le(uint64_t x, unsigned char out[8]) {
    out[0] = (unsigned char)(x);
    out[1] = (unsigned char)(x >> 8);
    out[2] = (unsigned char)(x >> 16);
    out[3] = (unsigned char)(x >> 24);
    out[4] = (unsigned char)(x >> 32);
    out[5] = (unsigned char)(x >> 40);
    out[6] = (unsigned char)(x >> 48);
    out[7] = (unsigned char)(x >> 56);
}

// ============== Seeded PRG ==============

Crypto::SeededPrg::SeededPrg(const U128* seeds, size_t numSeeds) : _ctxs(numSeeds, nullptr) {
    for (size_t i = 0; i < numSeeds; ++i) {
        unsigned char key[16];
        store_u64_le(seeds[i].lo, key);
        store_u64_le(seeds[i].hi, key + 8);
        _ctxs[i] = EVP_CIPHER_CTX_new();
        const bool ok = _ctxs[i] != nullptr &&
                        EVP_EncryptInit_ex(_ctxs[i], EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
                        EVP_CIPHER_CTX_set_padding(_ctxs[i], 0) == 1;
        OPENSSL_cleanse(key, sizeof(key));
        if (!ok) {
            for (auto* ctx : _ctxs) {
                EVP_CIPHER_CTX_free(ctx);
            }
            throw std::runtime_error("SeededPrg: AES key setup failed");
        }
    }
}

Crypto::SeededPrg::~SeededPrg() {
    for (auto* ctx : _ctxs) {
        EVP_CIPHER_CTX_free(ctx);
    }
}

void Crypto::SeededPrg::generate(const U128& nonce, uint64_t stream, U128* out, size_t blocksPerSeed) const {
    if (blocksPerSeed == 0) {
        return;
    }
    // Counter blocks are identical for every seed: (nonce.lo ^ block, nonce.hi ^ stream). They are distinct for
    // distinct (stream, block) under one nonce, so each AES key never encrypts the same input twice in a call
    // sequence that uses a fresh nonce.
    std::vector<unsigned char> counters(blocksPerSeed * 16);
    for (size_t b = 0; b < blocksPerSeed; ++b) {
        store_u64_le(nonce.lo ^ static_cast<uint64_t>(b), counters.data() + b * 16);
        store_u64_le(nonce.hi ^ stream, counters.data() + b * 16 + 8);
    }
    for (size_t i = 0; i < _ctxs.size(); ++i) {
        unsigned char* dst = reinterpret_cast<unsigned char*>(out + i * blocksPerSeed);
        int outLen = 0;
        if (EVP_EncryptUpdate(_ctxs[i], dst, &outLen, counters.data(), static_cast<int>(counters.size())) != 1 ||
            outLen != static_cast<int>(counters.size())) {
            throw std::runtime_error("SeededPrg: AES encryption failed");
        }
    }
}

// ============== Hash Function ==============

uint64_t Crypto::hash64(int index, uint64_t v) {
    // Cryptographically secure hash for OT protocols
    // Uses SHA-256 and extracts first 64 bits
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char input[16];

    // Pack index (4 bytes) + v (8 bytes) + domain separator (4 bytes)
    store_u64_le(v, input);
    std::memcpy(input + 8, &index, sizeof(int));

    // Domain separation constant to prevent hash collisions with other uses
    const uint32_t domain = 0x494B4E50; // "IKNP" in ASCII
    std::memcpy(input + 12, &domain, sizeof(uint32_t));

    SHA256(input, 16, digest);

    // Extract first 64 bits as little-endian
    uint64_t result;
    std::memcpy(&result, digest, sizeof(uint64_t));
    return result;
}

uint64_t Crypto::hash64(int index, const U128& v) {
    // Cryptographically secure hash for OT protocols with 128-bit input
    // Uses SHA-256 and extracts first 64 bits
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char input[24];

    // Pack: v.lo (8 bytes) + v.hi (8 bytes) + index (4 bytes) + domain (4 bytes)
    store_u64_le(v.lo, input);
    store_u64_le(v.hi, input + 8);
    std::memcpy(input + 16, &index, sizeof(int));

    // Domain separation constant
    const uint32_t domain = 0x494B4E50; // "IKNP" in ASCII
    std::memcpy(input + 20, &domain, sizeof(uint32_t));

    SHA256(input, 24, digest);

    // Extract first 64 bits as little-endian
    uint64_t result;
    std::memcpy(&result, digest, sizeof(uint64_t));
    return result;
}

uint64_t Crypto::hashBatchForBits(int baseIndex, const U128* columns, const size_t* colIndices, size_t count) {
    // Batch hash for packed-bits IKNP
    // Returns a 64-bit result where bit i = LSB(hash(columns[colIndices[i]]))
    // This avoids per-bit function call overhead
    uint64_t result = 0;

    // Process in batch - still using SHA-256 but with reduced call overhead
    for (size_t i = 0; i < count; ++i) {
        const size_t colIdx = colIndices[i];
        const U128& col = columns[colIdx];
        const int index = baseIndex + static_cast<int>(i);

        // Inline the hash computation to avoid function call overhead
        unsigned char digest[SHA256_DIGEST_LENGTH];
        unsigned char input[24];

        store_u64_le(col.lo, input);
        store_u64_le(col.hi, input + 8);
        std::memcpy(input + 16, &index, sizeof(int));

        const uint32_t domain = 0x494B4E50; // "IKNP"
        std::memcpy(input + 20, &domain, sizeof(uint32_t));

        SHA256(input, 24, digest);

        // Extract just LSB
        if (digest[0] & 1) {
            result |= (1ULL << i);
        }
    }

    return result;
}

// ============== Fixed-key AES hashes for IKNP ==============
//
// All fast OT hashes use the tweakable correlation-robust (TCCR) construction of Guo, Katz, Wang and Yu, "Efficient
// and Secure Multiparty Computation from Fixed-Key Block Ciphers" (IEEE S&P 2020), as used by EMP and MP-SPDZ:
//
//     H(i, x) = pi(pi(x) XOR i) XOR pi(x)
//
// where pi is AES-128 under a fixed public key and the tweak i is the OT index encoded as the block (i, 0). Callers
// use the low 64 bits (or only the least significant bit) of H.

namespace {

const unsigned char kFixedAesKey[16] = {0x49, 0x4B, 0x4E, 0x50, // "IKNP"
                                        0x48, 0x41, 0x53, 0x48, // "HASH"
                                        0x4B, 0x45, 0x59, 0x31, // "KEY1"
                                        0x00, 0x00, 0x00, 0x00};

// One fixed-key AES context per thread; key expansion happens once.
EVP_CIPHER_CTX* fixedKeyAes() {
    thread_local struct Holder {
        EVP_CIPHER_CTX* ctx = nullptr;
        Holder() {
            ctx = EVP_CIPHER_CTX_new();
            if (ctx == nullptr || EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, kFixedAesKey, nullptr) != 1 ||
                EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                ctx = nullptr;
            }
        }
        ~Holder() { EVP_CIPHER_CTX_free(ctx); }
    } holder;
    if (holder.ctx == nullptr) {
        throw std::runtime_error("Crypto: fixed-key AES setup failed");
    }
    return holder.ctx;
}

void permute(const unsigned char* in, unsigned char* out, size_t blocks) {
    if (blocks == 0) {
        return;
    }
    int outLen = 0;
    if (EVP_EncryptUpdate(fixedKeyAes(), out, &outLen, in, static_cast<int>(blocks * 16)) != 1 ||
        outLen != static_cast<int>(blocks * 16)) {
        throw std::runtime_error("Crypto: fixed-key AES failed");
    }
}

// out[k] = low 64 bits of H(index[k], x[k]) for k < n, n <= kMaxTccrBlocks.
constexpr size_t kMaxTccrBlocks = 256;

void tccrLow64(const uint64_t* index, const U128* x, uint64_t* out, size_t n) {
    if (n > kMaxTccrBlocks) {
        throw std::invalid_argument("Crypto: too many blocks for one TCCR call");
    }
    // Per-thread scratch space, allocated once.
    thread_local std::vector<unsigned char> bufStorage(kMaxTccrBlocks * 16);
    thread_local std::vector<unsigned char> pxStorage(kMaxTccrBlocks * 16);
    unsigned char* buf = bufStorage.data();
    unsigned char* px = pxStorage.data();
    for (size_t k = 0; k < n; ++k) {
        store_u64_le(x[k].lo, buf + k * 16);
        store_u64_le(x[k].hi, buf + k * 16 + 8);
    }
    permute(buf, px, n); // pi(x)
    for (size_t k = 0; k < n; ++k) {
        std::memcpy(buf + k * 16, px + k * 16, 16);
        for (int b = 0; b < 8; ++b) {
            buf[k * 16 + b] ^= static_cast<unsigned char>(index[k] >> (8 * b));
        }
    }
    permute(buf, buf, n); // pi(pi(x) ^ i)
    for (size_t k = 0; k < n; ++k) {
        uint64_t h = 0;
        uint64_t p = 0;
        for (int b = 7; b >= 0; --b) {
            h = (h << 8) | buf[k * 16 + b];
            p = (p << 8) | px[k * 16 + b];
        }
        out[k] = h ^ p;
    }
}

inline uint64_t tweak(int index) { return static_cast<uint64_t>(static_cast<uint32_t>(index)); }

} // namespace

uint64_t Crypto::hash64Fast(int index, const U128& v) {
    const uint64_t i = tweak(index);
    uint64_t h = 0;
    tccrLow64(&i, &v, &h, 1);
    return h;
}

uint64_t Crypto::hashBatchForBitsFast(int baseIndex, const U128* columns, const size_t* colIndices, size_t count) {
    // Bit i of the result is the LSB of H(baseIndex + i, columns[colIndices[i]]), for i < min(count, 64).
    const size_t n = std::min(count, static_cast<size_t>(64));
    uint64_t idx[64] = {};
    U128 in[64] = {};
    uint64_t h[64] = {};
    for (size_t i = 0; i < n; ++i) {
        idx[i] = tweak(baseIndex + static_cast<int>(i));
        in[i] = columns[colIndices[i]];
    }
    tccrLow64(idx, in, h, n);
    uint64_t result = 0;
    for (size_t i = 0; i < n; ++i) {
        result |= (h[i] & 1ULL) << i;
    }
    return result;
}

void Crypto::hashTileBatch(int baseIndex, const U128* tile, size_t validCount, uint64_t* hashBits) {
    // Hashes up to 128 columns of a tile in one batched AES pass and packs the LSB of each hash into hashBits[0..1].
    hashBits[0] = 0;
    hashBits[1] = 0;
    const size_t count = std::min(validCount, static_cast<size_t>(128));
    if (count == 0) {
        return;
    }
    uint64_t idx[128];
    uint64_t h[128];
    for (size_t k = 0; k < count; ++k) {
        idx[k] = tweak(baseIndex + static_cast<int>(k));
    }
    tccrLow64(idx, tile, h, count);
    for (size_t k = 0; k < count; ++k) {
        hashBits[k >> 6] |= (h[k] & 1ULL) << (k & 63);
    }
}

void Crypto::hashTileBatchPair(int baseIndex, const U128* tile, const U128* tileXorS, size_t validCount,
                               uint64_t* h0Bits, uint64_t* h1Bits) {
    // Hashes tile[k] and tileXorS[k] (same tweak) for up to 128 columns in one batched AES pass.
    h0Bits[0] = h0Bits[1] = 0;
    h1Bits[0] = h1Bits[1] = 0;
    const size_t count = std::min(validCount, static_cast<size_t>(128));
    if (count == 0) {
        return;
    }
    uint64_t idx[256];
    U128 in[256];
    uint64_t h[256];
    for (size_t k = 0; k < count; ++k) {
        idx[k] = idx[count + k] = tweak(baseIndex + static_cast<int>(k));
        in[k] = tile[k];
        in[count + k] = tileXorS[k];
    }
    tccrLow64(idx, in, h, 2 * count);
    for (size_t k = 0; k < count; ++k) {
        h0Bits[k >> 6] |= (h[k] & 1ULL) << (k & 63);
        h1Bits[k >> 6] |= (h[count + k] & 1ULL) << (k & 63);
    }
}

void Crypto::transpose128x128_inplace(U128 v[128]) {
    uint64_t x0[128];
    uint64_t x1[128];
    for (int i = 0; i < 128; ++i) {
        x0[i] = v[i].lo;
        x1[i] = v[i].hi;
    }

    auto transpose64 = [](uint64_t x[128]) {
        static constexpr uint64_t m1 = 0x5555555555555555ULL;
        static constexpr uint64_t m2 = 0x3333333333333333ULL;
        static constexpr uint64_t m4 = 0x0f0f0f0f0f0f0f0fULL;
        static constexpr uint64_t m8 = 0x00ff00ff00ff00ffULL;
        static constexpr uint64_t m16 = 0x0000ffff0000ffffULL;
        static constexpr uint64_t m32 = 0x00000000ffffffffULL;

        auto swap_rows = [&](int shift, uint64_t mask) {
            for (int i = 0; i < 128; i += 2 * shift) {
                for (int j = 0; j < shift; ++j) {
                    uint64_t a = x[i + j];
                    uint64_t b = x[i + j + shift];
                    uint64_t t = ((a >> shift) ^ b) & mask;
                    b ^= t;
                    a ^= (t << shift);
                    x[i + j] = a;
                    x[i + j + shift] = b;
                }
            }
        };

        swap_rows(1, m1);
        swap_rows(2, m2);
        swap_rows(4, m4);
        swap_rows(8, m8);
        swap_rows(16, m16);
        swap_rows(32, m32);
    };

    transpose64(x0);
    transpose64(x1);

    for (int i = 0; i < 64; ++i) {
        std::swap(x0[i + 64], x1[i]);
    }

    for (int i = 0; i < 128; ++i) {
        v[i] = {x0[i], x1[i]};
    }
}

void Crypto::transpose128xNBlocks_full(const std::array<std::vector<U128>, 128>& rows, std::vector<U128>& cols,
                                       size_t blocks) {
    cols.assign(blocks * 128, U128{0, 0});

    U128 tile[128];
    for (size_t blk = 0; blk < blocks; ++blk) {
        for (int r = 0; r < 128; ++r) {
            tile[r] = rows[r][blk];
        }
        transpose128x128_inplace(tile);
        for (int bit = 0; bit < 128; ++bit) {
            cols[blk * 128 + static_cast<size_t>(bit)] = tile[bit];
        }
    }
}

// ============== Column Extraction Functions ==============

U128 Crypto::extract_col0_from_rows(const U128 rows[128]) {
    uint64_t lo = 0, hi = 0;
    for (int r = 0; r < 64; ++r) {
        lo |= (rows[r].lo & 1ULL) << r;
    }
    for (int r = 64; r < 128; ++r) {
        hi |= (rows[r].lo & 1ULL) << (r - 64);
    }
    return U128{lo, hi};
}

U128 Crypto::extract_col64_from_rows(const U128 rows[128]) {
    uint64_t lo = 0, hi = 0;
    for (int r = 0; r < 64; ++r) {
        lo |= (rows[r].hi & 1ULL) << r;
    }
    for (int r = 64; r < 128; ++r) {
        hi |= (rows[r].hi & 1ULL) << (r - 64);
    }
    return U128{lo, hi};
}

void Crypto::extract128xNBlocks_col0_col64_fast(const U128* rows, size_t blocks, U128* outCol0, U128* outCol64) {
    U128 tile[128];
    for (size_t blk = 0; blk < blocks; ++blk) {
        const U128* base = rows + blk;
        for (int r = 0; r < 128; ++r) {
            tile[r] = base[static_cast<size_t>(r) * blocks];
        }
        outCol0[blk] = extract_col0_from_rows(tile);
        outCol64[blk] = extract_col64_from_rows(tile);
    }
}

void Crypto::extract_uRaw_col0_col64_fast(const uint64_t* uRaw, size_t blocks, U128* outCol0, U128* outCol64) {
    U128 tile[128];

    for (size_t blk = 0; blk < blocks; ++blk) {
        for (int r = 0; r < 128; ++r) {
            const size_t off = (static_cast<size_t>(r) * blocks + blk) * 2;
            tile[r].lo = static_cast<uint64_t>(uRaw[off]);
            tile[r].hi = static_cast<uint64_t>(uRaw[off + 1]);
        }
        outCol0[blk] = extract_col0_from_rows(tile);
        outCol64[blk] = extract_col64_from_rows(tile);
    }
}

} // namespace parsecdb
