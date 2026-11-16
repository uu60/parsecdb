
#ifndef MPC_PACKAGE_SYSTEM_H
#define MPC_PACKAGE_SYSTEM_H
#include "parsecdb/comm/Comm.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/third_party/ctpl_stl.h"

namespace parsecdb {

class System {
  private:
    // Task tags [0, PRESERVED_TASK_TAGS) belong to the background BMT generators.
    inline static int PRESERVED_TASK_TAGS = 2 * Conf::BMT_QUEUE_NUM;

  public:
    inline static std::atomic_bool _shutdown = false;

  public:
    static void init(int argc, char** argv);

    static void finalize();

    // The task tag for query traffic: the first one after the generators' tags (and at least 2). It is the same on
    // every call and every rank, because the client and the servers call it different numbers of times.
    static int nextTask();

    // The k-th task tag after nextTask() (k >= 1), for operators that run concurrently with the query and need their
    // own tag space. Throws when the tag does not fit in Conf::TASK_TAG_BITS.
    static int auxiliaryTask(int k);

    static int64_t currentTimeMillis();
};

} // namespace parsecdb

#endif
