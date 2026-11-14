#include "parsecdb/ot/BaseOtOperator.h"

#include "parsecdb/comm/Comm.h"
#include "parsecdb/ot/BaseOtBatchOperator.h"
#include <vector>

namespace parsecdb {

BaseOtOperator::BaseOtOperator(int sender, int64_t m0, int64_t m1, int choice, int l, int taskTag, int msgTagOffset)
    : AbstractOtOperator(sender, m0, m1, choice, l, taskTag, msgTagOffset) {}

BaseOtOperator* BaseOtOperator::execute() {
    if (!Comm::isServer()) {
        return this;
    }
    const int sender = _isSender ? Comm::rank() : 1 - Comm::rank();
    std::vector<int64_t> ms0, ms1;
    std::vector<int> choices;
    if (_isSender) {
        ms0 = {_m0};
        ms1 = {_m1};
    } else {
        choices = {_choice};
    }
    BaseOtBatchOperator batch(sender, &ms0, &ms1, &choices, _width, _taskTag, _startMsgTag);
    batch.execute();
    if (!_isSender) {
        _result = batch._results[0];
    }
    return this;
}

int BaseOtOperator::tagStride() { return BaseOtBatchOperator::tagStride(); }

} // namespace parsecdb
