// Arithmetic-share addition and multiplication (Beaver triples), batch and single-value, over several ring widths.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithAddBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithMultiplyBatchOperator.h"
#include "parsecdb/compute/single/arith/ArithAddOperator.h"
#include "parsecdb/compute/single/arith/ArithMultiplyOperator.h"
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

template <typename Op> bool testSingle(const std::string& name, int width, int64_t (*plain)(int64_t, int64_t, int)) {
    bool passed = true;
    const auto edges = edgeValues(width);
    for (size_t i = 0; i < edges.size(); ++i) {
        const int64_t x = edges[i];
        const int64_t y = edges[(i * 3 + 1) % edges.size()];
        Op op(x, y, width, System::nextTask(), 0, 2);
        op.execute()->reconstruct(2);
        passed = checkSingle(name, width, x, y, plain(x, y, width), op._result) && passed;
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
    for (int width : std::array<int, 7>{1, 2, 8, 16, 32, 63, 64}) {
        passed = testBatch<ArithAddBatchOperator>("ArithAdd batch", width, ++salt, plainAdd) && passed;
        passed = testBatch<ArithMultiplyBatchOperator>("ArithMultiply batch", width, ++salt, plainMultiply) && passed;
    }
    for (int width : std::array<int, 3>{1, 8, 64}) {
        passed = testSingle<ArithAddOperator>("ArithAdd single", width, plainAdd) && passed;
        passed = testSingle<ArithMultiplyOperator>("ArithMultiply single", width, plainMultiply) && passed;
    }

    const int status = finish("ArithAddMultiply", passed);
    System::finalize();
    return status;
}
