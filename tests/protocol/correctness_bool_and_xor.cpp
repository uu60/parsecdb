// Boolean-share AND (bitwise Beaver triples) and XOR (local) over several widths: the batch operators (the client
// calls execute() too, which must be a no-op there), BoolSecret::and_ / xor_ on edge values, and the BitSecret
// gates (xor_, and_, or_, not_) on all operand combinations.
//
// Share widths must be powers of two from 1 to 64; the last check makes sure the batch operators reject other widths
// with std::invalid_argument and accept every supported one.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithAddBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolXorBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>
#include <stdexcept>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
bool testAndBatch(int width, int salt) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        makePairs(rng, width, randomCount(), xs, ys);
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
    op.execute()->reconstruct(2);
    return checkResults("BoolXor batch", width, expected, op._results, &xs, &ys);
}

bool testContainer(int width) {
    bool passed = true;
    for (auto [x, y] : containerPairs(width)) {
        const int task = System::nextTask();
        const BoolSecret xs = shareBool(x, width, task);
        const BoolSecret ys = shareBool(y, width, task);
        passed = checkSingle("BoolSecret::and_", width, x, y, ring(x & y, width), xs.and_(ys).reconstruct(2)._data) &&
                 passed;
        passed = checkSingle("BoolSecret::xor_", width, x, y, ring(x ^ y, width), xs.xor_(ys).reconstruct(2)._data) &&
                 passed;
    }
    return passed;
}

bool testBitContainer() {
    bool passed = true;
    for (int bits = 0; bits < 4; ++bits) {
        const bool x = bits & 1, y = bits & 2;
        const int task = System::nextTask();
        const BitSecret xs = shareBit(x, task);
        const BitSecret ys = shareBit(y, task);
        passed = checkSingle("BitSecret::xor_", 1, x, y, x != y, openBit(xs.xor_(ys))) && passed;
        passed = checkSingle("BitSecret::and_", 1, x, y, x && y, openBit(xs.and_(ys))) && passed;
        passed = checkSingle("BitSecret::or_", 1, x, y, x || y, openBit(xs.or_(ys))) && passed;
        passed = checkSingle("BitSecret::not_", 1, x, 0, !x, openBit(xs.not_())) && passed;
    }
    return passed;
}

// Constructing an operator only validates the width when no client sharing is involved, so this runs locally on
// every rank without communication.
template <typename Make> bool throwsInvalidArgument(Make make) {
    try {
        make();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool testWidthValidation() {
    bool passed = true;
    for (int width = 0; width <= 65; ++width) {
        const bool supported = width >= 1 && width <= 64 && (width & (width - 1)) == 0;
        std::vector<int64_t> xs, ys;
        const bool boolThrows = throwsInvalidArgument(
            [&] { BoolXorBatchOperator op(&xs, &ys, width, 0, 0, SecureOperator::NO_CLIENT_COMPUTE); });
        const bool andThrows = throwsInvalidArgument(
            [&] { BoolAndBatchOperator op(&xs, &ys, width, 0, 0, SecureOperator::NO_CLIENT_COMPUTE); });
        const bool arithThrows = throwsInvalidArgument(
            [&] { ArithAddBatchOperator op(&xs, &ys, width, 0, 0, SecureOperator::NO_CLIENT_COMPUTE); });
        const bool singleThrows =
            throwsInvalidArgument([&] { BoolBatchOperator op(xs, width, 0, 0, SecureOperator::NO_CLIENT_COMPUTE); });
        if (boolThrows == supported || andThrows == supported || arithThrows == supported ||
            singleThrows == supported) {
            Log::e("width validation MISMATCH width={} supported={} threw: BoolXor={} BoolAnd={} ArithAdd={} "
                   "BoolBatch={}",
                   width, supported, boolThrows, andThrows, arithThrows, singleThrows);
            passed = false;
        }
    }
    if (passed) {
        Log::i("width validation ok (widths 0..65; only 1, 2, 4, 8, 16, 32, 64 accepted)");
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 400;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testAndBatch(width, ++salt) && passed;
        passed = testXorBatch(width, ++salt) && passed;
    }
    for (int width : std::array<int, 6>{1, 2, 4, 8, 32, 64}) {
        passed = testContainer(width) && passed;
    }
    passed = testBitContainer() && passed;
    passed = testWidthValidation() && passed;

    const int status = finish("BoolAndXor", passed);
    System::finalize();
    return status;
}
