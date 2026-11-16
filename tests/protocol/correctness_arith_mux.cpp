// Arithmetic-share multiplexer (ArithMutex*Operator): z = c ? x : y, with the condition bit shared by the client as
// an arithmetic 0/1 value, over several ring widths; plus ArithSecret::mux (one-element batches, BitSecret condition).
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithMutexBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
bool testBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, cs, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        makePairs(rng, width, randomCount(), xs, ys);
        // makePairs starts with every ordered pair of edge values. Run that block with c = 0 and append a copy with
        // c = 1, so each edge pair is selected both ways; the random pairs get random conditions.
        const size_t edgePairs = edgeValues(width).size() * edgeValues(width).size();
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
    ArithMutexBatchOperator op(&xs, &ys, &cs, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("ArithMutex batch", width, expected, op._results, &xs, &ys, &cs);
}

// ArithSecret::mux with a BitSecret condition: x.mux(y, c) = c ? x : y.
bool testContainer(int width) {
    bool passed = true;
    for (auto [x, y] : containerPairs(width)) {
        for (bool c : {false, true}) {
            const int task = System::nextTask();
            const ArithSecret xs = shareArith(x, width, task);
            const ArithSecret ys = shareArith(y, width, task);
            const BitSecret cs = shareBit(c, task);
            const int64_t got = xs.mux(ys, cs).reconstruct(2)._data;
            if (Comm::isClient() && got != (c ? x : y)) {
                Log::e("ArithSecret::mux width={} MISMATCH x={} y={} c={} expected={} got={}", width, x, y, c,
                       c ? x : y, got);
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

    int salt = 200;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 5>{1, 2, 8, 32, 64}) {
        passed = testContainer(width) && passed;
    }

    const int status = finish("ArithMutex", passed);
    System::finalize();
    return status;
}
