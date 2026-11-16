// Boolean-share less-than (BoolLess*Operator): unsigned comparison of the low `width` bits. Batch
// operator, plus BoolSecret::lessThan and BitSecret::lessThan (one-element batches).
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
int64_t unsignedLess(int64_t x, int64_t y, int width) { return toUnsigned(x, width) < toUnsigned(y, width) ? 1 : 0; }

bool testBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        makePairs(rng, width, randomCount(), xs, ys);
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(unsignedLess(xs[i], ys[i], width));
        }
    }
    BoolLessBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("BoolLess batch", width, expected, op._results, &xs, &ys);
}

bool testContainer(int width) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        for (auto [a, b] : std::array<std::pair<int64_t, int64_t>, 3>{{{x, y}, {y, x}, {x, x}}}) {
            const int task = System::nextTask();
            const BoolSecret as = shareBool(a, width, task);
            const BoolSecret bs = shareBool(b, width, task);
            passed =
                checkSingle("BoolSecret::lessThan", width, a, b, unsignedLess(a, b, width), openBit(as.lessThan(bs))) &&
                passed;
        }
    }
    return passed;
}

// BitSecret::lessThan over all four operand combinations (true only for 0 < 1).
bool testBitContainer() {
    bool passed = true;
    for (int bits = 0; bits < 4; ++bits) {
        const bool x = bits & 1, y = bits & 2;
        const int task = System::nextTask();
        const BitSecret xs = shareBit(x, task);
        const BitSecret ys = shareBit(y, task);
        passed = checkSingle("BitSecret::lessThan", 1, x, y, !x && y, openBit(xs.lessThan(ys))) && passed;
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 500;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 5>{1, 2, 8, 32, 64}) {
        passed = testContainer(width) && passed;
    }
    passed = testBitContainer() && passed;

    const int status = finish("BoolLess", passed);
    System::finalize();
    return status;
}
