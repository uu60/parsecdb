// Arithmetic-share addition and multiplication (Beaver triples) over several ring widths: the batch operators, and
// ArithSecret::add / mul (one-element batches) on edge values.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithAddBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithMultiplyBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
template <typename Op>
bool testBatch(const std::string& name, int width, int salt, int64_t (*plain)(int64_t, int64_t, int)) {
    std::vector<int64_t> xs, ys, expected;
    if (Comm::isClient()) {
        auto rng = makeRng(salt);
        makePairs(rng, width, randomCount(), xs, ys);
        for (size_t i = 0; i < xs.size(); ++i) {
            expected.push_back(plain(xs[i], ys[i], width));
        }
    }
    Op op(&xs, &ys, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults(name, width, expected, op._results, &xs, &ys);
}

// ArithSecret::add / mul on one shared value each.
bool testContainer(int width, int64_t (*plainAddFn)(int64_t, int64_t, int),
                   int64_t (*plainMulFn)(int64_t, int64_t, int)) {
    bool passed = true;
    for (auto [x, y] : containerPairs(width)) {
        const int task = System::nextTask();
        const ArithSecret xs = shareArith(x, width, task);
        const ArithSecret ys = shareArith(y, width, task);
        passed =
            checkSingle("ArithSecret::add", width, x, y, plainAddFn(x, y, width), xs.add(ys).reconstruct(2)._data) &&
            passed;
        passed =
            checkSingle("ArithSecret::mul", width, x, y, plainMulFn(x, y, width), xs.mul(ys).reconstruct(2)._data) &&
            passed;
    }
    return passed;
}

int64_t plainAdd(int64_t x, int64_t y, int width) { return Math::ringAdd(x, y, width); }

int64_t plainMultiply(int64_t x, int64_t y, int width) {
    return ring(static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)), width);
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 0;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testBatch<ArithAddBatchOperator>("ArithAdd batch", width, ++salt, plainAdd) && passed;
        passed = testBatch<ArithMultiplyBatchOperator>("ArithMultiply batch", width, ++salt, plainMultiply) && passed;
    }
    for (int width : std::array<int, 5>{1, 2, 8, 32, 64}) {
        passed = testContainer(width, plainAdd, plainMultiply) && passed;
    }

    const int status = finish("ArithAddMultiply", passed);
    System::finalize();
    return status;
}
