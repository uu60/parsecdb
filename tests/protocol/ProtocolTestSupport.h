// Shared helpers for the three-party protocol correctness tests. Rank 2 (the client) generates plaintext inputs,
// the operators secret-share them to ranks 0 and 1, and the client checks the reconstructed outputs against a
// plaintext computation. Mismatches are logged with the word MISMATCH so CTest's FAIL_REGULAR_EXPRESSION catches them
// even when the exit status is lost.
#ifndef PARSEC_TESTS_PROTOCOL_TEST_SUPPORT_H
#define PARSEC_TESTS_PROTOCOL_TEST_SUPPORT_H

#include "parsecdb/comm/Comm.h"
#include "parsecdb/conf/AppConfig.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace parsecdb::protocol_test {

// Number of random elements per batch case. Edge-value combinations are added on top.
inline int randomCount() { return AppConfig::getInt("count", 1000); }

// Plaintext inputs come from a seeded PRNG so a failure can be reproduced with --seed=<value>.
inline std::mt19937_64 makeRng(int salt) {
    const auto seed = static_cast<uint64_t>(AppConfig::getInt("seed", 20261114));
    return std::mt19937_64(seed * 1000003ULL + static_cast<uint64_t>(salt));
}

// Cases that reproduce known operator bugs are skipped by default. --known_bugs=<id>[,<id>...] (or `all`) runs them;
// CMakeLists.txt registers one such run per bug with WILL_FAIL, so it starts failing once the operator is fixed.
inline bool knownBug(const std::string& id) {
    const std::string list = "," + AppConfig::getString("known_bugs", "") + ",";
    return list.find(",all,") != std::string::npos || list.find("," + id + ",") != std::string::npos;
}

inline uint64_t mask(int width) { return width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1; }

inline int64_t ring(int64_t v, int width) { return Math::ring(v, width); }

// Two's complement value of the low `width` bits.
inline int64_t toSigned(int64_t v, int width) {
    if (width >= 64) {
        return v;
    }
    const auto bits = static_cast<uint64_t>(v) & mask(width);
    const uint64_t sign = uint64_t{1} << (width - 1);
    return static_cast<int64_t>((bits ^ sign) - sign);
}

inline uint64_t toUnsigned(int64_t v, int width) { return static_cast<uint64_t>(v) & mask(width); }

inline int64_t randomRing(std::mt19937_64& rng, int width) { return ring(static_cast<int64_t>(rng()), width); }

// 0, 1, 2, -1 (all ones), the signed minimum and maximum, and their neighbours, reduced to `width` bits and
// de-duplicated.
inline std::vector<int64_t> edgeValues(int width) {
    std::vector<int64_t> raw = {0, 1, 2, -1, -2};
    if (width > 1) {
        const int64_t smax = static_cast<int64_t>(mask(width - 1));
        const int64_t smin = ring(static_cast<int64_t>(uint64_t{1} << (width - 1)), width);
        raw.insert(raw.end(), {smax, smax - 1, smin, smin + 1});
    }
    std::vector<int64_t> out;
    for (int64_t v : raw) {
        const int64_t r = ring(v, width);
        if (std::find(out.begin(), out.end(), r) == out.end()) {
            out.push_back(r);
        }
    }
    return out;
}

// All ordered pairs of edge values (so equal operands are included), followed by `count` random pairs, a block of
// equal random pairs and a block of pairs that differ in a single bit.
inline void makePairs(std::mt19937_64& rng, int width, int count, std::vector<int64_t>& xs, std::vector<int64_t>& ys) {
    xs.clear();
    ys.clear();
    const auto edges = edgeValues(width);
    for (int64_t x : edges) {
        for (int64_t y : edges) {
            xs.push_back(x);
            ys.push_back(y);
        }
    }
    for (int i = 0; i < count; ++i) {
        xs.push_back(randomRing(rng, width));
        ys.push_back(randomRing(rng, width));
    }
    for (int i = 0; i < count / 8; ++i) {
        const int64_t x = randomRing(rng, width);
        xs.push_back(x);
        ys.push_back(x);
    }
    for (int i = 0; i < count / 8; ++i) {
        const int64_t x = randomRing(rng, width);
        const int bit = static_cast<int>(rng() % static_cast<uint64_t>(width));
        xs.push_back(x);
        ys.push_back(Math::changeBit(x, bit, !Math::getBit(x, bit)));
    }
}

// At most `n` pairs drawn from the edge values (every fourth pair equal). Used for batches that must stay small, such
// as boolean batches whose width does not divide 64 (see the bool tests).
inline void makeSmallPairs(int width, int n, std::vector<int64_t>& xs, std::vector<int64_t>& ys) {
    xs.clear();
    ys.clear();
    const auto edges = edgeValues(width);
    for (int i = 0; i < n; ++i) {
        const int64_t x = edges[i % edges.size()];
        xs.push_back(x);
        ys.push_back(i % 4 == 0 ? x : edges[(i * 3 + 1) % edges.size()]);
    }
}

// Elements per batch for boolean operators at a width that does not divide 64: BoolAndBatchOperator packs its JIT
// bitwise triples into 64-bit words and BitwiseBmt::extract throws once an element straddles two words, which cannot
// happen while the whole batch fits in one word. `andsPerElement` is how many AND lanes the operator spends per
// element (2 for the multiplexer).
inline int oneWordBatch(int width, int andsPerElement = 1) { return 64 / (width * andsPerElement); }

inline bool widthDivides64(int width) { return 64 % width == 0; }

// Compares reconstructed results on the client. Logs at most 10 mismatches per case.
inline bool checkResults(const std::string& name, int width, const std::vector<int64_t>& expected,
                         const std::vector<int64_t>& got, const std::vector<int64_t>* xs = nullptr,
                         const std::vector<int64_t>* ys = nullptr, const std::vector<int64_t>* cs = nullptr) {
    if (!Comm::isClient()) {
        return true;
    }
    if (expected.size() != got.size()) {
        Log::e("{} width={} MISMATCH result size: expected={} got={}", name, width, expected.size(), got.size());
        return false;
    }
    int mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] == got[i]) {
            continue;
        }
        if (++mismatches <= 10) {
            Log::e("{} width={} MISMATCH index={} x={} y={} c={} expected={} got={}", name, width, i,
                   xs ? std::to_string((*xs)[i]) : std::string("-"), ys ? std::to_string((*ys)[i]) : std::string("-"),
                   cs ? std::to_string((*cs)[i]) : std::string("-"), expected[i], got[i]);
        }
    }
    if (mismatches > 0) {
        Log::e("{} width={} FAIL mismatches={}/{}", name, width, mismatches, expected.size());
        return false;
    }
    Log::i("{} width={} ok ({} elements)", name, width, expected.size());
    return true;
}

inline bool checkSingle(const std::string& name, int width, int64_t x, int64_t y, int64_t expected, int64_t got) {
    if (!Comm::isClient() || expected == got) {
        return true;
    }
    Log::e("{} width={} MISMATCH x={} y={} expected={} got={}", name, width, x, y, expected, got);
    return false;
}

// Prints the PASS/FAIL line on the client and returns the process exit status.
inline int finish(const std::string& suite, bool passed) {
    if (Comm::isClient()) {
        Log::i(passed ? "[{} correctness] PASS" : "[{} correctness] FAIL", suite);
    }
    return passed ? 0 : 1;
}

} // namespace parsecdb::protocol_test

#endif
