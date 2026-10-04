#include "bmrf/dmr/call.hpp"

namespace bmrf {

uint64_t DMRCall::durationMs() const
{
    if (last_frame_timestamp_ns <= start_timestamp_ns)
        return 0;
    return (last_frame_timestamp_ns - start_timestamp_ns) / 1000000ULL;
}

} // namespace bmrf
