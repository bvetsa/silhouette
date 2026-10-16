#pragma once

#include "silhouette/span.h"

#include <vector>

namespace silhouette {

class TraceLifecycleSink {
public:
    virtual ~TraceLifecycleSink() = default;
    // Latest-state invalidations, not commands to apply an old transition.
    // Only newly quiescent survivors of the sweep are included.
    virtual void NotifyTracesQuiesced(std::vector<TraceId> trace_ids) noexcept = 0;
};

} // namespace silhouette
