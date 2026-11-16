
#include "parsecdb/compute/batch/arith/ArithToBoolBatchOperator.h"

#include "parsecdb/comm/Comm.h"
#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/intermediate/BitwiseBmtBatchGenerator.h"
#include "parsecdb/intermediate/IntermediateDataSupport.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"
#include "parsecdb/utils/System.h"
#include <iterator>
#include <stdexcept>

namespace parsecdb {

ArithToBoolBatchOperator::ArithToBoolBatchOperator(std::vector<int64_t>* xs, int width, int taskTag, int msgTagOffset,
                                                   int clientRank)
    : ArithBatchOperator(*xs, width, taskTag, msgTagOffset, clientRank) {
    _xis = new std::vector<int64_t>(std::move(_zis));
    _zis.resize(xs->size(), 0);
}

ArithToBoolBatchOperator::~ArithToBoolBatchOperator() { delete _xis; }

ArithToBoolBatchOperator* ArithToBoolBatchOperator::execute() {
    _currentMsgTag = _startMsgTag;

    if (Comm::isClient()) {
        return this;
    }

    int64_t start = 0;
    if (Conf::ENABLE_CLASS_WISE_TIMING) {
        start = System::currentTimeMillis();
    }

    int num = static_cast<int>(_xis->size());
    _zis.resize(num, 0);

    // With background BMTs, every AND below takes its BMTs from one vector obtained up front, in a fixed order.
    const bool background = Conf::BMT_METHOD == Conf::BMT_BACKGROUND;
    std::vector<BitwiseBmt> backgroundBmts;
    size_t backgroundOffset = 0;
    if (background) {
        backgroundBmts =
            _bmts != nullptr ? std::move(*_bmts) : IntermediateDataSupport::pollBitwiseBmts(bmtCount(num, _width), 64);
        if (backgroundBmts.size() != static_cast<size_t>(bmtCount(num, _width))) {
            throw std::runtime_error("ArithToBoolBatchOperator: Mismatch BMT size.");
        }
    }
    auto nextBmts = [&](int count) {
        std::vector<BitwiseBmt> result;
        if (!background) {
            return result;
        }
        const size_t end = backgroundOffset + static_cast<size_t>(count);
        if (end > backgroundBmts.size()) {
            throw std::runtime_error("ArithToBoolBatchOperator: Exhausted assigned BMTs.");
        }
        result.assign(std::make_move_iterator(backgroundBmts.begin() + backgroundOffset),
                      std::make_move_iterator(backgroundBmts.begin() + end));
        backgroundOffset = end;
        return result;
    };
    const int andBmts = BoolAndBatchOperator::bmtCount(num, 1);

    std::vector<int64_t> xi_i_vec(num), xi_o_vec(num);
    for (int i = 0; i < num; i++) {
        xi_i_vec[i] = Math::randBits();
        xi_o_vec[i] = xi_i_vec[i] ^ (*_xis)[i];
    }

    std::vector<bool> carry_i_vec(num, false);

    for (int bitPos = 0; bitPos < _width; bitPos++) {
        std::vector<bool> ai_vec(num), ao_vec(num), bi_vec(num), bo_vec(num);

        for (int i = 0; i < num; i++) {
            if (Comm::rank() == 0) {
                ai_vec[i] = Math::getBit(xi_i_vec[i], bitPos);
                ao_vec[i] = Math::getBit(xi_o_vec[i], bitPos);
            } else {
                bi_vec[i] = Math::getBit(xi_i_vec[i], bitPos);
                bo_vec[i] = Math::getBit(xi_o_vec[i], bitPos);
            }
        }

        std::vector<int64_t> self_ov(num), other_iv(num);
        for (int i = 0; i < num; i++) {
            if (Comm::rank() == 0) {
                self_ov[i] = static_cast<int64_t>(ao_vec[i]);
            } else {
                self_ov[i] = static_cast<int64_t>(bo_vec[i]);
            }
        }

        auto r0 = Comm::serverSendAsync(self_ov, _width, buildTag(_currentMsgTag));
        auto r1 = Comm::serverReceiveAsync(other_iv, num, _width, buildTag(_currentMsgTag));
        Comm::wait(r0);
        Comm::wait(r1);

        for (int i = 0; i < num; i++) {
            if (Comm::rank() == 0) {
                bi_vec[i] = other_iv[i];
            } else {
                ai_vec[i] = other_iv[i];
            }
        }

        for (int i = 0; i < num; i++) {
            _zis[i] = Math::changeBit(_zis[i], bitPos, (ai_vec[i] ^ bi_vec[i]) ^ carry_i_vec[i]);
        }

        if (bitPos < _width - 1) {
            std::vector<bool> propagate_i_vec(num), generate_i_vec(num), tempCarry_i_vec(num);

            for (int i = 0; i < num; i++) {
                propagate_i_vec[i] = ai_vec[i] ^ bi_vec[i];
            }

            std::vector<int64_t> ai_int(num), bi_int(num);
            for (int i = 0; i < num; i++) {
                ai_int[i] = static_cast<int64_t>(ai_vec[i]);
                bi_int[i] = static_cast<int64_t>(bi_vec[i]);
            }

            BoolAndBatchOperator generateOp(&ai_int, &bi_int, 1, _taskTag, _currentMsgTag, NO_CLIENT_COMPUTE);
            auto generateBmts = nextBmts(andBmts);
            generateOp.setBmts(background ? &generateBmts : nullptr)->execute();

            std::vector<int64_t> propagate_int(num), carry_int(num);
            for (int i = 0; i < num; i++) {
                propagate_int[i] = static_cast<int64_t>(propagate_i_vec[i]);
                carry_int[i] = static_cast<int64_t>(carry_i_vec[i]);
            }

            BoolAndBatchOperator propagateOp(&propagate_int, &carry_int, 1, _taskTag, _currentMsgTag,
                                             NO_CLIENT_COMPUTE);
            auto propagateBmts = nextBmts(andBmts);
            propagateOp.setBmts(background ? &propagateBmts : nullptr)->execute();

            std::vector<int64_t> generate_results = generateOp._zis;
            std::vector<int64_t> tempCarry_results = propagateOp._zis;
            std::vector<int64_t> sum_int(num);

            for (int i = 0; i < num; i++) {
                generate_i_vec[i] = generate_results[i];
                tempCarry_i_vec[i] = tempCarry_results[i];
                sum_int[i] = generate_results[i] ^ tempCarry_results[i];
            }

            BoolAndBatchOperator finalOp(&generate_results, &tempCarry_results, 1, _taskTag, _currentMsgTag,
                                         NO_CLIENT_COMPUTE);
            auto finalBmts = nextBmts(andBmts);
            finalOp.setBmts(background ? &finalBmts : nullptr)->execute();

            for (int i = 0; i < num; i++) {
                carry_i_vec[i] = sum_int[i] ^ finalOp._zis[i];
            }
        }
    }

    for (int i = 0; i < num; i++) {
        _zis[i] = ring(_zis[i]);
    }
    if (background && backgroundOffset != backgroundBmts.size()) {
        throw std::runtime_error("ArithToBoolBatchOperator: Unused assigned BMTs.");
    }

    if (Conf::ENABLE_CLASS_WISE_TIMING) {
        _totalTime += System::currentTimeMillis() - start;
    }

    return this;
}

ArithToBoolBatchOperator* ArithToBoolBatchOperator::reconstruct(int clientRank) {
    _currentMsgTag = _startMsgTag;
    BoolBatchOperator e(_zis, _width, _taskTag, _currentMsgTag, NO_CLIENT_COMPUTE);
    e.reconstruct(clientRank);
    if (Comm::rank() == clientRank) {
        _results = e._results;
    }
    return this;
}

int ArithToBoolBatchOperator::tagStride(int width) { return BoolAndBatchOperator::tagStride(); }

ArithToBoolBatchOperator* ArithToBoolBatchOperator::setBmts(std::vector<BitwiseBmt>* bmts) {
    if (bmts != nullptr && bmts->size() != static_cast<size_t>(bmtCount(static_cast<int>(_xis->size()), _width))) {
        throw std::runtime_error("ArithToBoolBatchOperator: Mismatch BMT size.");
    }
    _bmts = bmts;
    return this;
}

// Three one-bit ANDs for each of the width - 1 carry steps.
int ArithToBoolBatchOperator::bmtCount(int num, int width) {
    return width <= 1 ? 0 : 3 * (width - 1) * BoolAndBatchOperator::bmtCount(num, 1);
}

} // namespace parsecdb
