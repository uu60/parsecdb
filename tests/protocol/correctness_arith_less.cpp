// Arithmetic-share less-than. The operator converts x - y to a boolean share (A2B) and returns its most significant
// bit, i.e. a signed comparison that is exact whenever x - y does not overflow the ring. The main cases therefore
// draw signed operands from [-2^(w-2), 2^(w-2)); a second block probes overflowing pairs (such as min < max) and only
// checks that the operator returns MSB(x - y), noting how often that differs from a true signed comparison.
// ArithSecret::lessThan (a one-element batch) is checked on the in-range edge values.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithLessBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
// Signed operands whose difference cannot overflow `width` bits, as ring elements.
std::vector<int64_t> inRangeEdges(int width) {
    const int64_t hi = (int64_t{1} << (width - 2)) - 1; // 2 <= width <= 64, so the shift stays below 63
    const int64_t lo = -hi - 1;
    std::vector<int64_t> raw = {0, 1, -1, 2, -2, hi, hi - 1, lo, lo + 1};
    std::vector<int64_t> out;
    for (int64_t v : raw) {
        if (v < lo || v > hi) {
            continue;
        }
        const int64_t r = ring(v, width);
        if (std::find(out.begin(), out.end(), r) == out.end()) {
            out.push_back(r);
        }
    }
    return out;
}

int64_t randomInRange(std::mt19937_64& rng, int width) {
    // Arithmetic shift keeps the sign: the value lies in [-2^(w-2), 2^(w-2)).
    return ring(static_cast<int64_t>(rng()) >> (64 - (width - 1)), width);
}

int64_t signedLess(int64_t x, int64_t y, int width) { return toSigned(x, width) < toSigned(y, width) ? 1 : 0; }

int64_t msbOfDifference(int64_t x, int64_t y, int width) {
    return Math::getBit(Math::ringSubtract(x, y, width), width - 1) ? 1 : 0;
}

bool testBatchInRange(int width, int salt) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        const auto edges = inRangeEdges(width);
        for (int64_t x : edges) {
            for (int64_t y : edges) {
                xs.push_back(x);
                ys.push_back(y);
            }
        }
        const int count = randomCount();
        for (int i = 0; i < count; ++i) {
            xs.push_back(randomInRange(rng, width));
            ys.push_back(i % 8 == 0 ? xs.back() : randomInRange(rng, width));
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(signedLess(xs[i], ys[i], width));
        }
    }
    ArithLessBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("ArithLess batch", width, expected, op._results, &xs, &ys);
}

bool testBatchOverflow(int width) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        const auto edges = edgeValues(width); // full range, includes signed min and max
        for (int64_t x : edges) {
            for (int64_t y : edges) {
                xs.push_back(x);
                ys.push_back(y);
                expected.push_back(msbOfDifference(x, y, width));
            }
        }
    }
    ArithLessBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    if (Comm::isClient()) {
        int differ = 0;
        for (size_t i = 0; i < xs.size(); ++i) {
            differ += signedLess(xs[i], ys[i], width) != expected[i];
        }
        Log::i("ArithLess width={} note: {}/{} full-range edge pairs overflow, so MSB(x - y) differs from signed x < y",
               width, differ, xs.size());
    }
    return checkResults("ArithLess batch (MSB of x - y, overflowing pairs)", width, expected, op._results, &xs, &ys);
}

bool testContainer(int width) {
    bool passed = true;
    const auto edges = inRangeEdges(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 4 + 2) % edges.size()];
        for (auto [a, b] : std::array<std::pair<int64_t, int64_t>, 3>{{{x, y}, {y, x}, {x, x}}}) {
            const int task = System::nextTask();
            const ArithSecret as = shareArith(a, width, task);
            const ArithSecret bs = shareArith(b, width, task);
            passed =
                checkSingle("ArithSecret::lessThan", width, a, b, signedLess(a, b, width), openBit(as.lessThan(bs))) &&
                passed;
        }
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 100;
    for (int width : std::array<int, 6>{2, 4, 8, 16, 32, 64}) {
        passed = testBatchInRange(width, ++salt) && passed;
        passed = testBatchOverflow(width) && passed;
    }
    for (int width : std::array<int, 5>{2, 4, 8, 32, 64}) {
        passed = testContainer(width) && passed;
    }

    const int status = finish("ArithLess", passed);
    System::finalize();
    return status;
}
