#include "parsecdb/secret/item/BoolSecret.h"

#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolToArithBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolXorBatchOperator.h"
#include "parsecdb/utils/Math.h"

// Every method runs the corresponding batch operator on a batch of one element.

namespace parsecdb {

namespace {
// Result of a one-element batch; ranks that hold no result (the client, or servers after a reconstruction) get 0.
int64_t single(const std::vector<int64_t>& v) { return v.empty() ? 0 : v[0]; }
} // namespace

BoolSecret::BoolSecret() = default;

BoolSecret::BoolSecret(int64_t x, int l, int taskTag, int msgTagOffset)
    : _data(x), _width(l), _taskTag(taskTag), _currentMsgTag(msgTagOffset) {}

BoolSecret BoolSecret::task(int taskTag) const { return {_data, _width, taskTag, _currentMsgTag}; }

BoolSecret BoolSecret::msg(int msgTagOffset) const { return {_data, _width, _taskTag, msgTagOffset}; }

BoolSecret BoolSecret::share(int clientRank) const {
    std::vector<int64_t> x{_data};
    return {single(BoolBatchOperator(x, _width, _taskTag, _currentMsgTag, clientRank)._zis), _width, _taskTag,
            _currentMsgTag};
}

BoolSecret BoolSecret::reconstruct(int clientRank) const {
    std::vector<int64_t> x{_data};
    return {single(BoolBatchOperator(x, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .reconstruct(clientRank)
                       ->_results),
            _width, _taskTag, _currentMsgTag};
}

BoolSecret BoolSecret::xor_(BoolSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return {single(BoolXorBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .execute()
                       ->_zis),
            _width, _taskTag, _currentMsgTag};
}

BoolSecret BoolSecret::and_(BoolSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return {single(BoolAndBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .execute()
                       ->_zis),
            _width, _taskTag, _currentMsgTag};
}

BoolSecret BoolSecret::arithmetic() const {
    std::vector<int64_t> x{_data};
    return {single(BoolToArithBatchOperator(&x, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .execute()
                       ->_zis),
            _width, _taskTag, _currentMsgTag};
}

BitSecret BoolSecret::lessThan(BoolSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return BitSecret(
        single(BoolLessBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                   .execute()
                   ->_zis) &
            1,
        _taskTag);
}

BitSecret BoolSecret::getBit(int n) const { return BitSecret(Math::getBit(_data, n), _taskTag); }

BoolSecret BoolSecret::mux(BoolSecret yi, BitSecret cond_i) const {
    std::vector<int64_t> x{_data}, y{yi._data}, c{static_cast<int64_t>(cond_i.get())};
    return {
        single(BoolMutexBatchOperator(&x, &y, &c, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                   .execute()
                   ->_zis),
        _width, _taskTag, _currentMsgTag};
}

} // namespace parsecdb
