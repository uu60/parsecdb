#include "parsecdb/intermediate/IntermediateDataSupport.h"

#include "parsecdb/comm/Comm.h"
#include "parsecdb/intermediate/BitwiseBmtBatchGenerator.h"
#include "parsecdb/intermediate/BmtBatchGenerator.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include "parsecdb/sync/BoostLockFreeQueue.h"
#include "parsecdb/sync/BoostSpscQueue.h"
#include "parsecdb/sync/LockBlockingQueue.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <stdexcept>

#include "parsecdb/ot/BaseOtBatchOperator.h"
#include "parsecdb/utils/Crypto.h"

namespace parsecdb {

namespace {

// Background generators run one BMT protocol per loop iteration with the peer server. Before each iteration both
// servers exchange their shutdown flags on a control tag reserved for this generator's task, and both stop if either
// is shutting down, so neither side is left waiting inside a protocol run its peer has abandoned.
bool backgroundGeneratorShouldStop(int taskTag) {
    const int messageBits = 32 - Conf::TASK_TAG_BITS;
    const auto messageMask = (uint32_t{1} << messageBits) - 1;
    const int controlTag = static_cast<int>((static_cast<uint32_t>(taskTag) << messageBits) | messageMask);

    int64_t localStop = System::_shutdown.load() ? 1 : 0;
    int64_t peerStop = 0;
    auto send = Comm::serverSendAsync(localStop, 1, controlTag);
    auto receive = Comm::serverReceiveAsync(peerStop, 1, controlTag);
    Comm::wait(send);
    Comm::wait(receive);
    return localStop != 0 || peerStop != 0;
}

} // namespace

void IntermediateDataSupport::prepareBmt() {
    if (Conf::BMT_METHOD == Conf::BMT_FIXED) {
        _fixedBmt = BmtBatchGenerator(1, 64, 0, 0).execute()->_bmts[0];
        _fixedBitwiseBmt = BitwiseBmtBatchGenerator(1, 64, 0, 0).execute()->_bmts[0];
    } else if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
        _bitwiseBmtQs.resize(Conf::BMT_QUEUE_NUM);
        if (Conf::BMT_QUEUE_TYPE == Conf::LOCK_QUEUE) {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bitwiseBmtQs[i] = new LockBlockingQueue<BitwiseBmt>(Conf::MAX_BMTS);
            }
        } else if (Conf::BMT_QUEUE_TYPE == Conf::LOCK_FREE_QUEUE) {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bitwiseBmtQs[i] = new BoostLockFreeQueue<BitwiseBmt>(Conf::MAX_BMTS);
            }
        } else {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bitwiseBmtQs[i] = new BoostSPSCQueue<BitwiseBmt>(Conf::MAX_BMTS);
            }
        }

        startGenerateBitwiseBmtsAsync();

        if (Conf::DISABLE_ARITH) {
            return;
        }

        _bmtQs.resize(Conf::BMT_QUEUE_NUM);
        if (Conf::BMT_QUEUE_TYPE == Conf::LOCK_QUEUE) {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bmtQs[i] = new LockBlockingQueue<Bmt>(Conf::MAX_BMTS);
            }
        } else if (Conf::BMT_QUEUE_TYPE == Conf::LOCK_FREE_QUEUE) {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bmtQs[i] = new BoostLockFreeQueue<Bmt>(Conf::MAX_BMTS);
            }
        } else {
            for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
                _bmtQs[i] = new BoostSPSCQueue<Bmt>(Conf::MAX_BMTS);
            }
        }
        startGenerateBmtsAsync();
    }
}

void IntermediateDataSupport::init() {
    if (Comm::isClient()) {
        return;
    }

    prepareRot();

    prepareIknp();

    prepareBmt();

    if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND && Conf::BMT_PRE_GEN_SECONDS > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(Conf::BMT_PRE_GEN_SECONDS));
    }
}

void IntermediateDataSupport::finalize() {
    // Stop the background generators before freeing the queues they write to: closing unblocks them, and every
    // generator leaves its loop once either server has started shutting down.
    for (auto* queue : _bmtQs) {
        queue->close();
    }
    for (auto* queue : _bitwiseBmtQs) {
        queue->close();
    }
    for (auto& future : _generatorFutures) {
        if (!future.valid()) {
            continue;
        }
        try {
            future.get();
        } catch (...) {
        }
    }
    _generatorFutures.clear();

    for (auto* queue : _bmtQs) {
        delete queue;
    }
    for (auto* queue : _bitwiseBmtQs) {
        delete queue;
    }
    _bmtQs.clear();
    _bitwiseBmtQs.clear();

    delete _currentBmt;
    _currentBmt = nullptr;
    delete _currentBitwiseBmt;
    _currentBitwiseBmt = nullptr;
    delete _sRot0;
    _sRot0 = nullptr;
    delete _rRot0;
    _rRot0 = nullptr;
    delete _sRot1;
    _sRot1 = nullptr;
    delete _rRot1;
    _rRot1 = nullptr;
}

void IntermediateDataSupport::prepareRot() {
    if (Comm::isClient()) {
        return;
    }

    // One base-OT batch per direction prepares both random OTs: ROT1 uses the opposite choice bit of ROT0.
    for (int i = 0; i < 2; i++) {
        const bool isSender = Comm::rank() == i;
        std::vector<int64_t> r0s, r1s;
        std::vector<int> choices;
        if (isSender) {
            _sRot0 = new SRot();
            _sRot1 = new SRot();
            _sRot0->_r0 = Math::randBits();
            _sRot0->_r1 = Math::randBits();
            _sRot1->_r0 = Math::randBits();
            _sRot1->_r1 = Math::randBits();
            r0s = {_sRot0->_r0, _sRot1->_r0};
            r1s = {_sRot0->_r1, _sRot1->_r1};
        } else {
            _rRot0 = new RRot();
            _rRot1 = new RRot();
            _rRot0->_b = static_cast<int>(Math::randInt(0, 1));
            _rRot1->_b = 1 - _rRot0->_b;
            choices = {_rRot0->_b, _rRot1->_b};
        }
        BaseOtBatchOperator ot(i, &r0s, &r1s, &choices, 64, 2, i * BaseOtBatchOperator::tagStride());
        ot.execute();
        if (!isSender) {
            _rRot0->_rb = ot._results[0];
            _rRot1->_rb = ot._results[1];
        }
    }
}

std::vector<Bmt> IntermediateDataSupport::pollBmts(int count, int width) {
    std::vector<Bmt> result;
    if (Comm::isClient()) {
        return result;
    }
    if (_bmtQs.empty()) {
        throw std::runtime_error(
            "Arithmetic background BMTs were requested, but no arithmetic BMT queue is configured; "
            "set --disable_arith=false.");
    }

    result.reserve(count);

    while (count > 0) {
        int left = _currentBmtLeftTimes;

        if (_currentBmt == nullptr || left == 0) {
            delete _currentBmt;
            Bmt newBmt = _bmtQs[_currentBmtQ++ % Conf::BMT_QUEUE_NUM]->poll();
            newBmt._a = Math::ring(newBmt._a, width);
            newBmt._b = Math::ring(newBmt._b, width);
            newBmt._c = Math::ring(newBmt._c, width);
            _currentBmt = new Bmt(newBmt);
            _currentBmtLeftTimes = Conf::BMT_USAGE_LIMIT;
            left = Conf::BMT_USAGE_LIMIT;
        }

        int useCount = std::min(count, left);

        for (int i = 0; i < useCount; ++i) {
            result.push_back(*_currentBmt);
        }

        _currentBmtLeftTimes -= useCount;
        count -= useCount;
    }

    return result;
}

std::vector<BitwiseBmt> IntermediateDataSupport::pollBitwiseBmts(int count, int width) {
    std::vector<BitwiseBmt> result;
    if (Comm::isClient()) {
        return result;
    }
    if (_bitwiseBmtQs.empty()) {
        throw std::runtime_error("Bitwise background BMTs were requested before queue initialization.");
    }

    result.reserve(count);

    while (count > 0) {
        int left = _currentBitwiseBmtLeftTimes;

        if (_currentBitwiseBmt == nullptr || left == 0) {
            delete _currentBitwiseBmt;
            BitwiseBmt newBmt = _bitwiseBmtQs[_currentBitwiseBmtQ++ % Conf::BMT_QUEUE_NUM]->poll();
            newBmt._a = Math::ring(newBmt._a, width);
            newBmt._b = Math::ring(newBmt._b, width);
            newBmt._c = Math::ring(newBmt._c, width);
            _currentBitwiseBmt = new BitwiseBmt(newBmt);
            _currentBitwiseBmtLeftTimes = Conf::BMT_USAGE_LIMIT;
            left = Conf::BMT_USAGE_LIMIT;
        }

        int useCount = std::min(count, left);

        for (int i = 0; i < useCount; ++i) {
            result.push_back(*_currentBitwiseBmt);
        }

        _currentBitwiseBmtLeftTimes -= useCount;
        count -= useCount;
    }

    return result;
}

void IntermediateDataSupport::startGenerateBmtsAsync() {
    if (Comm::isServer() && Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
        for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
            _generatorFutures.emplace_back(ThreadPoolSupport::submit([i] {
                try {
                    auto q = _bmtQs[i];
                    const int taskTag = Conf::BMT_QUEUE_NUM + i;
                    const int batchSize = std::min(Conf::BMT_GEN_BATCH_SIZE, 1024);
                    while (!backgroundGeneratorShouldStop(taskTag)) {
                        auto bmts = BmtBatchGenerator(batchSize, 64, taskTag, 0).execute()->_bmts;
                        for (auto& bmt : bmts) {
                            q->offer(bmt);
                        }
                    }
                } catch (...) {
                }
            }));
        }
    }
}

void IntermediateDataSupport::startGenerateBitwiseBmtsAsync() {
    if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
        for (int i = 0; i < Conf::BMT_QUEUE_NUM; i++) {
            _generatorFutures.emplace_back(ThreadPoolSupport::submit([i] {
                try {
                    auto q = _bitwiseBmtQs[i];
                    while (!backgroundGeneratorShouldStop(i)) {
                        auto bmts = BitwiseBmtBatchGenerator(Conf::BMT_GEN_BATCH_SIZE, 64, i, 0).execute()->_bmts;
                        for (auto& bmt : bmts) {
                            q->offer(bmt);
                        }
                    }
                } catch (...) {
                }
            }));
        }
    }
}

void IntermediateDataSupport::prepareIknp() {
    if (Comm::isClient()) {
        return;
    }
    if (!_iknpBaseSeeds0.empty()) {
        return; // Already initialized
    }
    // Direction 0: rank 0 is the IKNP sender. Direction 1: rank 1 is the IKNP sender.
    prepareIknpDirection(0, 100, _iknpBaseSeeds0, _iknpSenderChoices0);
    prepareIknpDirection(1, 300, _iknpBaseSeeds1, _iknpSenderChoices1);
}

void IntermediateDataSupport::prepareIknpDirection(int iknpSender, int msgTagOffset,
                                                   std::vector<std::array<U128, 2>>& seeds,
                                                   std::vector<bool>& senderChoices) {
    // IKNP reverses the base-OT roles: the IKNP receiver is the base-OT sender and supplies two seeds per row; the
    // IKNP sender is the base-OT receiver, picks one seed per row with its secret choice bit s_i, and keeps s secret.
    //
    // Each 128-bit seed is one base OT in 128-bit message mode (low and high words under one key exchange).
    constexpr int kRows = 128;
    const int baseOtSender = 1 - iknpSender;
    const bool isBaseOtSender = Comm::rank() == baseOtSender;
    const int taskTag = 1;

    std::vector<int64_t> lo0, lo1, hi0, hi1;
    std::vector<int> choices;
    if (isBaseOtSender) {
        lo0.resize(kRows);
        lo1.resize(kRows);
        hi0.resize(kRows);
        hi1.resize(kRows);
        for (int i = 0; i < kRows; ++i) {
            lo0[i] = Math::randBits();
            hi0[i] = Math::randBits();
            lo1[i] = Math::randBits();
            hi1[i] = Math::randBits();
        }
    } else {
        choices.resize(kRows);
        for (int i = 0; i < kRows; ++i) {
            choices[i] = static_cast<int>(Math::randInt(0, 1));
        }
    }

    BaseOtBatchOperator baseOt(baseOtSender, &lo0, &lo1, &choices, 64, taskTag, msgTagOffset);
    baseOt.withHighWords(isBaseOtSender ? &hi0 : nullptr, isBaseOtSender ? &hi1 : nullptr)->execute();

    seeds.assign(kRows, {});
    if (isBaseOtSender) {
        // IKNP receiver: keeps both seeds of every row.
        for (int i = 0; i < kRows; ++i) {
            seeds[i][0] = {static_cast<uint64_t>(lo0[i]), static_cast<uint64_t>(hi0[i])};
            seeds[i][1] = {static_cast<uint64_t>(lo1[i]), static_cast<uint64_t>(hi1[i])};
        }
    } else {
        // IKNP sender: keeps the chosen seed k_i^{s_i} and the choice bits.
        senderChoices.assign(kRows, false);
        for (int i = 0; i < kRows; ++i) {
            seeds[i][0] = {static_cast<uint64_t>(baseOt._results[i]), static_cast<uint64_t>(baseOt._resultsHi[i])};
            seeds[i][1] = seeds[i][0];
            senderChoices[i] = choices[i] != 0;
        }
    }
}

} // namespace parsecdb
