
#ifndef MPC_PACKAGE_MATH_H
#define MPC_PACKAGE_MATH_H

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Forward declarations for OpenSSL types (avoid including OpenSSL headers in header)
typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;

namespace parsecdb {

class Math {
  public:
    // All random values below come from OpenSSL's CSPRNG (RAND_bytes), buffered per thread.

    // Uniform over all 2^64 values. Use this for shares, masks and other protocol randomness.
    static int64_t randBits();

    // Fills `out` with `bytes` random bytes.
    static void randBytes(void* out, size_t bytes);

    // Uniform over [0, INT64_MAX] (never negative). Intended for generating plaintext test data; protocol code that
    // needs a uniform ring element must use randBits().
    static int64_t randInt();

    // Uniform over [lowest, highest], without modulo bias.
    static int64_t randInt(int64_t lowest, int64_t highest);

    static int64_t pow(int64_t base, int64_t exponent);

    static std::string randString(int bytes);

    static std::string add(const std::string& add0, int64_t add1);

    static std::string add(const std::string& add0, const std::string& add1);

    static std::string minus(const std::string& add0, const std::string& add1);

    static int64_t ring(int64_t num, int width);

    static int64_t ringAdd(int64_t lhs, int64_t rhs, int width);

    static int64_t ringSubtract(int64_t lhs, int64_t rhs, int width);

    static int64_t ringMultiply(int64_t lhs, int64_t rhs, int width);

    static bool getBit(int64_t v, int i);

    static int64_t changeBit(int64_t v, int i, bool b);

    template <int width> static std::string toBinString(int64_t v) { return std::bitset<width>(v).to_string(); };

    /**
     * KDF using SHA256: compute SHA256 and return first 8 bytes as uint64_t.
     * @param input Input byte string
     * @return First 8 bytes of SHA256 digest as uint64_t
     * @throws std::runtime_error on failure
     */
    static uint64_t kdfSha256To8Bytes(const std::string& input);

    // First 16 bytes of SHA-256(input) as two little-endian words; word 0 equals kdfSha256To8Bytes(input).
    static std::array<uint64_t, 2> kdfSha256To16Bytes(const std::string& input);
};

} // namespace parsecdb

#endif
