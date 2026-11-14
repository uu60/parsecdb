#ifndef MPC_PACKAGE_BASEOTOPERATOR_H
#define MPC_PACKAGE_BASEOTOPERATOR_H

#include "parsecdb/ot/base/AbstractOtOperator.h"

namespace parsecdb {

// Single base OT. Runs BaseOtBatchOperator (Chou-Orlandi over P-256) with one message pair.
class BaseOtOperator : public AbstractOtOperator {
  public:
    explicit BaseOtOperator(int sender, int64_t m0, int64_t m1, int choice, int l, int taskTag, int msgTagOffset);

    BaseOtOperator* execute() override;

    [[nodiscard]] static int tagStride();
};

} // namespace parsecdb

#endif
