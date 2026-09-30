#include "silhouette/service_graph.h"

#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

namespace silhouette {

namespace {

std::optional<std::string> KnownService(const Span& span)
{
    if (!span.service_name || span.service_name->empty()) {
        return std::nullopt;
    }
    return span.service_name;
}

std::optional<ServiceGraphGapKind> GapKindFor(
    const ParentResolution resolution)
{
    switch (resolution) {
    case ParentResolution::missing:
        return ServiceGraphGapKind::missing_parent;
    case ParentResolution::ambiguous:
        return ServiceGraphGapKind::ambiguous_parent;
    case ParentResolution::cycle:
        return ServiceGraphGapKind::cycle_parent;
    case ParentResolution::root:
    case ParentResolution::resolved:
        return std::nullopt;
    }

    return std::nullopt;
}

std::string_view GapKindName(const ServiceGraphGapKind kind)
{
    switch (kind) {
    case ServiceGraphGapKind::missing_service:
        return "missing_service";
    case ServiceGraphGapKind::missing_parent:
        return "missing_parent";
    case ServiceGraphGapKind::ambiguous_parent:
        return "ambiguous_parent";
    case ServiceGraphGapKind::cycle_parent:
        return "cycle_parent";
    }

    return "unknown_gap";
}

void AppendServiceEndpoint(
    const std::optional<std::string>& service,
    std::string& output)
{
    output += service.value_or("<unknown-service>");
}

} // namespace

ServiceGraph BuildServiceGraph(
    const std::span<const ReconstructedTrace> traces)
{
    std::set<std::string> services;
    std::set<ServiceEdge> edges;
    std::set<ServiceGraphGap> gaps;

    for (const auto& trace : traces) {
        std::vector<bool> missing_service_has_resolved_relationship(
            trace.spans.size(), false);

        for (const auto& node : trace.spans) {
            if (const auto service = KnownService(node.span)) {
                services.insert(*service);
            }
        }

        for (std::size_t child_index = 0;
             child_index < trace.spans.size();
             ++child_index) {
            const auto& child = trace.spans[child_index];
            const auto child_service = KnownService(child.span);

            if (child.parent_resolution == ParentResolution::resolved) {
                const auto parent_index = *child.parent_index;
                const auto parent_service =
                    KnownService(trace.spans[parent_index].span);

                if (!parent_service) {
                    missing_service_has_resolved_relationship[parent_index] = true;
                }
                if (!child_service) {
                    missing_service_has_resolved_relationship[child_index] = true;
                }

                if (!parent_service || !child_service) {
                    gaps.insert(ServiceGraphGap{
                        .kind = ServiceGraphGapKind::missing_service,
                        .source_service = parent_service,
                        .destination_service = child_service,
                    });
                } else if (*parent_service != *child_service) {
                    edges.insert(ServiceEdge{
                        .source_service = *parent_service,
                        .destination_service = *child_service,
                    });
                }
                continue;
            }

            if (const auto gap_kind = GapKindFor(child.parent_resolution)) {
                gaps.insert(ServiceGraphGap{
                    .kind = *gap_kind,
                    .source_service = std::nullopt,
                    .destination_service = child_service,
                });
            }
        }

        for (std::size_t index = 0; index < trace.spans.size(); ++index) {
            if (!KnownService(trace.spans[index].span)
                && !missing_service_has_resolved_relationship[index]) {
                gaps.insert(ServiceGraphGap{
                    .kind = ServiceGraphGapKind::missing_service,
                    .source_service = std::nullopt,
                    .destination_service = std::nullopt,
                });
            }
        }
    }

    return ServiceGraph{
        .services = {services.begin(), services.end()},
        .edges = {edges.begin(), edges.end()},
        .gaps = {gaps.begin(), gaps.end()},
    };
}

std::string FormatServiceGraph(const ServiceGraph& graph)
{
    std::string output{"Service graph\nServices:\n"};
    if (graph.services.empty()) {
        output += "  (none)\n";
    } else {
        for (const auto& service : graph.services) {
            output += "  - ";
            output += service;
            output += '\n';
        }
    }

    output += "Edges:\n";
    if (graph.edges.empty()) {
        output += "  (none)\n";
    } else {
        for (const auto& edge : graph.edges) {
            output += "  - ";
            output += edge.source_service;
            output += " -> ";
            output += edge.destination_service;
            output += '\n';
        }
    }

    output += "Gaps:\n";
    if (graph.gaps.empty()) {
        output += "  (none)\n";
    } else {
        for (const auto& gap : graph.gaps) {
            output += "  - ";
            output += GapKindName(gap.kind);
            output += ": ";
            AppendServiceEndpoint(gap.source_service, output);
            output += " -> ";
            AppendServiceEndpoint(gap.destination_service, output);
            output += '\n';
        }
    }

    return output;
}

} // namespace silhouette
