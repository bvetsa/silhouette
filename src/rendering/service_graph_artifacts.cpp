#include "rendering/service_graph_artifacts.h"

#include "silhouette/rendering/service_graph_dot.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace silhouette::rendering {
namespace {

std::string RemoveSvg()
{
    std::error_code error;
    std::filesystem::remove(kServiceGraphSvgFilename, error);
    if (error) {
        return "could not remove stale/partial silhouette.svg: " + error.message();
    }
    return {};
}

ServiceGraphArtifactResult RenderingFailed(std::string message)
{
    if (const auto cleanup_error = RemoveSvg(); !cleanup_error.empty()) {
        message += "; " + cleanup_error;
    }
    return {.svg_rendered = false, .render_error = std::move(message)};
}

} // namespace

ServiceGraphArtifactResult WriteServiceGraphArtifacts(const ServiceGraph& graph)
{
    const auto dot = FormatServiceGraphDot(graph);
    try {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(kServiceGraphDotFilename, std::ios::binary | std::ios::trunc);
        file << dot;
        file.close();
    } catch (const std::ios_base::failure& error) {
        throw std::runtime_error{
            std::string{"could not write silhouette.dot: "} + error.what()};
    }

    if (const auto error = RemoveSvg(); !error.empty()) {
        return {.svg_rendered = false,
                .render_error = "Graphviz SVG rendering was not started: " + error};
    }

    // Every argument and filename is fixed. No service, label, endpoint, or other
    // user-derived text is interpolated into the shell command.
    const auto status = std::system("dot -Tsvg silhouette.dot -o silhouette.svg");
    if (status != 0) {
        return RenderingFailed(
            "Graphviz `dot` failed to render silhouette.svg (command status "
            + std::to_string(status) + "). Ensure Graphviz is installed on PATH");
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(kServiceGraphSvgFilename, error);
    if (error || size == 0) {
        return RenderingFailed(
            "Graphviz `dot` did not produce a nonempty silhouette.svg");
    }
    return {.svg_rendered = true, .render_error = {}};
}

} // namespace silhouette::rendering
