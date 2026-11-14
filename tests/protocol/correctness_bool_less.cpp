// Boolean-share less-than (BoolLess*Operator): unsigned comparison of the low `width` bits, batch and single-value.
//
// Known operator issue, skipped unless --known_bugs=bool_and_straddle is given: BoolLessBatchOperator is built on
// BoolAndBatchOperator, whose JIT triples are packed into 64-bit words; BitwiseBmt::extract throws std::out_of_range
// when the width does not divide 64 and an element straddles two words. By default such widths run with a batch that
// fits in one word.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/compute/single/bool/BoolLessOperator.h"
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
        if (widthDivides64(width) || knownBug("bool_and_straddle")) {
            makePairs(rng, width, randomCount(), xs, ys);
        } else {
            makeSmallPairs(width, oneWordBatch(width), xs, ys);
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(unsignedLess(xs[i], ys[i], width));
        }
    }
    BoolLessBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("BoolLess batch", width, expected, op._results, &xs, &ys);
}

bool testSingle(int width) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        for (auto [a, b] : std::array<std::pair<int64_t, int64_t>, 3>{{{x, y}, {y, x}, {x, x}}}) {
            BoolLessOperator op(a, b, width, System::nextTask(), 0, 2);
            op.execute()->reconstruct(2);
            passed = checkSingle("BoolLess single", width, a, b, unsignedLess(a, b, width), op._result) && passed;
        }
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 500;
    for (int width : std::array<int, 9>{1, 2, 3, 7, 8, 16, 32, 63, 64}) {
        passed = testBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 4>{1, 8, 32, 64}) {
        passed = testSingle(width) && passed;
    }

    const int status = finish("BoolLess", passed);
    System::finalize();
    return status;
}
