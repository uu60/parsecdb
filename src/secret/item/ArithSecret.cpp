#include "parsecdb/secret/item/ArithSecret.h"

#include "parsecdb/compute/batch/arith/ArithAddBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithLessBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithMultiplyBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithMutexBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithToBoolBatchOperator.h"
#include "parsecdb/utils/Math.h"

// Every method runs the corresponding batch operator on a batch of one element.

namespace parsecdb {

namespace {
// Result of a one-element batch; ranks that hold no result (the client, or servers after a reconstruction) get 0.
int64_t single(const std::vector<int64_t>& v) { return v.empty() ? 0 : v[0]; }
} // namespace

ArithSecret::ArithSecret() = default;

ArithSecret::ArithSecret(int64_t x, int l, int taskTag) : _data(x), _width(l), _taskTag(taskTag) {}

ArithSecret::ArithSecret(int64_t x, int l, int taskTag, int msgTagOffset)
    : _data(x), _width(l), _taskTag(taskTag), _currentMsgTag(msgTagOffset) {}

ArithSecret ArithSecret::task(int taskTag) const { return {_data, _width, taskTag, _currentMsgTag}; }

ArithSecret ArithSecret::msg(int msgTagOffset) const { return {_data, _width, _taskTag, msgTagOffset}; }

ArithSecret ArithSecret::share(int clientRank) const {
    std::vector<int64_t> x{_data};
    return {single(ArithBatchOperator(x, _width, _taskTag, _currentMsgTag, clientRank)._zis), _width, _taskTag,
            _currentMsgTag};
}

ArithSecret ArithSecret::reconstruct(int clientRank) const {
    std::vector<int64_t> x{_data};
    return {single(ArithBatchOperator(x, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .reconstruct(clientRank)
                       ->_results),
            _width, _taskTag, _currentMsgTag};
}

ArithSecret ArithSecret::add(ArithSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return {single(ArithAddBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .execute()
                       ->_zis),
            _width, _taskTag, _currentMsgTag};
}

ArithSecret ArithSecret::mul(ArithSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return {
        single(ArithMultiplyBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                   .execute()
                   ->_zis),
        _width, _taskTag, _currentMsgTag};
}

ArithSecret ArithSecret::boolean() const {
    std::vector<int64_t> x{_data};
    return {single(ArithToBoolBatchOperator(&x, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                       .execute()
                       ->_zis),
            _width, _taskTag, _currentMsgTag};
}

BitSecret ArithSecret::lessThan(ArithSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return BitSecret(
        single(ArithLessBatchOperator(&x, &y, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                   .execute()
                   ->_zis) &
            1,
        _taskTag);
}

BitSecret ArithSecret::getBit(int n) const { return BitSecret(Math::getBit(_data, n), _taskTag); }

ArithSecret ArithSecret::mux(ArithSecret yi, BitSecret cond_i) const {
    std::vector<int64_t> x{_data}, y{yi._data}, c{static_cast<int64_t>(cond_i.get())};
    return {
        single(ArithMutexBatchOperator(&x, &y, &c, _width, _taskTag, _currentMsgTag, SecureOperator::NO_CLIENT_COMPUTE)
                   .execute()
                   ->_zis),
        _width, _taskTag, _currentMsgTag};
}

} // namespace parsecdb
