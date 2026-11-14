
#include "parsecdb/utils/Math.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>

namespace parsecdb {

BIGNUM* bignum(const std::string& str, const bool positive) {
    std::vector<uint8_t> binaryData = std::vector<uint8_t>(str.begin(), str.end());
    BIGNUM* bn = BN_new();
    BN_bin2bn(binaryData.data(), binaryData.size(), bn);
    if (!positive) {
        BN_set_negative(bn, 1);
    }
    return bn;
}

BIGNUM* bignum(const std::string& str) { return bignum(str, true); }

std::string string(BIGNUM* bn) {
    int num_bytes = BN_num_bytes(bn);
    std::vector<unsigned char> bin(num_bytes);
    BN_bn2bin(bn, bin.data());

    std::string binary_str;
    for (unsigned char byte : bin) {
        for (int i = 7; i >= 0; --i) {
            binary_str.push_back((byte & (1 << i)) ? '1' : '0');
        }
    }
    std::string result;
    for (size_t i = 0; i < binary_str.size(); i += 8) {
        std::string byte_str = binary_str.substr(i, 8);
        char byte = static_cast<char>(std::stoi(byte_str, nullptr, 2));
        result.push_back(byte);
    }
    return result;
}

BIGNUM* add_bignum_with_int(BIGNUM* add0, int64_t add1) {
    BIGNUM* bn_add1 = BN_new();
    BN_set_word(bn_add1, add1);
    BIGNUM* result = BN_new();
    BN_add(result, add0, bn_add1);
    BN_free(bn_add1);
    return result;
}

std::string add_or_minus(const std::string& add0, const std::string& add1, bool minus) {
    BIGNUM* add0N = bignum(add0);
    BIGNUM* add1N = bignum(add1, !minus);
    BIGNUM* result = BN_new();
    BN_add(result, add0N, add1N);
    std::string resultStr = string(result);
    BN_free(add0N);
    BN_free(add1N);
    BN_free(result);
    return resultStr;
}

namespace {

// Per-thread buffer of CSPRNG output. RAND_bytes is OpenSSL's DRBG (thread-safe, reseeded by OpenSSL); buffering
// amortizes the call overhead for the many 8-byte draws made by the protocols.
class RandomBuffer {
  public:
    void fill(unsigned char* out, size_t bytes) {
        while (bytes > 0) {
            if (_pos == _buf.size()) {
                refill();
            }
            const size_t n = std::min(bytes, _buf.size() - _pos);
            std::memcpy(out, _buf.data() + _pos, n);
            _pos += n;
            out += n;
            bytes -= n;
        }
    }

    ~RandomBuffer() { OPENSSL_cleanse(_buf.data(), _buf.size()); }

  private:
    void refill() {
        if (RAND_bytes(_buf.data(), static_cast<int>(_buf.size())) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        _pos = 0;
    }

    std::array<unsigned char, 4096> _buf{};
    size_t _pos = _buf.size();
};

RandomBuffer& randomBuffer() {
    thread_local RandomBuffer buffer;
    return buffer;
}

uint64_t randomU64() {
    uint64_t v;
    randomBuffer().fill(reinterpret_cast<unsigned char*>(&v), sizeof(v));
    return v;
}

} // namespace

void Math::randBytes(void* out, size_t bytes) { randomBuffer().fill(static_cast<unsigned char*>(out), bytes); }

int64_t Math::randBits() { return static_cast<int64_t>(randomU64()); }

int64_t Math::randInt() { return static_cast<int64_t>(randomU64() >> 1); }

int64_t Math::randInt(int64_t lowest, int64_t highest) {
    if (lowest > highest) {
        throw std::invalid_argument("randInt: lowest must not exceed highest");
    }
    const uint64_t span = static_cast<uint64_t>(highest) - static_cast<uint64_t>(lowest);
    if (span == UINT64_MAX) {
        return static_cast<int64_t>(randomU64());
    }
    // Rejection sampling: accept draws below the largest multiple of (span + 1) to avoid modulo bias.
    const uint64_t range = span + 1;
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % range + 1) % range;
    uint64_t r;
    do {
        r = randomU64();
    } while (r > limit);
    return static_cast<int64_t>(static_cast<uint64_t>(lowest) + r % range);
}

std::string Math::add(const std::string& add0, int64_t add1) {
    BIGNUM* add0N = bignum(add0);
    BIGNUM* sum = add_bignum_with_int(add0N, add1);
    std::string result = string(sum);
    BN_free(add0N);
    BN_free(sum);
    return result;
}

std::string Math::randString(int bytes) {
    if (bytes < 0) {
        throw std::invalid_argument("randString: bytes must be non-negative");
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    randBytes(out.data(), out.size());
    return out;
}

std::string Math::add(const std::string& add0, const std::string& add1) { return add_or_minus(add0, add1, false); }

std::string Math::minus(const std::string& add0, const std::string& add1) { return add_or_minus(add0, add1, true); }

namespace {
int64_t fromUnsignedBits(uint64_t value) {
    int64_t result;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
} // namespace

int64_t Math::ring(int64_t num, int width) {
    if (width <= 0 || width > 64) {
        throw std::invalid_argument("ring width must be in [1, 64]");
    }
    uint64_t bits = static_cast<uint64_t>(num);
    if (width < 64) {
        bits &= (uint64_t{1} << width) - 1;
    }
    return fromUnsignedBits(bits);
}

int64_t Math::ringAdd(int64_t lhs, int64_t rhs, int width) {
    return ring(fromUnsignedBits(static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs)), width);
}

int64_t Math::ringSubtract(int64_t lhs, int64_t rhs, int width) {
    return ring(fromUnsignedBits(static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs)), width);
}

int64_t Math::ringMultiply(int64_t lhs, int64_t rhs, int width) {
    return ring(fromUnsignedBits(static_cast<uint64_t>(lhs) * static_cast<uint64_t>(rhs)), width);
}

bool Math::getBit(int64_t v, int i) { return (static_cast<uint64_t>(v) >> i) & 1; }

int64_t Math::changeBit(int64_t v, int i, bool b) {
    const uint64_t bit = uint64_t{1} << i;
    const uint64_t result = (static_cast<uint64_t>(v) & ~bit) | (b ? bit : uint64_t{0});
    return fromUnsignedBits(result);
}

int64_t Math::pow(int64_t base, int64_t exponent) {
    int64_t result = 1;
    while (exponent > 0) {
        if (exponent % 2 == 1) {
            result *= base;
        }
        base *= base;
        exponent /= 2;
    }
    return result;
}

uint64_t Math::kdfSha256To8Bytes(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) == nullptr) {
        throw std::runtime_error("kdfSha256To8Bytes: SHA256 failed");
    }
    uint64_t result = 0;
    std::memcpy(&result, digest, sizeof(uint64_t));
    return result;
}

std::array<uint64_t, 2> Math::kdfSha256To16Bytes(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) == nullptr) {
        throw std::runtime_error("kdfSha256To16Bytes: SHA256 failed");
    }
    std::array<uint64_t, 2> result{};
    std::memcpy(result.data(), digest, sizeof(result));
    return result;
}

} // namespace parsecdb
