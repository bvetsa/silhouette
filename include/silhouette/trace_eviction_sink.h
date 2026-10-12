#pragma once

#include "silhouette/span.h"

#include <vector>

namespace silhouette {

class TraceEvictionSink {
public:
    virtual ~TraceEvictionSink() = default;

    // Latest-state invalidations, not deletion commands. An ID may already
    // have reappeared; consumers must look up current authoritative state.
    // Called after manager unlock; enqueue work without waiting for presentation.
    virtual void NotifyTracesEvicted(std::vector<TraceId> trace_ids) noexcept = 0;
};

} // namespace silhouette
