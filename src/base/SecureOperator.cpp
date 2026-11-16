
#include "parsecdb/base/SecureOperator.h"

#include "parsecdb/conf/Conf.h"
#include "parsecdb/ot/RandOtBatchOperator.h"
#include "parsecdb/parallel/ThreadPoolSupport.h"
#include "parsecdb/utils/Math.h"
#include <stdexcept>
#include <string>

namespace parsecdb {

void SecureOperator::requireSupportedWidth(int width) {
    if (width < 1 || width > 64 || (width & (width - 1)) != 0) {
        throw std::invalid_argument("Unsupported share width " + std::to_string(width) +
                                    ": only powers of two from 1 to 64 are supported.");
    }
}

int SecureOperator::buildTag(int msgTag) const {
    const int bits = 32 - Conf::TASK_TAG_BITS;
    const uint32_t msgMask = (uint32_t{1} << bits) - 1;
    const uint32_t taskMask = (uint32_t{1} << Conf::TASK_TAG_BITS) - 1;

    // Primitive code passes a logical task number in _taskTag, while the database layer passes an already encoded
    // task prefix in msgTag (and occasionally in _taskTag). Both conventions are preserved: dropping the high bits of
    // msgTag would alias database traffic with task 0, which the background BMT generators use.
    const auto rawTask = static_cast<uint32_t>(_taskTag);
    const uint32_t taskPrefix = rawTask <= taskMask ? rawTask << bits : rawTask & ~msgMask;
    const auto rawMsg = static_cast<uint32_t>(msgTag);
    return static_cast<int>(taskPrefix | rawMsg);
}

int64_t SecureOperator::ring(int64_t raw) const { return Math::ring(raw, _width); }

} // namespace parsecdb
