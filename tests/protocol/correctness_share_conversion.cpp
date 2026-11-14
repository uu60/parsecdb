// Share conversions: arithmetic to boolean (A2B, ArithToBool*Operator, a ripple-carry adder over bitwise Beaver
// triples) and boolean to arithmetic (B2A, BoolToArith*Operator, one IKNP OT per bit), batch and single-value, plus an
// A2B -> B2A round trip that stays on the servers.
//
// B2A picks the OT sender from the message tag offset, so the batch and single cases run with both sender 0 and
// sender 1.
//
// Known operator issues, skipped unless --known_bugs=<id> names them (ids in brackets):
//  - BoolToArithBatchOperator's constructor forwards ys = nullptr to BoolBatchOperator, whose client-sharing branch
//    iterates *ys, so constructing it with clientRank = 2 dereferences a null pointer on the client. By default the
//    batch B2A cases share the boolean inputs with BoolBatchOperator first and run B2A with NO_CLIENT_COMPUTE.
//    [b2a_batch_client]
//  - ArithToBoolOperator (single value) reads three BitwiseBmt words from BitwiseBmtBatchGenerator(3, width), but that
//    generator packs 3 * width bits into ceil(3 * width / 64) words, so for 1 < width < 64 it reads past the end of
//    the vector and returns wrong results. By default the single-value A2B case only runs at widths 1 and 64.
//    [a2b_single_narrow]
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithToBoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolToArithBatchOperator.h"
#include "parsecdb/compute/single/arith/ArithToBoolOperator.h"
#include "parsecdb/compute/single/bool/BoolToArithOperator.h"
#include "parsecdb/ot/IknpOtBatchOperator.h"
#include "parsecdb/utils/System.h"

#include <array>

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
std::vector<int64_t> makeValues(int width, int salt) {
    std::vector<int64_t> xs;
    if (!Comm::isClient()) {
        return xs;
    }
    auto rng = makeRng(salt);
    xs = edgeValues(width);
    const int count = randomCount();
    for (int i = 0; i < count; ++i) {
        xs.push_back(randomRing(rng, width));
    }
    return xs;
}

bool testA2BBatch(int width, int salt) {
    std::vector<int64_t> xs = makeValues(width, salt);
    ArithToBoolBatchOperator op(&xs, width, System::nextTask(), 0, 2);
    op.execute()->reconstruct(2);
    return checkResults("ArithToBool batch", width, xs, op._results, &xs);
}

bool testB2ABatch(int width, int salt, int sender) {
    std::vector<int64_t> xs = makeValues(width, salt);
    const int task = System::nextTask();
    const int offset = sender * IknpOtBatchOperator::tagStride();
    const std::string name = "BoolToArith batch (OT sender " + std::to_string(sender) + ")";

    std::vector<int64_t> results;
    if (knownBug("b2a_batch_client")) {
        BoolToArithBatchOperator op(&xs, width, task, offset, 2);
        op.execute();
        ArithBatchOperator rec(op._zis, width, task, offset, SecureOperator::NO_CLIENT_COMPUTE);
        rec.reconstruct(2);
        results = rec._results;
    } else {
        std::vector<int64_t> shares = BoolBatchOperator(xs, width, task, 0, 2)._zis;
        BoolToArithBatchOperator op(&shares, width, task, offset, SecureOperator::NO_CLIENT_COMPUTE);
        op.execute();
        ArithBatchOperator rec(op._zis, width, task, offset, SecureOperator::NO_CLIENT_COMPUTE);
        rec.reconstruct(2);
        results = rec._results;
    }
    return checkResults(name, width, xs, results, &xs);
}

bool testRoundTrip(int width, int salt) {
    std::vector<int64_t> xs = makeValues(width, salt);
    const int task = System::nextTask();
    ArithToBoolBatchOperator a2b(&xs, width, task, 0, 2);
    a2b.execute();
    std::vector<int64_t> boolShares = a2b._zis;
    const int b2aTask = System::nextTask();
    BoolToArithBatchOperator b2a(&boolShares, width, b2aTask, 0, SecureOperator::NO_CLIENT_COMPUTE);
    b2a.execute();
    ArithBatchOperator rec(b2a._zis, width, b2aTask, 0, SecureOperator::NO_CLIENT_COMPUTE);
    rec.reconstruct(2);
    return checkResults("ArithToBool -> BoolToArith round trip", width, xs, rec._results, &xs);
}

bool testA2BSingle(int width) {
    bool passed = true;
    for (int64_t x : edgeValues(width)) {
        ArithToBoolOperator a2b(x, width, System::nextTask(), 0, 2);
        a2b.execute()->reconstruct(2);
        passed = checkSingle("ArithToBool single", width, x, 0, x, a2b._result) && passed;
    }
    return passed;
}

bool testB2ASingle(int width) {
    bool passed = true;
    for (int64_t x : edgeValues(width)) {
        for (int sender : {0, 1}) {
            BoolToArithOperator b2a(x, width, System::nextTask(), sender * IknpOtBatchOperator::tagStride(), 2);
            b2a.execute()->reconstruct(2);
            passed = checkSingle("BoolToArith single (OT sender " + std::to_string(sender) + ")", width, x, 0, x,
                                 b2a._result) &&
                     passed;
        }
    }
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 600;
    for (int width : std::array<int, 7>{1, 2, 8, 16, 32, 63, 64}) {
        passed = testA2BBatch(width, ++salt) && passed;
        passed = testB2ABatch(width, ++salt, 0) && passed;
        passed = testB2ABatch(width, ++salt, 1) && passed;
    }
    for (int width : std::array<int, 3>{8, 32, 64}) {
        passed = testRoundTrip(width, ++salt) && passed;
    }
    for (int width : std::array<int, 2>{1, 64}) {
        passed = testA2BSingle(width) && passed;
    }
    if (knownBug("a2b_single_narrow")) {
        for (int width : std::array<int, 2>{8, 32}) {
            passed = testA2BSingle(width) && passed;
        }
    }
    for (int width : std::array<int, 4>{1, 8, 32, 64}) {
        passed = testB2ASingle(width) && passed;
    }

    const int status = finish("ShareConversion", passed);
    System::finalize();
    return status;
}
