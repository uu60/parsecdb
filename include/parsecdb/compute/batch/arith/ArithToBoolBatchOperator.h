
#ifndef ARITHTOBOOLBATCHOPERATOR_H
#define ARITHTOBOOLBATCHOPERATOR_H
#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/intermediate/item/BitwiseBmt.h"

namespace parsecdb {

class ArithToBoolBatchOperator : public ArithBatchOperator {
  private:
    std::vector<BitwiseBmt>* _bmts{};

  public:
    inline static std::atomic_int64_t _totalTime = 0;

  public:
    ArithToBoolBatchOperator(std::vector<int64_t>* xs, int width, int taskTag, int msgTagOffset, int clientRank);

    ~ArithToBoolBatchOperator() override;

    ArithToBoolBatchOperator* execute() override;

    ArithToBoolBatchOperator* reconstruct(int clientRank) override;

    // Background BMTs for this conversion (bmtCount(n, width) of them, consumed by execute()). Without them, execute()
    // polls the background queue itself.
    ArithToBoolBatchOperator* setBmts(std::vector<BitwiseBmt>* bmts);

    [[nodiscard]] static int bmtCount(int num, int width);

    static int tagStride(int width);
};

} // namespace parsecdb

#endif
