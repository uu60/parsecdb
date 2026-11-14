#include "parsecdb/ot/BaseOtBatchOperator.h"

#include "parsecdb/comm/Comm.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/utils/System.h"
#include <array>
#include <cstring>
#include <memory>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>

namespace parsecdb {

namespace {

// Compressed SEC1 encoding of a P-256 point.
constexpr size_t kPointBytes = 33;
constexpr char kKdfDomain[] = "parsec/base-ot/chou-orlandi/p256/v1";

struct BnFree {
    void operator()(BIGNUM* bn) const { BN_clear_free(bn); }
};
struct PointFree {
    void operator()(EC_POINT* p) const { EC_POINT_clear_free(p); }
};
using BnPtr = std::unique_ptr<BIGNUM, BnFree>;
using PointPtr = std::unique_ptr<EC_POINT, PointFree>;

// One P-256 group and scratch context. Not thread-safe; every operator execution creates its own.
class Curve {
  public:
    Curve() : _group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)), _ctx(BN_CTX_new()) {
        if (_group == nullptr || _ctx == nullptr) {
            release();
            throw std::runtime_error("BaseOt: failed to set up P-256");
        }
    }
    ~Curve() { release(); }
    Curve(const Curve&) = delete;
    Curve& operator=(const Curve&) = delete;

    // Uniform scalar in [1, q-1].
    BnPtr randomScalar() const {
        BnPtr k(BN_secure_new());
        const BIGNUM* order = EC_GROUP_get0_order(_group);
        if (!k) {
            throw std::runtime_error("BaseOt: BN allocation failed");
        }
        do {
            if (BN_priv_rand_range(k.get(), order) != 1) {
                throw std::runtime_error("BaseOt: scalar sampling failed");
            }
        } while (BN_is_zero(k.get()));
        return k;
    }

    PointPtr newPoint() const {
        PointPtr p(EC_POINT_new(_group));
        if (!p) {
            throw std::runtime_error("BaseOt: point allocation failed");
        }
        return p;
    }

    // k * G
    PointPtr mulGenerator(const BIGNUM* k) const {
        PointPtr r = newPoint();
        if (EC_POINT_mul(_group, r.get(), k, nullptr, nullptr, _ctx) != 1) {
            throw std::runtime_error("BaseOt: scalar multiplication failed");
        }
        return r;
    }

    // k * P
    PointPtr mul(const EC_POINT* p, const BIGNUM* k) const {
        PointPtr r = newPoint();
        if (EC_POINT_mul(_group, r.get(), nullptr, p, k, _ctx) != 1) {
            throw std::runtime_error("BaseOt: scalar multiplication failed");
        }
        return r;
    }

    PointPtr add(const EC_POINT* p, const EC_POINT* q) const {
        PointPtr r = newPoint();
        if (EC_POINT_add(_group, r.get(), p, q, _ctx) != 1) {
            throw std::runtime_error("BaseOt: point addition failed");
        }
        return r;
    }

    void invert(EC_POINT* p) const {
        if (EC_POINT_invert(_group, p, _ctx) != 1) {
            throw std::runtime_error("BaseOt: point inversion failed");
        }
    }

    void encode(const EC_POINT* p, char* out) const {
        const size_t n = EC_POINT_point2oct(_group, p, POINT_CONVERSION_COMPRESSED,
                                            reinterpret_cast<unsigned char*>(out), kPointBytes, _ctx);
        if (n != kPointBytes) {
            throw std::runtime_error("BaseOt: point encoding failed");
        }
    }

    std::string encode(const EC_POINT* p) const {
        std::string out(kPointBytes, '\0');
        encode(p, out.data());
        return out;
    }

    // Decodes a peer's point. EC_POINT_oct2point checks that the point lies on the curve; the point at infinity is
    // rejected as well, since it would make both OT keys equal.
    PointPtr decode(const char* in) const {
        PointPtr p = newPoint();
        if (EC_POINT_oct2point(_group, p.get(), reinterpret_cast<const unsigned char*>(in), kPointBytes, _ctx) != 1 ||
            EC_POINT_is_at_infinity(_group, p.get())) {
            throw std::runtime_error("BaseOt: received an invalid curve point");
        }
        return p;
    }

  private:
    void release() {
        BN_CTX_free(_ctx);
        EC_GROUP_free(_group);
        _ctx = nullptr;
        _group = nullptr;
    }

    EC_GROUP* _group;
    BN_CTX* _ctx;
};

// H(i, A, B, P) = SHA-256(domain || i || A || B || P), truncated to two little-endian 64-bit words.
std::array<uint64_t, 2> deriveKey(uint64_t index, const char* a, const char* b, const char* p) {
    unsigned char input[sizeof(kKdfDomain) + 8 + 3 * kPointBytes];
    unsigned char* w = input;
    std::memcpy(w, kKdfDomain, sizeof(kKdfDomain));
    w += sizeof(kKdfDomain);
    for (int k = 0; k < 8; ++k) {
        *w++ = static_cast<unsigned char>(index >> (8 * k));
    }
    std::memcpy(w, a, kPointBytes);
    std::memcpy(w + kPointBytes, b, kPointBytes);
    std::memcpy(w + 2 * kPointBytes, p, kPointBytes);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(input, sizeof(input), digest) == nullptr) {
        throw std::runtime_error("BaseOt: SHA-256 failed");
    }
    std::array<uint64_t, 2> key{};
    std::memcpy(key.data(), digest, sizeof(key));
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(input, sizeof(input));
    return key;
}

} // namespace

BaseOtBatchOperator::BaseOtBatchOperator(int sender, std::vector<int64_t>* ms0, std::vector<int64_t>* ms1,
                                         std::vector<int>* choices, int width, int taskTag, int msgTagOffset)
    : AbstractOtBatchOperator(sender, ms0, ms1, choices, width, taskTag, msgTagOffset) {}

BaseOtBatchOperator* BaseOtBatchOperator::withHighWords(std::vector<int64_t>* ms0Hi, std::vector<int64_t>* ms1Hi) {
    _wide = true;
    _ms0Hi = ms0Hi;
    _ms1Hi = ms1Hi;
    return this;
}

BaseOtBatchOperator* BaseOtBatchOperator::execute() {
    _currentMsgTag = _startMsgTag;
    if (!Comm::isServer()) {
        return this;
    }

    int64_t start = 0;
    if (Conf::ENABLE_CLASS_WISE_TIMING) {
        start = System::currentTimeMillis();
    }

    if (_isSender) {
        runSender();
    } else {
        runReceiver();
    }

    if (Conf::ENABLE_CLASS_WISE_TIMING) {
        _totalTime += System::currentTimeMillis() - start;
    }

    return this;
}

void BaseOtBatchOperator::runSender() {
    const size_t n = _ms0->size();
    if (_ms1->size() != n ||
        (_wide && (_ms0Hi == nullptr || _ms1Hi == nullptr || _ms0Hi->size() != n || _ms1Hi->size() != n))) {
        throw std::invalid_argument("BaseOtBatchOperator: sender message vectors differ in size");
    }
    const size_t msgBytes = _wide ? 16 : 8;
    const int tag = buildTag(_currentMsgTag);
    Curve curve;

    const BnPtr a = curve.randomScalar();
    const PointPtr bigA = curve.mulGenerator(a.get());
    const std::string encA = curve.encode(bigA.get());
    Comm::serverSend(encA, tag);

    std::string allB;
    Comm::serverReceive(allB, tag);
    if (allB.size() != n * kPointBytes) {
        throw std::runtime_error("BaseOtBatchOperator: unexpected receiver message size");
    }

    // -aA, so that a(B - A) = aB + (-aA).
    PointPtr negAA = curve.mul(bigA.get(), a.get());
    curve.invert(negAA.get());

    std::string ciphertexts(n * 2 * msgBytes, '\0');
    char enc0[kPointBytes];
    char enc1[kPointBytes];
    for (size_t i = 0; i < n; ++i) {
        const char* encB = allB.data() + i * kPointBytes;
        const PointPtr bigB = curve.decode(encB);
        const PointPtr p0 = curve.mul(bigB.get(), a.get());
        const PointPtr p1 = curve.add(p0.get(), negAA.get());
        curve.encode(p0.get(), enc0);
        curve.encode(p1.get(), enc1);
        const auto k0 = deriveKey(i, encA.data(), encB, enc0);
        const auto k1 = deriveKey(i, encA.data(), encB, enc1);

        const uint64_t c0[2] = {static_cast<uint64_t>((*_ms0)[i]) ^ k0[0],
                                _wide ? static_cast<uint64_t>((*_ms0Hi)[i]) ^ k0[1] : 0};
        const uint64_t c1[2] = {static_cast<uint64_t>((*_ms1)[i]) ^ k1[0],
                                _wide ? static_cast<uint64_t>((*_ms1Hi)[i]) ^ k1[1] : 0};
        std::memcpy(ciphertexts.data() + (2 * i) * msgBytes, c0, msgBytes);
        std::memcpy(ciphertexts.data() + (2 * i + 1) * msgBytes, c1, msgBytes);
    }
    OPENSSL_cleanse(enc0, sizeof(enc0));
    OPENSSL_cleanse(enc1, sizeof(enc1));

    Comm::serverSend(ciphertexts, tag);
}

void BaseOtBatchOperator::runReceiver() {
    const size_t n = _choices->size();
    const size_t msgBytes = _wide ? 16 : 8;
    const int tag = buildTag(_currentMsgTag);
    Curve curve;

    std::string encA;
    Comm::serverReceive(encA, tag);
    if (encA.size() != kPointBytes) {
        throw std::runtime_error("BaseOtBatchOperator: unexpected sender message size");
    }
    const PointPtr bigA = curve.decode(encA.data());

    std::vector<BnPtr> bs;
    bs.reserve(n);
    std::string allB(n * kPointBytes, '\0');
    for (size_t i = 0; i < n; ++i) {
        bs.push_back(curve.randomScalar());
        PointPtr bigB = curve.mulGenerator(bs.back().get());
        if ((*_choices)[i] & 1) {
            bigB = curve.add(bigB.get(), bigA.get());
        }
        curve.encode(bigB.get(), allB.data() + i * kPointBytes);
    }
    Comm::serverSend(allB, tag);

    // The keys depend only on values already known, so derive them before waiting for the ciphertexts.
    std::vector<std::array<uint64_t, 2>> keys(n);
    char encP[kPointBytes];
    for (size_t i = 0; i < n; ++i) {
        const PointPtr p = curve.mul(bigA.get(), bs[i].get());
        curve.encode(p.get(), encP);
        keys[i] = deriveKey(i, encA.data(), allB.data() + i * kPointBytes, encP);
    }
    OPENSSL_cleanse(encP, sizeof(encP));
    bs.clear();

    std::string ciphertexts;
    Comm::serverReceive(ciphertexts, tag);
    if (ciphertexts.size() != n * 2 * msgBytes) {
        throw std::runtime_error("BaseOtBatchOperator: unexpected ciphertext size");
    }

    _results.resize(n);
    if (_wide) {
        _resultsHi.resize(n);
    }
    for (size_t i = 0; i < n; ++i) {
        const int b = (*_choices)[i] & 1;
        uint64_t c[2] = {0, 0};
        std::memcpy(c, ciphertexts.data() + (2 * i + b) * msgBytes, msgBytes);
        _results[i] = static_cast<int64_t>(c[0] ^ keys[i][0]);
        if (_wide) {
            _resultsHi[i] = static_cast<int64_t>(c[1] ^ keys[i][1]);
        }
    }
    OPENSSL_cleanse(keys.data(), keys.size() * sizeof(keys[0]));
}

int BaseOtBatchOperator::tagStride() { return 1; }

} // namespace parsecdb
