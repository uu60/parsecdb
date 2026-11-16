#include "parsecdb/secret/item/BitSecret.h"

#include "parsecdb/comm/Comm.h"
#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolXorBatchOperator.h"

// A BitSecret is a 1-bit Boolean share. Every method runs the corresponding batch operator on a batch of one element.

namespace parsecdb {

namespace {
// Result of a one-element batch; ranks that hold no result (the client, or servers after a reconstruction) get 0.
bool single(const std::vector<int64_t>& v) { return !v.empty() && (v[0] & 1); }
} // namespace

BitSecret::BitSecret(bool x, int taskTag) : _data(x), _taskTag(taskTag) {}

BitSecret BitSecret::task(int taskTag) { return BitSecret(_data, taskTag); }

BitSecret BitSecret::share(int clientRank) const {
    std::vector<int64_t> x{_data};
    return BitSecret(single(BoolBatchOperator(x, 1, _taskTag, 0, clientRank)._zis), _taskTag);
}

BitSecret BitSecret::lessThan(BitSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return BitSecret(
        single(BoolLessBatchOperator(&x, &y, 1, _taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE).execute()->_zis),
        _taskTag);
}

BitSecret BitSecret::not_() const { return BitSecret(_data ^ Comm::rank(), _taskTag); }

BitSecret BitSecret::xor_(BitSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return BitSecret(
        single(BoolXorBatchOperator(&x, &y, 1, _taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE).execute()->_zis),
        _taskTag);
}

BitSecret BitSecret::and_(BitSecret yi) const {
    std::vector<int64_t> x{_data}, y{yi._data};
    return BitSecret(
        single(BoolAndBatchOperator(&x, &y, 1, _taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE).execute()->_zis),
        _taskTag);
}

BitSecret BitSecret::or_(BitSecret yi) const { return xor_(yi).xor_(and_(yi)); }

BitSecret BitSecret::mux(BitSecret yi, BitSecret cond_i) const {
    std::vector<int64_t> x{_data}, y{yi._data}, c{cond_i._data};
    return BitSecret(
        single(BoolMutexBatchOperator(&x, &y, &c, 1, _taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE).execute()->_zis),
        _taskTag);
}

BitSecret BitSecret::reconstruct(int clientRank) const {
    std::vector<int64_t> x{_data};
    return BitSecret(
        single(
            BoolBatchOperator(x, 1, _taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE).reconstruct(clientRank)->_results),
        _taskTag);
}

bool BitSecret::get() const { return _data; }

} // namespace parsecdb
