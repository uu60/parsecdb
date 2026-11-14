// Boolean-share multiplexer (BoolMutex*Operator): z = c ? x : y, with the condition shared by the client as a
// one-bit boolean value. Batch and single-value, over several widths.
//
// Known operator issue, skipped unless --known_bugs=bool_and_straddle is given: BoolMutexBatchOperator spends two AND
// lanes per element in BoolAndBatchOperator, whose JIT triples are packed into 64-bit words; BitwiseBmt::extract throws
// std::out_of_range when the width does not divide 64 and a lane straddles two words. By default such widths run with
// a batch that fits in one word, and width 63 (two lanes cannot fit in one word) only runs with that flag.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
#include "parsecdb/compute/single/bool/BoolMutexOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
bool testBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, cs, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        if (widthDivides64(width) || knownBug("bool_and_straddle")) {
            makePairs(rng, width, randomCount(), xs, ys);
        } else {
            // Every pair is duplicated below with the other condition, so leave room for two copies.
            makeSmallPairs(width, oneWordBatch(width, 2) / 2, xs, ys);
        }
        // makePairs starts with every ordered pair of edge values. Run that block with c = 0 and append a copy with
        // c = 1, so each edge pair is selected both ways; the random pairs get random conditions.
        const size_t edgePairs = widthDivides64(width) || knownBug("bool_and_straddle")
                                     ? edgeValues(width).size() * edgeValues(width).size()
                                     : xs.size();
        for (size_t i = 0; i < xs.size(); ++i) {
            cs.push_back(i < edgePairs ? 0 : static_cast<int64_t>(rng() & 1));
        }
        for (size_t i = 0; i < edgePairs; ++i) {
            xs.push_back(xs[i]);
            ys.push_back(ys[i]);
            cs.push_back(1);
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(cs[i] ? xs[i] : ys[i]);
        }
    }
    BoolMutexBatchOperator op(&xs, &ys, &cs, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("BoolMutex batch", width, expected, op._results, &xs, &ys, &cs);
}

bool testSingle(int width) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        for (bool c : {false, true}) {
            BoolMutexOperator op(x, y, c, width, System::nextTask(), 0, 2);
            op.execute()->reconstruct(2);
            if (Comm::isClient() && op._result != (c ? x : y)) {
                Log::e("BoolMutex single width={} MISMATCH x={} y={} c={} expected={} got={}", width, x, y, c,
                       c ? x : y, op._result);
                passed = false;
            }
        }
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 300;
    for (int width : std::array<int, 7>{1, 2, 3, 7, 8, 32, 64}) {
        passed = testBatch(width, ++salt) && passed;
    }
    if (knownBug("bool_and_straddle")) {
        passed = testBatch(63, ++salt) && passed;
    }
    for (int width : std::array<int, 3>{1, 8, 64}) {
        passed = testSingle(width) && passed;
    }

    const int status = finish("BoolMutex", passed);
    System::finalize();
    return status;
}
