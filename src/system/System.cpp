
#include "parsecdb/utils/System.h"

#include "parsecdb/base/SecureOperator.h"
#include "parsecdb/comm/Comm.h"
#include "parsecdb/comm/MpiComm.h"
#include "parsecdb/conf/Conf.h"
#include "parsecdb/intermediate/IntermediateDataSupport.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include "parsecdb/utils/Log.h"
#include "parsecdb/utils/Math.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace parsecdb {

void System::init(int argc, char** argv) {
    _shutdown = false;
    Conf::init(argc, argv);

    if (Conf::BMT_METHOD == Conf::BMT_BACKGROUND) {
        PRESERVED_TASK_TAGS = Conf::BMT_QUEUE_NUM;
        if (!Conf::DISABLE_ARITH) {
            PRESERVED_TASK_TAGS *= 2;
        }
    }

    Comm::init(argc, argv);

    // The rank must be known before the pool is created, so that the client rank does not allocate worker threads.
    ThreadPoolSupport::init();

    IntermediateDataSupport::init();
    Log::i("System initialized.");
}

void System::finalize() {
    Log::i("Prepare to shutdown... (if not finalized please press Ctrl + C)");
    _shutdown = true;
    // Join the background BMT generators (which still communicate) before the pool and the communicator go away.
    IntermediateDataSupport::finalize();
    ThreadPoolSupport::finalize();
    Comm::finalize();
    if (Comm::rank() == 0) {
        std::cout << "System shut down." << std::endl;
    }
}

int System::nextTask() { return std::max(PRESERVED_TASK_TAGS, 2); }

int System::auxiliaryTask(int k) {
    const int task = nextTask() + k;
    if (k < 1 || task >= (1 << Conf::TASK_TAG_BITS)) {
        throw std::out_of_range("System::auxiliaryTask: task tag " + std::to_string(task) + " does not fit in " +
                                std::to_string(Conf::TASK_TAG_BITS) +
                                " task-tag bits; reduce --bmt_queue_num or "
                                "raise --task_tag_bits.");
    }
    return task;
}

int64_t System::currentTimeMillis() {
    auto now = std::chrono::system_clock::now();

    auto duration = now.time_since_epoch();
    long millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    return millis;
}

} // namespace parsecdb
