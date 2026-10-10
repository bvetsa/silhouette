#pragma once

#include "silhouette/service_graph.h"

#include <string>

namespace silhouette::rendering {

struct SvgRenderResult final {
    std::string svg;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept { return error.empty(); }
};

class ServiceGraphRenderer {
public:
    virtual ~ServiceGraphRenderer() = default;
    [[nodiscard]] virtual SvgRenderResult RenderSvg(
        const ServiceGraph& graph) = 0;

protected:
    ServiceGraphRenderer() = default;
};

class GraphvizServiceGraphRenderer final : public ServiceGraphRenderer {
public:
    [[nodiscard]] SvgRenderResult RenderSvg(
        const ServiceGraph& graph) override;
};

} // namespace silhouette::rendering
