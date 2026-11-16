// Share conversions: arithmetic to boolean (A2B, ArithToBoolBatchOperator, a ripple-carry adder over bitwise Beaver
// triples) and boolean to arithmetic (B2A, BoolToArithBatchOperator, one IKNP OT per bit). Covers the batch operators
// with client-shared inputs, B2A on inputs that are already boolean shares, an A2B -> B2A round trip that stays on
// the servers, and the container conversions ArithSecret::boolean / BoolSecret::arithmetic (one-element batches).
//
// B2A picks the OT sender from the message tag offset, so the B2A cases run with both sender 0 and sender 1.
//
// The containers keep the converted share in the same container type: ArithSecret::boolean() returns an ArithSecret
// holding a boolean share, and BoolSecret::arithmetic() a BoolSecret holding an arithmetic share, so the results are
// opened with the other container's reconstruct.
#include "ProtocolTestSupport.h"

#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithToBoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolToArithBatchOperator.h"
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

    // Inputs shared by the client through the operator itself.
    BoolToArithBatchOperator op(&xs, width, task, offset, 2);
    op.execute();
    ArithBatchOperator rec(op._zis, width, task, offset, SecureOperator::NO_CLIENT_COMPUTE);
    rec.reconstruct(2);
    bool passed = checkResults(name, width, xs, rec._results, &xs);

    // Inputs that are already boolean shares on the servers.
    const int task2 = System::nextTask();
    std::vector<int64_t> shares = BoolBatchOperator(xs, width, task2, 0, 2)._zis;
    BoolToArithBatchOperator op2(&shares, width, task2, offset, SecureOperator::NO_CLIENT_COMPUTE);
    op2.execute();
    ArithBatchOperator rec2(op2._zis, width, task2, offset, SecureOperator::NO_CLIENT_COMPUTE);
    rec2.reconstruct(2);
    return checkResults(name + " on boolean shares", width, xs, rec2._results, &xs) && passed;
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

bool testContainers(int width) {
    bool passed = true;
    for (int64_t x : edgeValues(width)) {
        int task = System::nextTask();
        const ArithSecret a2b = shareArith(x, width, task).boolean();
        passed = checkSingle("ArithSecret::boolean", width, x, 0, x, openBool(a2b._data, width, task)) && passed;

        for (int sender : {0, 1}) {
            task = System::nextTask();
            const int offset = sender * IknpOtBatchOperator::tagStride();
            const BoolSecret b2a = BoolSecret(x, width, task, offset).share(2).arithmetic();
            passed = checkSingle("BoolSecret::arithmetic (OT sender " + std::to_string(sender) + ")", width, x, 0, x,
                                 openArith(b2a._data, width, task)) &&
                     passed;
        }

        task = System::nextTask();
        const ArithSecret boolShare = shareArith(x, width, task).boolean();
        const BoolSecret back = BoolSecret(boolShare._data, width, task, 0).arithmetic();
        passed = checkSingle("ArithSecret::boolean -> BoolSecret::arithmetic", width, x, 0, x,
                             openArith(back._data, width, task)) &&
                 passed;
    }
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    int salt = 600;
    for (int width : std::array<int, 7>{1, 2, 4, 8, 16, 32, 64}) {
        passed = testA2BBatch(width, ++salt) && passed;
        passed = testB2ABatch(width, ++salt, 0) && passed;
        passed = testB2ABatch(width, ++salt, 1) && passed;
    }
    for (int width : std::array<int, 4>{2, 8, 32, 64}) {
        passed = testRoundTrip(width, ++salt) && passed;
    }
    for (int width : std::array<int, 5>{1, 2, 8, 32, 64}) {
        passed = testContainers(width) && passed;
    }

    const int status = finish("ShareConversion", passed);
    System::finalize();
    return status;
}
