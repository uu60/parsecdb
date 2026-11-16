
#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"

#include "parsecdb/conf/Conf.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include "parsecdb/utils/Math.h"

namespace parsecdb {

ArithBatchOperator::ArithBatchOperator(std::vector<int64_t>& zs, int width, int taskTag, int msgTagOffset,
                                       int clientRank)
    : AbstractBatchOperator(width, taskTag, msgTagOffset) {
    requireSupportedWidth(width);
    if (clientRank < 0) {
        _zis = zs;
    } else {
        if (Comm::rank() == clientRank) {
            std::vector<int64_t> zv0, zv1;
            zv0.reserve(zs.size());
            zv1.reserve(zs.size());
            for (auto z : zs) {
                int64_t z1 = Math::randBits();
                int64_t z0 = Math::ringSubtract(z, z1, _width);
                zv0.push_back(z0);
                zv1.push_back(z1);
            }
            auto r = Comm::sendAsync(zv0, _width, 0, buildTag(_currentMsgTag));
            Comm::send(zv1, _width, 1, buildTag(_currentMsgTag));
            Comm::wait(r);
        } else if (Comm::isServer()) {
            _zis.clear();
            Comm::receive(_zis, _width, clientRank, buildTag(_currentMsgTag));
        }
    }
}

ArithBatchOperator::ArithBatchOperator(std::vector<int64_t>* xs, std::vector<int64_t>* ys, int width, int taskTag,
                                       int msgTagOffset, int clientRank)
    : AbstractBatchOperator(width, taskTag, msgTagOffset) {
    requireSupportedWidth(width);
    if (clientRank < 0) {
        _xis = xs;
        _yis = ys;
    } else {
        if (Comm::rank() == clientRank) {
            std::vector<int64_t> v0, v1;
            // ys may be null for single-input operators (e.g. share conversions); then only xs is shared.
            size_t size = xs->size();
            v0.reserve(2 * size);
            v1.reserve(2 * size);

            for (auto x : *xs) {
                int64_t x1 = Math::randBits();
                int64_t x0 = Math::ringSubtract(x, x1, _width);
                v0.push_back(x0);
                v1.push_back(x1);
            }
            if (ys != nullptr) {
                for (auto y : *ys) {
                    int64_t y1 = Math::randBits();
                    int64_t y0 = Math::ringSubtract(y, y1, _width);
                    v0.push_back(y0);
                    v1.push_back(y1);
                }
            }
            auto r = Comm::sendAsync(v0, _width, 0, buildTag(_currentMsgTag));
            Comm::send(v1, _width, 1, buildTag(_currentMsgTag));
            Comm::wait(r);
        } else if (Comm::isServer()) {
            std::vector<int64_t> temp;
            Comm::receive(temp, _width, clientRank, buildTag(_currentMsgTag));
            if (ys == nullptr) {
                _xis = new std::vector<int64_t>(std::move(temp));
                _dx = true;
            } else {
                const size_t size = temp.size() / 2;
                _xis = new std::vector<int64_t>(temp.begin(), temp.begin() + size);
                _dx = true;
                _yis = new std::vector<int64_t>(temp.begin() + size, temp.end());
                _dy = true;
            }
        }
    }
}

ArithBatchOperator* ArithBatchOperator::reconstruct(int clientRank) {
    _currentMsgTag = _startMsgTag;
    if (Comm::isServer()) {
        Comm::send(_zis, _width, clientRank, buildTag(_currentMsgTag));
    } else if (Comm::rank() == clientRank) {
        std::vector<int64_t> temp0, temp1;
        Comm::receive(temp0, _width, 0, buildTag(_currentMsgTag));
        Comm::receive(temp1, _width, 1, buildTag(_currentMsgTag));
        _results.clear();
        _results.reserve(temp0.size() + temp1.size());
        for (int i = 0; i < temp0.size(); i++) {
            _results.push_back(Math::ringAdd(temp0[i], temp1[i], _width));
        }
    }
    return this;
}

ArithBatchOperator* ArithBatchOperator::execute() { throw std::runtime_error("Needs implementation."); }

} // namespace parsecdb
