
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <future>

#include "parsecdb/comm/Comm.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/parallel/Async.h"
#include "parsecdb/parallel/CtplThreadPool.h"
#include "parsecdb/parallel/TbbThreadPool.h"

namespace parsecdb {

class ThreadPoolSupport {
  public:
    inline static CtplThreadPool* _ctplPool = nullptr;
    inline static TbbThreadPool* _tbbPool = nullptr;
    inline static Async* _async = nullptr;

  public:
    // Requires Comm::init(): the client rank only supplies and reconstructs inputs and never runs server-side
    // operators, so it gets no worker pool and submit() runs tasks on the calling thread.
    static void init() {
        if (Conf::DISABLE_MULTI_THREAD || Comm::isClient()) {
            return;
        }
        if (Conf::THREAD_POOL_TYPE == Conf::CTPL_POOL) {
            _ctplPool = new CtplThreadPool(Conf::LOCAL_THREADS);
#ifdef PARSEC_HAS_TBB
        } else if (Conf::THREAD_POOL_TYPE == Conf::TBB_POOL) {
            _tbbPool = new TbbThreadPool(Conf::LOCAL_THREADS);
#endif
        } else if (Conf::THREAD_POOL_TYPE == Conf::ASYNC) {
            _async = new Async();
        }
    }

    static void finalize() {
        delete _ctplPool;
        delete _tbbPool;
        delete _async;
        _ctplPool = nullptr;
        _tbbPool = nullptr;
        _async = nullptr;
    }

    template <typename F> static std::future<std::invoke_result_t<F>> callerRun(F&& f) {
        using ReturnType = std::invoke_result_t<F>;
        std::promise<ReturnType> promise;
        if constexpr (std::is_void_v<ReturnType>) {
            std::invoke(std::forward<F>(f));
            promise.set_value();
        } else {
            promise.set_value(std::invoke(std::forward<F>(f)));
        }
        return promise.get_future();
    }

    template <typename F> static auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        if (Conf::DISABLE_MULTI_THREAD) {
            return callerRun(f);
        }
        if (Conf::THREAD_POOL_TYPE == Conf::CTPL_POOL) {
            return _ctplPool == nullptr ? callerRun(std::forward<F>(f)) : _ctplPool->submit(f);
        }
#ifdef PARSEC_HAS_TBB
        if (Conf::THREAD_POOL_TYPE == Conf::TBB_POOL) {
            return _tbbPool == nullptr ? callerRun(std::forward<F>(f)) : _tbbPool->submit(f);
        }
#endif
        if (Conf::THREAD_POOL_TYPE == Conf::ASYNC) {
            return _async == nullptr ? callerRun(std::forward<F>(f)) : _async->submit(f);
        }
        return callerRun(f);
    }
};

} // namespace parsecdb

#endif
