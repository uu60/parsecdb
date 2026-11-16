// Base OT (BaseOtBatchOperator, Chou-Orlandi over P-256) between the two servers: a batch of one, a larger batch with
// random 64-bit messages and choices, and the 128-bit mode (withHighWords). Every case runs with each server as the
// sender. The receiver checks the received messages against the sender's, which both servers derive from the same
// seeded generator.
#include "ProtocolTestSupport.h"

#include "parsecdb/ot/BaseOtBatchOperator.h"
#include "parsecdb/utils/System.h"

using namespace parsecdb;
using namespace parsecdb::protocol_test;

namespace {
bool testBaseOt(int sender, int n, bool wide, int salt) {
    // Both servers generate the same messages and choices, so the receiver can compute the expected outputs.
    auto rng = makeRng(salt);
    std::vector<int64_t> m0(n), m1(n), m0Hi(n), m1Hi(n);
    std::vector<int> choices(n);
    for (int i = 0; i < n; ++i) {
        m0[i] = static_cast<int64_t>(rng());
        m1[i] = static_cast<int64_t>(rng());
        m0Hi[i] = static_cast<int64_t>(rng());
        m1Hi[i] = static_cast<int64_t>(rng());
        choices[i] = static_cast<int>(rng() & 1);
    }
    if (n >= 4) {
        // Edge messages: all zeros and all ones, and equal messages.
        m0[0] = 0;
        m1[0] = -1;
        m0[1] = -1;
        m1[1] = 0;
        m1[2] = m0[2];
        choices[0] = 0;
        choices[1] = 1;
    }

    const int task = System::nextTask();
    const bool isSender = Comm::rank() == sender;
    BaseOtBatchOperator op(sender, isSender ? &m0 : nullptr, isSender ? &m1 : nullptr, isSender ? nullptr : &choices,
                           64, task, 0);
    if (wide) {
        op.withHighWords(isSender ? &m0Hi : nullptr, isSender ? &m1Hi : nullptr);
    }
    op.execute();
    if (isSender) {
        return true;
    }

    const std::string name = std::string("BaseOtBatch") + (wide ? " 128-bit" : "") + " n=" + std::to_string(n) +
                             " sender=" + std::to_string(sender);
    if (op._results.size() != static_cast<size_t>(n) || (wide && op._resultsHi.size() != static_cast<size_t>(n))) {
        Log::e("{} MISMATCH result size: expected={} got={} (high words {})", name, n, op._results.size(),
               op._resultsHi.size());
        return false;
    }
    int mismatches = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t expected = choices[i] ? m1[i] : m0[i];
        const int64_t expectedHi = choices[i] ? m1Hi[i] : m0Hi[i];
        if (op._results[i] != expected || (wide && op._resultsHi[i] != expectedHi)) {
            if (++mismatches <= 10) {
                Log::e("{} MISMATCH index={} choice={} expected={}:{} got={}:{}", name, i, choices[i], expectedHi,
                       expected, wide ? op._resultsHi[i] : 0, op._results[i]);
            }
        }
    }
    if (mismatches > 0) {
        Log::e("{} FAIL mismatches={}/{}", name, mismatches, n);
        return false;
    }
    Log::i("{} ok", name);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool passed = true;

    // The client takes no part in base OT, but it still calls nextTask() so all ranks agree on task tags.
    int salt = 700;
    for (int sender : {0, 1}) {
        for (bool wide : {false, true}) {
            for (int n : {1, 300}) {
                ++salt;
                if (Comm::isClient()) {
                    System::nextTask();
                    continue;
                }
                passed = testBaseOt(sender, n, wide, salt) && passed;
            }
        }
    }

    int status = passed ? 0 : 1;
    if (Comm::rank() == 0 || Comm::rank() == 1) {
        // Each server reports for the OTs it received.
        Log::i(passed ? "[Base OT correctness] PASS" : "[Base OT correctness] FAIL");
    }
    System::finalize();
    return status;
}
