// Boolean-share AND (bitwise Beaver triples) and XOR (local), batch and single-value, over several widths.
//
// Known operator issues, skipped unless --known_bugs=<id> names them (ids in brackets):
//  - BoolXorBatchOperator::execute() does not return early on the client, whose _xis/_yis are null when the client
//    supplied the inputs, so calling execute() on rank 2 dereferences a null pointer. By default the client skips
//    execute() for XOR. [xor_batch_client]
//  - BoolAndBatchOperator (JIT triples) throws std::out_of_range from BitwiseBmt::extract when the width does not
//    divide 64 and the batch spans more than one 64-bit word (an element straddles two words). By default such widths
//    run with a batch that fits in one word. [bool_and_straddle]
//  - BoolAndOperator::execute() (single value) has no client guard: on rank 2 it builds a triple and calls the
//    server-to-server exchange, which crashes the client with SIGSEGV. By default the client skips execute().
//    [and_single_client]
//  - BoolXorOperator takes its operands as `bool`, so any input wider than one bit is collapsed to 0/1 before it is
//    shared. By default the single-value XOR case only runs at width 1. [xor_single_wide]
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolXorBatchOperator.h"
#include "parsecdb/compute/single/bool/BoolAndOperator.h"
#include "parsecdb/compute/single/bool/BoolXorOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
bool testAndBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        if (widthDivides64(width) || knownBug("bool_and_straddle")) {
            makePairs(rng, width, randomCount(), xs, ys);
        } else {
            makeSmallPairs(width, oneWordBatch(width), xs, ys);
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(ring(xs[i] & ys[i], width));
        }
    }
    BoolAndBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("BoolAnd batch", width, expected, op._results, &xs, &ys);
}

bool testXorBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        makePairs(rng, width, randomCount(), xs, ys);
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(ring(xs[i] ^ ys[i], width));
        }
    }
    BoolXorBatchOperator op(&xs, &ys, width, System::nextTask(), 0, 2);
    if (Comm::isServer() || knownBug("xor_batch_client")) {
        op.execute();
    }
    op.reconstruct(2);
    return checkResults("BoolXor batch", width, expected, op._results, &xs, &ys);
}

bool testAndSingle(int width) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        BoolAndOperator op(x, y, width, System::nextTask(), 0, 2);
        if (Comm::isServer() || knownBug("and_single_client")) {
            op.execute();
        }
        op.reconstruct(2);
        passed = checkSingle("BoolAnd single", width, x, y, ring(x & y, width), op._result) && passed;
    }
    return passed;
}

bool testXorSingle(int width) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        BoolXorOperator op(x, y, width, System::nextTask(), 0, 2);
        op.execute()->reconstruct(2);
        passed = checkSingle("BoolXor single", width, x, y, ring(x ^ y, width), op._result) && passed;
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 400;
    for (int width : std::array<int, 9>{1, 2, 3, 7, 8, 16, 32, 63, 64}) {
        passed = testAndBatch(width, ++salt) && passed;
        passed = testXorBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 3>{1, 8, 64}) {
        passed = testAndSingle(width) && passed;
    }
    passed = testXorSingle(1) && passed;
    if (knownBug("xor_single_wide")) {
        for (int width : std::array<int, 2>{8, 64}) {
            passed = testXorSingle(width) && passed;
        }
    }

    const int status = finish("BoolAndXor", passed);
    System::finalize();
    return status;
}
