#pragma once

#include "silhouette/service_graph.h"

#include <string>

namespace silhouette::rendering {

// Pure presentation boundary for graphs produced by BuildServiceGraph. Expects
// its ordered, unique services/edges/gaps and known endpoints listed in services.
// Gap kind affects appearance only; connections use only the stored endpoints.
[[nodiscard]] std::string FormatServiceGraphDot(const ServiceGraph& graph);

} // namespace silhouette::rendering
