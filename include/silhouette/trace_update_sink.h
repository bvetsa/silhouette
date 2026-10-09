#pragma once

#include "silhouette/span.h"

#include <vector>

namespace silhouette {

class TraceUpdateSink {
public:
    virtual ~TraceUpdateSink() = default;

    TraceUpdateSink(const TraceUpdateSink&) = delete;
    TraceUpdateSink& operator=(const TraceUpdateSink&) = delete;
    TraceUpdateSink(TraceUpdateSink&&) = delete;
    TraceUpdateSink& operator=(TraceUpdateSink&&) = delete;

    // IDs are invalidations: consumers must read their latest authoritative
    // state rather than treating this as an immutable export snapshot.
    virtual void NotifyTracesDirty(std::vector<TraceId> trace_ids) noexcept = 0;

protected:
    TraceUpdateSink() = default;
};

} // namespace silhouette
