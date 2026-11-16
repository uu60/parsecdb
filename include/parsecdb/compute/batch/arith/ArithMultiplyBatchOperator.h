
#ifndef ARITHMULTIPLYBATCHOPERATOR_H
#define ARITHMULTIPLYBATCHOPERATOR_H

#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/intermediate/item/Bmt.h"

namespace parsecdb {

class ArithMultiplyBatchOperator : public ArithBatchOperator {
  private:
    std::vector<Bmt>* _bmts{};

  public:
    ArithMultiplyBatchOperator(std::vector<int64_t>* xs, std::vector<int64_t>* ys, int width, int taskTag,
                               int msgTagOffset, int clientRank);

    ArithMultiplyBatchOperator* execute() override;

    ArithMultiplyBatchOperator* reconstruct(int clientRank) override;

    // Uses these BMTs (bmtCount(n) of them, consumed by execute()) instead of obtaining them itself. Callers that run
    // operators in parallel with background BMTs poll them in a fixed order on one thread and pass them in here.
    ArithMultiplyBatchOperator* setBmts(std::vector<Bmt>* bmts);

    [[nodiscard]] static int bmtCount(int num);

    [[nodiscard]] static int tagStride(int width);
};

} // namespace parsecdb

#endif
