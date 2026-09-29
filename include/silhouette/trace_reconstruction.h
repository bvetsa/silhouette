#pragma once

#include "silhouette/span.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace silhouette {

enum class ParentResolution {
    root,
    resolved,
    missing,
    ambiguous,
    cycle,
};

struct ReconstructedSpan final {
    Span span;
    ParentResolution parent_resolution;
    std::optional<std::size_t> parent_index;
    std::vector<std::size_t> child_indices;
    bool duplicate_span_id;
};

struct ReconstructedTrace final {
    TraceId trace_id;
    std::vector<ReconstructedSpan> spans;
    std::vector<std::size_t> top_level_indices;
};

// Indices express relationships within one returned trace. Their numeric values
// and the storage order of spans are implementation details.
[[nodiscard]] std::vector<ReconstructedTrace> ReconstructTraces(
    std::vector<Span> spans);

// Expects the structural invariants established by ReconstructTraces, including
// valid parent, child, and top-level indices. This function formats those
// results; it does not validate arbitrary ReconstructedTrace values.
[[nodiscard]] std::string FormatReconstructedTraces(
    std::span<const ReconstructedTrace> traces);

} // namespace silhouette
