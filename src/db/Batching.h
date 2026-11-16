#ifndef PARSECDB_DB_BATCHING_H
#define PARSECDB_DB_BATCHING_H

// Private helpers shared by View.cpp and Views.cpp: splitting operator inputs into batches, and shorthands for the
// batch operators the database layer calls on boolean shares.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "parsecdb/base/SecureOperator.h"
#include "parsecdb/comm/Comm.h"
#include "parsecdb/compute/batch/arith/ArithMultiplyBatchOperator.h"
#include "parsecdb/compute/batch/arith/ArithToBoolBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolAndBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolEqualBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolLessBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolMutexBatchOperator.h"
#include "parsecdb/compute/batch/bool/BoolToArithBatchOperator.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/intermediate/IntermediateDataSupport.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"

namespace parsecdb::db {

using Shares = std::vector<int64_t>;

inline constexpr int NO_CLIENT = SecureOperator::NO_CLIENT_COMPUTE;

// True when operators split their input into Conf::BATCH_SIZE batches and run them in parallel on the thread pool.
// Otherwise every operator processes its whole input as one batch on the calling thread.
inline bool multiBatchEnabled() { return Conf::BATCH_SIZE > 0 && !Conf::DISABLE_MULTI_THREAD; }

// A split of the index range [0, total) into consecutive batches.
struct BatchPlan {
    size_t total = 0;
    size_t batchSize = 1;
    int numBatches = 0;
    bool parallel = false;

    [[nodiscard]] size_t begin(int b) const { return static_cast<size_t>(b) * batchSize; }
    [[nodiscard]] size_t end(int b) const { return std::min(total, begin(b) + batchSize); }
};

// Batches of Conf::BATCH_SIZE for the thread pool when multiBatchEnabled(), else one batch run on the caller.
inline BatchPlan planBatches(size_t total) {
    BatchPlan plan;
    plan.total = total;
    if (total == 0) {
        return plan;
    }
    plan.parallel = multiBatchEnabled();
    plan.batchSize = plan.parallel ? static_cast<size_t>(Conf::BATCH_SIZE) : total;
    plan.numBatches = static_cast<int>((total + plan.batchSize - 1) / plan.batchSize);
    return plan;
}

// Runs fn(i) for i in [0, count), on the thread pool when `parallel` and otherwise in order on the calling thread.
// Returns the results in index order (nothing for void tasks). The pool runs tasks inline when multi-threading is off.
template <typename F> auto runTasks(int count, bool parallel, F&& fn) {
    using R = std::invoke_result_t<F&, int>;
    if constexpr (std::is_void_v<R>) {
        if (!parallel) {
            for (int i = 0; i < count; ++i) {
                fn(i);
            }
            return;
        }
        std::vector<std::future<void>> futures;
        futures.reserve(count);
        for (int i = 0; i < count; ++i) {
            futures.push_back(ThreadPoolSupport::submit([&fn, i] { fn(i); }));
        }
        for (auto& f : futures) {
            f.get();
        }
    } else {
        std::vector<R> results;
        results.reserve(count);
        if (!parallel) {
            for (int i = 0; i < count; ++i) {
                results.push_back(fn(i));
            }
            return results;
        }
        std::vector<std::future<R>> futures;
        futures.reserve(count);
        for (int i = 0; i < count; ++i) {
            futures.push_back(ThreadPoolSupport::submit([&fn, i] { return fn(i); }));
        }
        for (auto& f : futures) {
            results.push_back(f.get());
        }
        return results;
    }
}

// Like runTasks, but first calls prepare(i) on the calling thread for every task, in index order, before any task
// starts, and then runs fn(i, prepared_i). Use it for per-task inputs that must be taken in a fixed order from shared
// state (for example correlated randomness polled from a queue).
template <typename P, typename F> auto runTasks(int count, bool parallel, P&& prepare, F&& fn) {
    std::vector<std::invoke_result_t<P&, int>> prepared;
    prepared.reserve(count);
    for (int i = 0; i < count; ++i) {
        prepared.push_back(prepare(i));
    }
    return runTasks(count, parallel, [&](int i) { return fn(i, prepared[i]); });
}

// Runs fn(b, begin, end) for every batch of the plan; see runTasks.
template <typename F> auto runBatches(const BatchPlan& plan, F&& fn) {
    return runTasks(plan.numBatches, plan.parallel, [&](int b) { return fn(b, plan.begin(b), plan.end(b)); });
}

// Runs prepare(b, begin, end) for every batch on the calling thread, in batch order, then fn(b, begin, end, prepared_b)
// for every batch; see runTasks.
template <typename P, typename F> auto runBatches(const BatchPlan& plan, P&& prepare, F&& fn) {
    return runTasks(
        plan.numBatches, plan.parallel, [&](int b) { return prepare(b, plan.begin(b), plan.end(b)); },
        [&](int b, auto& prepared) { return fn(b, plan.begin(b), plan.end(b), prepared); });
}

// Concatenates per-batch results in order; a single part is moved instead of copied.
inline Shares concat(std::vector<Shares>&& parts) {
    if (parts.size() == 1) {
        return std::move(parts[0]);
    }
    size_t total = 0;
    for (const auto& p : parts) {
        total += p.size();
    }
    Shares out;
    out.reserve(total);
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// The rows [begin, end) of col: col itself when the range covers all of it, else a copy stored in buf.
inline Shares* slice(Shares& col, size_t begin, size_t end, Shares& buf) {
    if (begin == 0 && end == col.size()) {
        return &col;
    }
    buf.assign(col.begin() + static_cast<std::ptrdiff_t>(begin), col.begin() + static_cast<std::ptrdiff_t>(end));
    return &buf;
}

// Background BMTs.
//
// With Conf::BMT_METHOD == BMT_BACKGROUND an operator that is not handed BMTs polls them from the shared background
// queues itself. Both servers must poll in the same order, so operators that run concurrently on the thread pool get
// their BMTs polled beforehand on the calling thread, in a fixed order (see the prepare overloads of runTasks and
// runBatches). Operators running on the calling thread while nothing else polls may poll themselves.

// Whether operators dispatched concurrently need BMTs polled for them beforehand.
inline bool prefetchBmts(bool concurrent) { return concurrent && Conf::BMT_METHOD == Conf::BMT_BACKGROUND; }

// The BMTs of one task, polled in the order its operators consume them. An inactive list polls nothing and hands out
// nullptr, so every operator obtains its own BMTs as usual.
class BmtList {
  public:
    BmtList() = default;
    explicit BmtList(bool active) : _active(active) {}

    // Polls the BMTs of a boolean operator over count BMTs (its bmtCount()).
    void addBitwise(int count) {
        if (_active) {
            _bitwise.push_back(count > 0 ? IntermediateDataSupport::pollBitwiseBmts(count, 64)
                                         : std::vector<BitwiseBmt>());
        }
    }

    // Polls the BMTs of an arithmetic operator.
    void addArith(int count, int width) {
        if (_active) {
            _arith.push_back(count > 0 ? IntermediateDataSupport::pollBmts(count, width) : std::vector<Bmt>());
        }
    }

    // The next polled boolean BMTs, or nullptr when inactive.
    std::vector<BitwiseBmt>* nextBitwise() { return next(_bitwise, _nextBitwise); }

    std::vector<Bmt>* nextArith() { return next(_arith, _nextArith); }

  private:
    template <typename T> std::vector<T>* next(std::vector<std::vector<T>>& lists, size_t& cursor) {
        if (!_active) {
            return nullptr;
        }
        if (cursor >= lists.size()) {
            throw std::logic_error("BmtList: more operators than polled BMT sets");
        }
        return &lists[cursor++];
    }

    bool _active = false;
    std::vector<std::vector<BitwiseBmt>> _bitwise;
    std::vector<std::vector<Bmt>> _arith;
    size_t _nextBitwise = 0;
    size_t _nextArith = 0;
};

// Operator shorthands. All run without client help. bmts (nullptr: the operator obtains its own) are the operator's
// BMTs, of the size its bmtCount() gives. taskTag selects a task tag namespace.

inline Shares andOp(Shares& xs, Shares& ys, int msgTag, std::vector<BitwiseBmt>* bmts = nullptr, int taskTag = 0) {
    return BoolAndBatchOperator(&xs, &ys, 1, taskTag, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

inline Shares lessOp(Shares& xs, Shares& ys, int width, int msgTag, std::vector<BitwiseBmt>* bmts = nullptr,
                     int taskTag = 0) {
    return BoolLessBatchOperator(&xs, &ys, width, taskTag, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

inline Shares equalOp(Shares& xs, Shares& ys, int width, int msgTag, std::vector<BitwiseBmt>* bmts = nullptr,
                      int taskTag = 0) {
    return BoolEqualBatchOperator(&xs, &ys, width, taskTag, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

// Per element: conds ? xs : ys.
inline Shares mutexOp(Shares& xs, Shares& ys, Shares& conds, int width, int msgTag,
                      std::vector<BitwiseBmt>* bmts = nullptr) {
    return BoolMutexBatchOperator(&xs, &ys, &conds, width, 0, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

// Bidirectional mutex: returns the first outputs followed by the second outputs. conds repeats when it is shorter than
// xs.
inline Shares mutexBothOp(Shares& xs, Shares& ys, Shares& conds, int width, int msgTag,
                          std::vector<BitwiseBmt>* bmts = nullptr) {
    return BoolMutexBatchOperator(&xs, &ys, &conds, width, 0, msgTag).setBmts(bmts)->execute()->_zis;
}

inline Shares boolToArithOp(Shares& xs, int msgTag) {
    return BoolToArithBatchOperator(&xs, 64, 0, msgTag, NO_CLIENT).execute()->_zis;
}

inline Shares arithToBoolOp(Shares& xs, int msgTag, std::vector<BitwiseBmt>* bmts = nullptr) {
    return ArithToBoolBatchOperator(&xs, 64, 0, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

inline Shares arithMultiplyOp(Shares& xs, Shares& ys, int msgTag, std::vector<Bmt>* bmts = nullptr) {
    return ArithMultiplyBatchOperator(&xs, &ys, 64, 0, msgTag, NO_CLIENT).setBmts(bmts)->execute()->_zis;
}

// Boolean NOT of 1-bit shares: one party flips its share.
inline void flipBits(Shares& bits) {
    const int64_t rank = Comm::rank();
    for (auto& b : bits) {
        b ^= rank;
    }
}

} // namespace parsecdb::db

#endif
