#include "parsecdb/secret/Secrets.h"

#include "parsecdb/compute/batch/arith/ArithBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithLessBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithMutexBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/intermediate/IntermediateDataSupport.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include <algorithm>
#include <future>
#include <memory>

namespace parsecdb {

namespace {

void flipDescending(std::vector<int64_t>& conds, const std::vector<bool>& ascs) {
    for (size_t t = 0; t < ascs.size(); ++t) {
        if (!ascs[t]) {
            conds[t] ^= Comm::rank();
        }
    }
}

// Compare-and-swap of a batch of pairs (xs[t], ys[t]) for the Boolean and the arithmetic sharing. swap() returns 2n
// values: the first n are the new values at the x positions, the last n the new values at the y positions. A pair is
// put in ascending order when ascs[t] is true and in descending order otherwise.

struct BoolCompareSwap {
    using SecretT = BoolSecret;

    // Bitwise BMTs for one batch, polled on the calling thread so that both servers consume the background queues in
    // the same order regardless of thread scheduling. Empty unless BMTs are generated in the background.
    struct Bmts {
        std::vector<BitwiseBmt> less;
        std::vector<BitwiseBmt> mux;
    };

    static int tagStride(int /*width*/) {
        return std::max(BoolLessBatchOperator::tagStride(), BoolMutexBatchOperator::tagStride());
    }

    static std::shared_ptr<Bmts> pollBmts(size_t n, int width) {
        auto bmts = std::make_shared<Bmts>();
        if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
            const int count = static_cast<int>(n);
            bmts->less = IntermediateDataSupport::pollBitwiseBmts(BoolLessBatchOperator::bmtCount(count, width), 64);
            bmts->mux = IntermediateDataSupport::pollBitwiseBmts(BoolMutexBatchOperator::bmtCount(count, width), 64);
        }
        return bmts;
    }

    static std::vector<int64_t> swap(std::vector<int64_t>& xs, std::vector<int64_t>& ys, const std::vector<bool>& ascs,
                                     int width, int taskTag, int msgTag, Bmts& bmts) {
        const bool background = Conf::BMT_METHOD == Conf::BMT_BACKGROUND;
        auto conds = BoolLessBatchOperator(&xs, &ys, width, taskTag, msgTag, SecureOperator::NO_CLIENT_COMPUTE)
                         .setBmts(background ? &bmts.less : nullptr)
                         ->execute()
                         ->_zis;
        flipDescending(conds, ascs);
        // The operator without a client rank is bidirectional: it returns (c ? x : y) followed by (c ? y : x).
        return BoolMutexBatchOperator(&xs, &ys, &conds, width, taskTag, msgTag)
            .setBmts(background ? &bmts.mux : nullptr)
            ->execute()
            ->_zis;
    }
};

struct ArithCompareSwap {
    using SecretT = ArithSecret;

    // Background BMTs for one batch, polled on the calling thread (see BoolCompareSwap).
    struct Bmts {
        std::vector<BitwiseBmt> less;
        std::vector<Bmt> mux;
    };

    static int tagStride(int width) {
        return std::max(ArithLessBatchOperator::tagStride(width), ArithMutexBatchOperator::tagStride(width));
    }

    static std::shared_ptr<Bmts> pollBmts(size_t n, int width) {
        auto bmts = std::make_shared<Bmts>();
        if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
            const int count = static_cast<int>(n);
            bmts->less = IntermediateDataSupport::pollBitwiseBmts(ArithLessBatchOperator::bmtCount(count, width), 64);
            // One mux over both orders: 2n elements.
            bmts->mux = IntermediateDataSupport::pollBmts(ArithMutexBatchOperator::bmtCount(width, 2 * count), width);
        }
        return bmts;
    }

    static std::vector<int64_t> swap(std::vector<int64_t>& xs, std::vector<int64_t>& ys, const std::vector<bool>& ascs,
                                     int width, int taskTag, int msgTag, Bmts& bmts) {
        const bool background = Conf::BMT_METHOD == Conf::BMT_BACKGROUND;
        auto conds = ArithLessBatchOperator(&xs, &ys, width, taskTag, msgTag, SecureOperator::NO_CLIENT_COMPUTE)
                         .setBmts(background ? &bmts.less : nullptr)
                         ->execute()
                         ->_zis;
        flipDescending(conds, ascs);
        // One mux over the concatenation computes both outputs: (c ? x : y) and (c ? y : x).
        const size_t n = xs.size();
        std::vector<int64_t> lhs, rhs, conds2;
        lhs.reserve(2 * n);
        rhs.reserve(2 * n);
        conds2.reserve(2 * n);
        lhs.insert(lhs.end(), xs.begin(), xs.end());
        lhs.insert(lhs.end(), ys.begin(), ys.end());
        rhs.insert(rhs.end(), ys.begin(), ys.end());
        rhs.insert(rhs.end(), xs.begin(), xs.end());
        conds2.insert(conds2.end(), conds.begin(), conds.end());
        conds2.insert(conds2.end(), conds.begin(), conds.end());
        return ArithMutexBatchOperator(&lhs, &rhs, &conds2, width, taskTag, msgTag, SecureOperator::NO_CLIENT_COMPUTE)
            .setBmts(background ? &bmts.mux : nullptr)
            ->execute()
            ->_zis;
    }
};

// A batch of the comparisons of one bitonic step.
struct StepBatch {
    std::vector<int64_t> xs, ys;
    std::vector<int> xIdx, yIdx;
    std::vector<bool> ascs;
};

// Bitonic sort over a power-of-two number of secrets; padding entries sort to the end. Each step's comparisons run as
// one batch on the calling thread when multithreading is disabled, otherwise as batches of Conf::BATCH_SIZE on the
// thread pool, each with its own message-tag range.
template <class Ops> void bitonicSort(std::vector<typename Ops::SecretT>& secrets, bool asc, int taskTag) {
    const size_t n = secrets.size();
    // Only the servers hold shares; the operators return no results on the client.
    if (n < 2 || !Comm::isServer()) {
        return;
    }
    const int width = secrets[0]._width;
    const bool parallel = Conf::BATCH_SIZE > 0 && !Conf::DISABLE_MULTI_THREAD;
    const size_t batchSize = parallel ? static_cast<size_t>(Conf::BATCH_SIZE) : n;
    const int tagStride = Ops::tagStride(width);

    for (size_t k = 2; k <= n; k *= 2) {
        for (size_t j = k / 2; j > 0; j /= 2) {
            std::vector<StepBatch> batches;
            for (size_t i = 0; i < n; ++i) {
                const size_t l = i ^ j;
                if (l <= i) {
                    continue;
                }
                const bool dir = (i & k) == 0;
                auto& si = secrets[i];
                auto& sl = secrets[l];
                if (si._padding && sl._padding) {
                    continue;
                }
                if ((si._padding && dir) || (sl._padding && !dir)) {
                    std::swap(si, sl);
                    continue;
                }
                if (si._padding || sl._padding) {
                    continue;
                }
                if (batches.empty() || batches.back().xs.size() == batchSize) {
                    batches.emplace_back();
                }
                auto& b = batches.back();
                b.xs.push_back(si._data);
                b.ys.push_back(sl._data);
                b.xIdx.push_back(static_cast<int>(i));
                b.yIdx.push_back(static_cast<int>(l));
                b.ascs.push_back(dir ^ !asc);
            }

            auto apply = [&secrets](const StepBatch& b, const std::vector<int64_t>& zs) {
                const size_t sz = b.xIdx.size();
                for (size_t t = 0; t < sz; ++t) {
                    secrets[b.xIdx[t]]._data = zs[t];
                    secrets[b.yIdx[t]]._data = zs[t + sz];
                }
            };

            if (!parallel) {
                for (auto& b : batches) {
                    auto bmts = Ops::pollBmts(b.xs.size(), width);
                    apply(b, Ops::swap(b.xs, b.ys, b.ascs, width, taskTag, 0, *bmts));
                }
                continue;
            }

            std::vector<std::future<std::vector<int64_t>>> futures;
            futures.reserve(batches.size());
            for (size_t bi = 0; bi < batches.size(); ++bi) {
                auto bmts = Ops::pollBmts(batches[bi].xs.size(), width);
                futures.emplace_back(ThreadPoolSupport::submit([&batches, bi, bmts, width, taskTag, tagStride] {
                    auto& b = batches[bi];
                    return Ops::swap(b.xs, b.ys, b.ascs, width, taskTag, static_cast<int>(bi) * tagStride, *bmts);
                }));
            }
            for (size_t bi = 0; bi < batches.size(); ++bi) {
                apply(batches[bi], futures[bi].get());
            }
        }
    }
}

// Pads to the next power of two with padding entries, sorts, and removes the padding again.
template <class Ops> void sortPadded(std::vector<typename Ops::SecretT>& secrets, bool asc, int taskTag) {
    const size_t n = secrets.size();
    size_t padded = 1;
    while (padded < n) {
        padded <<= 1;
    }
    typename Ops::SecretT padding;
    padding._padding = true;
    secrets.resize(padded, padding);
    bitonicSort<Ops>(secrets, asc, taskTag);
    secrets.resize(n);
}

} // namespace

void Secrets::sort(std::vector<BoolSecret>& secrets, bool asc, int taskTag) {
    sortPadded<BoolCompareSwap>(secrets, asc, taskTag);
}

void Secrets::sort(std::vector<ArithSecret>& secrets, bool asc, int taskTag) {
    sortPadded<ArithCompareSwap>(secrets, asc, taskTag);
}

std::vector<int64_t> Secrets::boolShare(std::vector<int64_t>& origins, int clientRank, int width, int taskTag) {
    return BoolBatchOperator(origins, width, taskTag, 0, clientRank)._zis;
}

std::vector<int64_t> Secrets::boolReconstruct(std::vector<int64_t>& secrets, int clientRank, int width, int taskTag) {
    return BoolBatchOperator(secrets, width, taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE)
        .reconstruct(clientRank)
        ->_results;
}

std::vector<int64_t> Secrets::arithShare(std::vector<int64_t>& origins, int clientRank, int width, int taskTag) {
    return ArithBatchOperator(origins, width, taskTag, 0, clientRank)._zis;
}

std::vector<int64_t> Secrets::arithReconstruct(std::vector<int64_t>& secrets, int clientRank, int width, int taskTag) {
    return ArithBatchOperator(secrets, width, taskTag, 0, SecureOperator::NO_CLIENT_COMPUTE)
        .reconstruct(clientRank)
        ->_results;
}

} // namespace parsecdb
