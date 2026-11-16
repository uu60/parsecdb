
#ifndef ABSTRACTBLOCKINGQUEUE_H
#define ABSTRACTBLOCKINGQUEUE_H

#include <cstddef>

namespace parsecdb {

template <typename T> class AbstractBlockingQueue {
  public:
    virtual ~AbstractBlockingQueue() = default;

    virtual void offer(T item) = 0;

    virtual T poll() = 0;

    // Unblocks producers and consumers during shutdown. After close(), offer() returns without enqueueing and poll()
    // throws once no queued item remains.
    virtual void close() = 0;

    [[nodiscard]] virtual size_t size() const = 0;

    [[nodiscard]] virtual size_t capacity() const = 0;
};

} // namespace parsecdb

#endif
