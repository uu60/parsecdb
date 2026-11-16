// Boolean-share multiplexer (BoolMutex*Operator): z = c ? x : y, with the condition shared by the client as a
// one-bit boolean value, over several widths; plus BoolSecret::mux and BitSecret::mux (one-element batches).
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
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
    BoolMutexBatchOperator op(&xs, &ys, &cs, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("BoolMutex batch", width, expected, op._results, &xs, &ys, &cs);
}

// BoolSecret::mux with a BitSecret condition: x.mux(y, c) = c ? x : y.
bool testContainer(int width) {
    bool passed = true;
    for (auto [x, y] : containerPairs(width)) {
        for (bool c : {false, true}) {
            const int task = System::nextTask();
            const BoolSecret xs = shareBool(x, width, task);
            const BoolSecret ys = shareBool(y, width, task);
            const BitSecret cs = shareBit(c, task);
            const int64_t got = xs.mux(ys, cs).reconstruct(2)._data;
            if (Comm::isClient() && got != (c ? x : y)) {
                Log::e("BoolSecret::mux width={} MISMATCH x={} y={} c={} expected={} got={}", width, x, y, c, c ? x : y,
                       got);
                passed = false;
            }
        }
    }
    return passed;
}

// BitSecret::mux over all eight (x, y, c) combinations.
bool testBitContainer() {
    bool passed = true;
    for (int bits = 0; bits < 8; ++bits) {
        const bool x = bits & 1, y = bits & 2, c = bits & 4;
        const int task = System::nextTask();
        const BitSecret xs = shareBit(x, task);
        const BitSecret ys = shareBit(y, task);
        const BitSecret cs = shareBit(c, task);
        passed =
            checkSingle("BitSecret::mux c=" + std::to_string(c), 1, x, y, c ? x : y, openBit(xs.mux(ys, cs))) && passed;
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 300;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 5>{1, 2, 8, 32, 64}) {
        passed = testContainer(width) && passed;
    }
    passed = testBitContainer() && passed;

    const int status = finish("BoolMutex", passed);
    System::finalize();
    return status;
}
