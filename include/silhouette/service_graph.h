#pragma once

#include "silhouette/trace_reconstruction.h"

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace silhouette {

struct ServiceEdge final {
    std::string source_service;
    std::string destination_service;

    bool operator==(const ServiceEdge&) const = default;
    auto operator<=>(const ServiceEdge&) const = default;
};

enum class ServiceGraphGapKind {
    missing_service,
    missing_parent,
    ambiguous_parent,
    cycle_parent,
};

struct ServiceGraphGap final {
    ServiceGraphGapKind kind;
    std::optional<std::string> source_service;
    std::optional<std::string> destination_service;

    bool operator==(const ServiceGraphGap&) const = default;
    auto operator<=>(const ServiceGraphGap&) const = default;
};

struct ServiceGraph final {
    std::vector<std::string> services;
    std::vector<ServiceEdge> edges;
    std::vector<ServiceGraphGap> gaps;
};

// Expects the structural invariants established by ReconstructTraces. This
// function consumes reconstruction decisions; it does not re-resolve original
// parent IDs or validate arbitrary ReconstructedTrace values.
[[nodiscard]] ServiceGraph BuildServiceGraph(
    std::span<const ReconstructedTrace> traces);

// Formats an already aggregated graph without modifying or validating it.
[[nodiscard]] std::string FormatServiceGraph(const ServiceGraph& graph);

} // namespace silhouette
