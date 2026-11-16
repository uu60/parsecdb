// IKNP OT extension between the two servers, with each server as the sender:
//   - word mode: 2048 OTs of 32-bit and 64-bit messages,
//   - packed-bit mode: 16384 one-bit OTs packed into 64-bit limbs.
// The sender checks what the receiver got against its own messages and the receiver's choices. It also checks the
// IKNP set-up: the sender's choice bits never reach the receiver, and the 128-bit base seeds use their high halves.
// Prints FAIL or MISMATCH and exits non-zero on any error.

#include "parsecdb/comm/Comm.h"
#include "parsecdb/intermediate/IntermediateDataSupport.h"
#include "parsecdb/ot/IknpOtBatchOperator.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"
#include "parsecdb/utils/System.h"

#include <cstdint>
#include <vector>

using namespace parsecdb;

namespace {

// Message tags of this test: the receiver reports its choices and results to the sender on helper tags, apart from
// the OT's own range.
int helperTag(int task, int k) { return task * 1000 + k; }

bool testWords(int sender, int width, int task) {
    const int n = 2048;
    const bool isSender = Comm::rank() == sender;
    std::vector<int64_t> ms0, ms1, choices64;
    std::vector<int> choices;
    if (isSender) {
        ms0.resize(n);
        ms1.resize(n);
        for (int i = 0; i < n; ++i) {
            ms0[i] = Math::ring(Math::randBits(), width);
            ms1[i] = Math::ring(Math::randBits(), width);
        }
    } else {
        choices.resize(n);
        choices64.resize(n);
        for (int i = 0; i < n; ++i) {
            choices[i] = static_cast<int>(Math::randInt(0, 1));
            choices64[i] = choices[i];
        }
    }

    IknpOtBatchOperator op(sender, isSender ? &ms0 : nullptr, isSender ? &ms1 : nullptr, isSender ? nullptr : &choices,
                           width, task, helperTag(task, 10));
    op.execute();

    if (!isSender) {
        Comm::send(choices64, 1, sender, helperTag(task, 1));
        Comm::send(op._results, width, sender, helperTag(task, 2));
        return true;
    }
    std::vector<int64_t> peerChoices, got;
    Comm::receive(peerChoices, 1, 1 - sender, helperTag(task, 1));
    Comm::receive(got, width, 1 - sender, helperTag(task, 2));
    int mismatches = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t expected = peerChoices[i] ? ms1[i] : ms0[i];
        if (Math::ring(got[i], width) != expected && ++mismatches <= 10) {
            Log::e("MISMATCH words sender={} width={} i={} expected={} got={}", sender, width, i, expected, got[i]);
        }
    }
    Log::i("[IKNP words sender={} width={}] {}", sender, width, mismatches == 0 ? "ok" : "FAIL");
    return mismatches == 0;
}

bool testPackedBits(int sender, int task) {
    const int limbs = 256; // 256 limbs * 64 bits = 16384 OTs
    const bool isSender = Comm::rank() == sender;
    std::vector<int64_t> bits0, bits1, choices;
    if (isSender) {
        bits0.resize(limbs);
        bits1.resize(limbs);
        for (int i = 0; i < limbs; ++i) {
            bits0[i] = Math::randBits();
            bits1[i] = Math::randBits();
        }
    } else {
        choices.resize(limbs);
        for (int i = 0; i < limbs; ++i) {
            choices[i] = Math::randBits();
        }
    }

    IknpOtBatchOperator op(sender, isSender ? &bits0 : nullptr, isSender ? &bits1 : nullptr,
                           isSender ? nullptr : &choices, task, helperTag(task, 10));
    op.execute();

    if (!isSender) {
        Comm::send(choices, 64, sender, helperTag(task, 1));
        Comm::send(op._results, 64, sender, helperTag(task, 2));
        return true;
    }
    std::vector<int64_t> peerChoices, got;
    Comm::receive(peerChoices, 64, 1 - sender, helperTag(task, 1));
    Comm::receive(got, 64, 1 - sender, helperTag(task, 2));
    int mismatches = 0;
    for (int limb = 0; limb < limbs; ++limb) {
        const auto c = static_cast<uint64_t>(peerChoices[limb]);
        const uint64_t expected = (static_cast<uint64_t>(bits0[limb]) & ~c) | (static_cast<uint64_t>(bits1[limb]) & c);
        if (static_cast<uint64_t>(got[limb]) != expected && ++mismatches <= 10) {
            Log::e("MISMATCH bits sender={} limb={} expected={} got={}", sender, limb, expected,
                   static_cast<uint64_t>(got[limb]));
        }
    }
    Log::i("[IKNP packed bits sender={}] {}", sender, mismatches == 0 ? "ok" : "FAIL");
    return mismatches == 0;
}

// The IKNP sender's choice bits s must stay on the sender: a receiver that learned s could recover both of the
// sender's messages. Direction d has rank d as the IKNP sender.
bool testSetup() {
    bool ok = true;
    for (int direction = 0; direction < 2; ++direction) {
        const auto& choices = direction == 0 ? IntermediateDataSupport::_iknpSenderChoices0
                                             : IntermediateDataSupport::_iknpSenderChoices1;
        const auto& seeds =
            direction == 0 ? IntermediateDataSupport::_iknpBaseSeeds0 : IntermediateDataSupport::_iknpBaseSeeds1;
        if (Comm::rank() != direction && !choices.empty()) {
            Log::e("FAIL: the IKNP receiver holds the sender's choice bits (direction {})", direction);
            ok = false;
        }
        bool anyHigh = false;
        for (const auto& pair : seeds) {
            anyHigh |= pair[0].hi != 0;
        }
        if (!anyHigh) {
            Log::e("FAIL: IKNP base seeds have no high 64 bits (direction {})", direction);
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    System::init(argc, argv);
    bool ok = true;
    if (Comm::isServer()) {
        const int task = System::nextTask();
        int round = 0;
        for (int sender = 0; sender < 2; ++sender) {
            for (int width : {32, 64}) {
                ok &= testWords(sender, width, task + ++round);
            }
            ok &= testPackedBits(sender, task + ++round);
        }
        ok &= testSetup();
        Log::i("[IKNP OT] {}", ok ? "PASS" : "FAIL");
    }
    System::finalize();
    return ok ? 0 : 1;
}
