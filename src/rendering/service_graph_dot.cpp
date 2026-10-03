#include "silhouette/rendering/service_graph_dot.h"

#include <array>
#include <map>
#include <stdexcept>
#include <string_view>

namespace silhouette::rendering {
namespace {

struct GapAppearance final {
    ServiceGraphGapKind kind;
    std::string_view label;
    std::string_view shape;
    std::string_view color;
    std::string_view fill;
};

constexpr std::array kGapAppearances{
    GapAppearance{ServiceGraphGapKind::missing_service,
                  "Missing\nservice identity", "diamond", "#b45309", "#fef3c7"},
    GapAppearance{ServiceGraphGapKind::missing_parent,
                  "Missing\nparent", "triangle", "#b91c1c", "#fee2e2"},
    GapAppearance{ServiceGraphGapKind::ambiguous_parent,
                  "Ambiguous\nparent", "hexagon", "#7e22ce", "#f3e8ff"},
    GapAppearance{ServiceGraphGapKind::cycle_parent,
                  "Cycle\nparent", "octagon", "#0f766e", "#ccfbf1"},
};

std::string EscapeLabel(const std::string_view label)
{
    std::string escaped;
    for (const unsigned char character : label) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        // Graphviz's \r changes alignment and \t drops the backslash. Display
        // these controls explicitly instead of interpreting them as markup.
        case '\r': escaped += "\\\\r"; break;
        case '\t': escaped += "\\\\t"; break;
        default:
            if (character < 0x20) {
                constexpr std::string_view hex{"0123456789abcdef"};
                escaped += "\\\\x";
                escaped += hex[character >> 4];
                escaped += hex[character & 0xf];
            } else {
                escaped += static_cast<char>(character);
            }
        }
    }
    return escaped;
}

const GapAppearance& AppearanceFor(const ServiceGraphGapKind kind)
{
    for (const auto& appearance : kGapAppearances) {
        if (appearance.kind == kind) {
            return appearance;
        }
    }
    throw std::invalid_argument{"unrecognized service graph gap kind"};
}

void AppendGapNode(
    const std::string_view id,
    const GapAppearance& appearance,
    const std::string_view indent,
    std::string& output)
{
    output += indent;
    output += id;
    output += " [label=\"";
    output += EscapeLabel(appearance.label);
    output += "\", shape=";
    output += appearance.shape;
    output += ", style=filled, color=\"";
    output += appearance.color;
    output += "\", fillcolor=\"";
    output += appearance.fill;
    output += "\", fontsize=10];\n";
}

void AppendDiagnosticEdge(
    const std::string_view source,
    const std::string_view destination,
    const GapAppearance& appearance,
    std::string& output)
{
    output += "  ";
    output += source;
    output += " -> ";
    output += destination;
    output += " [style=dashed, color=\"";
    output += appearance.color;
    output += "\"];\n";
}

void AppendLegend(std::string& output)
{
    output += "\n  subgraph cluster_legend {\n"
              "    label=\"Legend\"; color=\"#cbd5e1\"; fontsize=12;\n"
              "    legend_service [label=\"Known observed service\"];\n"
              "    legend_target [label=\"Known observed service\"];\n"
              "    legend_service -> legend_target "
              "[label=\"Confirmed observed dependency\", fontsize=10];\n";
    for (std::size_t index = 0; index < kGapAppearances.size(); ++index) {
        AppendGapNode(
            "legend_gap_" + std::to_string(index),
            kGapAppearances[index], "    ", output);
    }
    output += "    legend_gap_0 -> legend_target "
              "[style=dashed, color=\"#b45309\", "
              "label=\"Diagnostic / incomplete relationship\", fontsize=10];\n"
              "  }\n";
}

} // namespace

std::string FormatServiceGraphDot(const ServiceGraph& graph)
{
    std::string output{
        "digraph silhouette {\n"
        "  graph [rankdir=LR, bgcolor=\"white\", fontname=\"Helvetica\", "
        "label=\"Silhouette observed service graph\", labelloc=t, "
        "pad=0.3, nodesep=0.5, ranksep=0.8];\n"
        "  node [shape=box, style=\"rounded,filled\", color=\"#2563eb\", "
        "fillcolor=\"#eff6ff\", fontname=\"Helvetica\", fontsize=12];\n"
        "  edge [color=\"#334155\", style=solid, fontname=\"Helvetica\", "
        "arrowsize=0.8];\n"};

    std::map<std::string_view, std::string> service_ids;
    for (std::size_t index = 0; index < graph.services.size(); ++index) {
        const auto& service = graph.services[index];
        const auto id = "service_" + std::to_string(index);
        service_ids.emplace(service, id);
        output += "  " + id + " [label=\"" + EscapeLabel(service) + "\"];\n";
    }

    for (const auto& edge : graph.edges) {
        output += "  " + service_ids.at(edge.source_service) + " -> "
            + service_ids.at(edge.destination_service) + ";\n";
    }

    for (std::size_t index = 0; index < graph.gaps.size(); ++index) {
        const auto& gap = graph.gaps[index];
        const auto id = "gap_" + std::to_string(index);
        const auto& appearance = AppearanceFor(gap.kind);
        AppendGapNode(id, appearance, "  ", output);
        if (gap.source_service) {
            AppendDiagnosticEdge(
                service_ids.at(*gap.source_service), id, appearance, output);
        }
        if (gap.destination_service) {
            AppendDiagnosticEdge(
                id, service_ids.at(*gap.destination_service), appearance, output);
        }
    }

    AppendLegend(output);
    output += "}\n";
    return output;
}

} // namespace silhouette::rendering
