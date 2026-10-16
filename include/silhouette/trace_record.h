#pragma once

#include "silhouette/span.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <vector>

namespace silhouette {

enum class TraceLifecycle { active, quiescent };
using TraceIncarnationId = std::uint64_t;

struct TraceRecordKey final {
    TraceId trace_id;
    TraceIncarnationId incarnation;
    auto operator<=>(const TraceRecordKey&) const = default;
};

struct TraceRecordRevision final {
    TraceRecordKey key;
    std::uint64_t publication_sequence;
};

struct TraceRecordMetadata final {
    TraceRecordKey key;
    TraceLifecycle lifecycle;
    std::uint64_t publication_sequence;
    std::chrono::steady_clock::time_point last_evidence_time;
    // Computed by the manager's injected clock, never by a query consumer.
    std::chrono::steady_clock::duration evidence_age;
    std::size_t span_count;
};

struct TraceRecordSnapshot final {
    TraceRecordMetadata metadata;
    std::vector<Span> evidence;
};

} // namespace silhouette
