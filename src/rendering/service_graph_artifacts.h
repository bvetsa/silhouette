#pragma once

#include "silhouette/service_graph.h"

#include <string>

namespace silhouette::rendering {

inline constexpr auto kServiceGraphDotFilename = "silhouette.dot";
inline constexpr auto kServiceGraphSvgFilename = "silhouette.svg";

struct ServiceGraphArtifactResult final {
    bool svg_rendered;
    std::string render_error;
};

// V1 local CLI convenience only: overwrites fixed files in the current directory
// and runs Graphviz. Reusable renderers should consume ServiceGraph or the pure
// FormatServiceGraphDot boundary instead. DOT write failures throw; SVG failures
// return an error, retain DOT, and remove stale/partial SVG where possible.
[[nodiscard]] ServiceGraphArtifactResult WriteServiceGraphArtifacts(
    const ServiceGraph& graph);

} // namespace silhouette::rendering
