#ifndef BASEOTBATCHOPERATOR_H
#define BASEOTBATCHOPERATOR_H

#include "parsecdb/ot/base/AbstractOtBatchOperator.h"

namespace parsecdb {

// Base OT between the two servers, following the Chou-Orlandi "simplest OT" over NIST P-256:
//
//   sender:   a <- Z_q, A = aG                       --A-->
//   receiver: b_i <- Z_q, B_i = b_i G + c_i A         <--B_i--
//   sender:   k_i^0 = H(i, A, B_i, a B_i), k_i^1 = H(i, A, B_i, a (B_i - A)), sends m_i^j XOR k_i^j
//   receiver: k_i^{c_i} = H(i, A, B_i, b_i A)
//
// H is SHA-256 with domain separation, truncated to 128 bits. A fresh A is drawn on every execute(), so keys are never
// reused across batches. The construction is secure against semi-honest parties under the CDH assumption in the random
// oracle model, which matches Parsec's security model (see docs/security-model.md).
class BaseOtBatchOperator : public AbstractOtBatchOperator {
  private:
    // 128-bit message mode (see withHighWords).
    bool _wide = false;
    std::vector<int64_t>* _ms0Hi{};
    std::vector<int64_t>* _ms1Hi{};

  public:
    inline static std::atomic_int64_t _totalTime = 0;

    // High 64-bit words of the received messages in 128-bit mode (receiver only).
    std::vector<int64_t> _resultsHi{};

    explicit BaseOtBatchOperator(int sender, std::vector<int64_t>* ms0, std::vector<int64_t>* ms1,
                                 std::vector<int>* choices, int width, int taskTag, int msgTagOffset);

    // Switches to 128-bit messages: each OT also transfers ms0Hi[i] / ms1Hi[i], masked by the second half of the same
    // 128-bit key. Both parties must call it before execute(); the receiver passes nullptrs and reads the high words
    // from _resultsHi.
    BaseOtBatchOperator* withHighWords(std::vector<int64_t>* ms0Hi, std::vector<int64_t>* ms1Hi);

    BaseOtBatchOperator* execute() override;

    [[nodiscard]] static int tagStride();

  private:
    void runSender();

    void runReceiver();
};

} // namespace parsecdb

#endif
