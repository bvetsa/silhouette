#include "silhouette/service_graph.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using silhouette::BuildServiceGraph;
using silhouette::FormatServiceGraph;
using silhouette::ReconstructTraces;
using silhouette::ServiceEdge;
using silhouette::ServiceGraph;
using silhouette::ServiceGraphGap;
using silhouette::ServiceGraphGapKind;
using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

TraceId MakeTraceId(const std::uint8_t marker)
{
    TraceId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    auto id = TraceId::FromBytes(bytes);
    Require(id.has_value(), "test trace ID was invalid");
    return *id;
}

SpanId MakeSpanId(const std::uint8_t marker)
{
    SpanId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    auto id = SpanId::FromBytes(bytes);
    Require(id.has_value(), "test span ID was invalid");
    return *id;
}

Span MakeSpan(
    const std::uint8_t trace_marker,
    const std::uint8_t span_marker,
    const std::optional<std::uint8_t> parent_marker,
    const std::uint64_t start_time,
    std::string operation_name,
    std::optional<std::string> service_name)
{
    return Span{
        .trace_id = MakeTraceId(trace_marker),
        .span_id = MakeSpanId(span_marker),
        .parent_span_id = parent_marker
            ? std::optional<SpanId>{MakeSpanId(*parent_marker)}
            : std::nullopt,
        .service_name = std::move(service_name),
        .operation_name = std::move(operation_name),
        .start_time_unix_nano = start_time,
        .end_time_unix_nano = start_time + 1,
    };
}

ServiceGraph BuildGraph(std::vector<Span> spans)
{
    const auto traces = ReconstructTraces(std::move(spans));
    return BuildServiceGraph(traces);
}

bool GraphsEqual(const ServiceGraph& left, const ServiceGraph& right)
{
    return left.services == right.services && left.edges == right.edges
        && left.gaps == right.gaps;
}

bool HasEdge(
    const ServiceGraph& graph,
    const std::string_view source,
    const std::string_view destination)
{
    return std::find(
               graph.edges.begin(),
               graph.edges.end(),
               ServiceEdge{
                   .source_service = std::string{source},
                   .destination_service = std::string{destination},
               })
        != graph.edges.end();
}

bool HasGap(
    const ServiceGraph& graph,
    const ServiceGraphGapKind kind,
    std::optional<std::string> source,
    std::optional<std::string> destination)
{
    return std::find(
               graph.gaps.begin(),
               graph.gaps.end(),
               ServiceGraphGap{
                   .kind = kind,
                   .source_service = std::move(source),
                   .destination_service = std::move(destination),
               })
        != graph.gaps.end();
}

void EmptyGraphFormatsAllSections()
{
    const auto graph = BuildServiceGraph(
        std::span<const silhouette::ReconstructedTrace>{});
    Require(graph.services.empty() && graph.edges.empty() && graph.gaps.empty(),
            "empty input did not produce an empty graph");
    Require(
        FormatServiceGraph(graph)
            == "Service graph\n"
               "Services:\n"
               "  (none)\n"
               "Edges:\n"
               "  (none)\n"
               "Gaps:\n"
               "  (none)\n",
        "empty graph formatting changed");
}

void KnownServicesAndCrossServiceEdgesAreAggregated()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "solo", std::string{"solo"}),
        MakeSpan(2, 1, std::nullopt, 10, "root", std::string{"A"}),
        MakeSpan(2, 2, 1, 20, "middle", std::string{"B"}),
        MakeSpan(2, 3, 2, 30, "leaf", std::string{"C"}),
    });

    Require(graph.services == std::vector<std::string>{"A", "B", "C", "solo"},
            "known services were not retained in lexical order");
    Require(graph.edges
                == std::vector<ServiceEdge>{{"A", "B"}, {"B", "C"}},
            "cross-service chain was not aggregated");
    Require(graph.gaps.empty(), "complete service data produced a gap");
}

void RepeatedAndSameServiceRelationshipsAreCollapsed()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "root-one", std::string{"A"}),
        MakeSpan(1, 2, 1, 20, "first-b", std::string{"B"}),
        MakeSpan(1, 3, 1, 30, "second-b", std::string{"B"}),
        MakeSpan(1, 4, 2, 40, "internal-b", std::string{"B"}),
        MakeSpan(2, 1, std::nullopt, 10, "root-two", std::string{"A"}),
        MakeSpan(2, 2, 1, 20, "third-b", std::string{"B"}),
    });

    Require(graph.edges == std::vector<ServiceEdge>{{"A", "B"}},
            "repeated edges were not deduplicated");
    Require(!HasEdge(graph, "B", "B"),
            "same-service relationship produced a self-edge");
}

void IsolatedServicesAndComponentsAreRetained()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "isolated", std::string{"isolated"}),
        MakeSpan(2, 1, std::nullopt, 10, "x-root", std::string{"X"}),
        MakeSpan(2, 2, 1, 20, "y-child", std::string{"Y"}),
        MakeSpan(3, 1, std::nullopt, 10, "p-root", std::string{"P"}),
        MakeSpan(3, 2, 1, 20, "q-child", std::string{"Q"}),
    });

    Require(
        graph.services
            == std::vector<std::string>{"P", "Q", "X", "Y", "isolated"},
        "isolated or disconnected services were lost");
    Require(graph.edges
                == std::vector<ServiceEdge>{{"P", "Q"}, {"X", "Y"}},
            "disconnected components were not retained");
}

void DuplicateIdsFollowReconstructionDecisions()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "root", std::string{"A"}),
        MakeSpan(1, 2, 1, 20, "duplicate-one", std::string{"B"}),
        MakeSpan(1, 2, 1, 30, "duplicate-two", std::string{"B"}),
        MakeSpan(1, 3, 2, 40, "ambiguous-child", std::string{"C"}),
    });

    Require(graph.edges == std::vector<ServiceEdge>{{"A", "B"}},
            "duplicate spans lost their independently resolved relationship");
    Require(!HasEdge(graph, "B", "C"),
            "ambiguous child produced a service edge");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::ambiguous_parent,
            std::nullopt,
            std::string{"C"}),
        "ambiguous parent evidence was not retained");
}

void MissingServiceGapsPreserveKnownEndpoints()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "missing-source", std::nullopt),
        MakeSpan(1, 2, 1, 20, "known-child", std::string{"B"}),
        MakeSpan(2, 1, std::nullopt, 10, "known-root", std::string{"A"}),
        MakeSpan(2, 2, 1, 20, "missing-child", std::string{}),
        MakeSpan(3, 1, std::nullopt, 10, "both-source", std::nullopt),
        MakeSpan(3, 2, 1, 20, "both-child", std::nullopt),
    });

    Require(graph.services == std::vector<std::string>{"A", "B"},
            "empty service identity was treated as known");
    Require(graph.edges.empty(), "missing service endpoint produced an edge");
    Require(
        graph.gaps
            == std::vector<ServiceGraphGap>{
                {ServiceGraphGapKind::missing_service,
                 std::nullopt,
                 std::nullopt},
                {ServiceGraphGapKind::missing_service,
                 std::nullopt,
                 std::string{"B"}},
                {ServiceGraphGapKind::missing_service,
                 std::string{"A"},
                 std::nullopt},
            },
        "missing-service facts were not ordered and deduplicated");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::missing_service,
            std::nullopt,
            std::string{"B"}),
        "known destination was lost from missing-service gap");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::missing_service,
            std::string{"A"},
            std::nullopt),
        "known source was lost from missing-service gap");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::missing_service,
            std::nullopt,
            std::nullopt),
        "fully unknown resolved relationship was not retained");
}

void EndpointlessMissingServiceIsNotRedundant()
{
    const auto standalone = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "standalone", std::nullopt),
    });
    Require(standalone.gaps
                == std::vector<ServiceGraphGap>{{
                    ServiceGraphGapKind::missing_service,
                    std::nullopt,
                    std::nullopt,
                }},
            "isolated missing service did not produce one endpoint-less fact");

    const auto contextual = BuildGraph({
        MakeSpan(1, 1, std::nullopt, 10, "missing-parent-service", std::nullopt),
        MakeSpan(1, 2, 1, 20, "known-child", std::string{"B"}),
    });
    Require(contextual.gaps
                == std::vector<ServiceGraphGap>{{
                    ServiceGraphGapKind::missing_service,
                    std::nullopt,
                    std::string{"B"},
                }},
            "resolved relationship gained a redundant endpoint-less fact");

    const auto unresolved = BuildGraph({
        MakeSpan(1, 1, 9, 10, "unresolved", std::nullopt),
    });
    Require(
        HasGap(
            unresolved,
            ServiceGraphGapKind::missing_service,
            std::nullopt,
            std::nullopt)
            && HasGap(
                unresolved,
                ServiceGraphGapKind::missing_parent,
                std::nullopt,
                std::nullopt),
        "unresolved missing-service span did not preserve both facts");

    const auto unresolved_with_child = BuildGraph({
        MakeSpan(1, 1, 9, 10, "unresolved", std::nullopt),
        MakeSpan(1, 2, 1, 20, "resolved-child", std::string{"C"}),
    });
    Require(unresolved_with_child.gaps.size() == 2,
            "resolved child did not suppress only the standalone fact");
    Require(
        HasGap(
            unresolved_with_child,
            ServiceGraphGapKind::missing_service,
            std::nullopt,
            std::string{"C"})
            && HasGap(
                unresolved_with_child,
                ServiceGraphGapKind::missing_parent,
                std::nullopt,
                std::nullopt),
        "relationship-level and parent-resolution evidence were not preserved");
}

void ParentResolutionGapsPreserveReasons()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, 9, 10, "missing", std::string{"missing-child"}),
        MakeSpan(2, 2, std::nullopt, 10, "duplicate-one", std::string{"parent"}),
        MakeSpan(2, 2, std::nullopt, 20, "duplicate-two", std::string{"parent"}),
        MakeSpan(2, 3, 2, 30, "ambiguous", std::string{"ambiguous-child"}),
    });

    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::missing_parent,
            std::nullopt,
            std::string{"missing-child"}),
        "missing-parent fact was not retained");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::ambiguous_parent,
            std::nullopt,
            std::string{"ambiguous-child"}),
        "ambiguous-parent fact was not retained");
    Require(graph.edges.empty(), "unresolved parent claim produced an edge");
}

void CycleGapsKeepSourceUnknownAndPreserveChildren()
{
    const auto graph = BuildGraph({
        MakeSpan(1, 1, 2, 10, "cycle-a", std::string{"A"}),
        MakeSpan(1, 2, 1, 20, "cycle-b", std::string{"B"}),
        MakeSpan(1, 3, 1, 30, "non-cycle-child", std::string{"C"}),
    });

    Require(graph.edges == std::vector<ServiceEdge>{{"A", "C"}},
            "resolved non-cycle child of a cycle member was lost");
    Require(!HasEdge(graph, "A", "B") && !HasEdge(graph, "B", "A"),
            "detached cycle relationship produced an edge");
    Require(
        HasGap(
            graph,
            ServiceGraphGapKind::cycle_parent,
            std::nullopt,
            std::string{"A"})
            && HasGap(
                graph,
                ServiceGraphGapKind::cycle_parent,
                std::nullopt,
                std::string{"B"}),
        "cycle-parent facts did not leave their source unknown");
    Require(graph.gaps.size() == 2,
            "cycle aggregation added an inferred parent fact");
}

void OutputIsStableAcrossInputPermutations()
{
    const std::vector<Span> spans{
        MakeSpan(2, 1, std::nullopt, 10, "missing-service-root", std::nullopt),
        MakeSpan(1, 2, 1, 20, "known-child", std::string{"A"}),
        MakeSpan(1, 1, std::nullopt, 10, "known-root", std::string{"B"}),
        MakeSpan(2, 2, 1, 20, "known-service-child", std::string{"D"}),
        MakeSpan(1, 3, 9, 30, "missing-parent", std::string{"C"}),
        MakeSpan(3, 1, std::nullopt, 10, "repeated-gap-root", std::nullopt),
        MakeSpan(3, 2, 1, 20, "repeated-gap-child", std::string{"D"}),
        MakeSpan(4, 1, 9, 10, "repeated-missing-parent", std::string{"C"}),
    };

    auto reversed = spans;
    std::reverse(reversed.begin(), reversed.end());
    auto rotated = spans;
    std::rotate(rotated.begin(), rotated.begin() + 2, rotated.end());

    const auto first = BuildGraph(spans);
    const auto second = BuildGraph(std::move(reversed));
    const auto third = BuildGraph(std::move(rotated));
    Require(GraphsEqual(first, second) && GraphsEqual(first, third),
            "aggregated graph depended on span arrival order");
    Require(FormatServiceGraph(first) == FormatServiceGraph(second)
                && FormatServiceGraph(first) == FormatServiceGraph(third),
            "formatted graph depended on span arrival order");

    Require(
        FormatServiceGraph(first)
            == "Service graph\n"
               "Services:\n"
               "  - A\n"
               "  - B\n"
               "  - C\n"
               "  - D\n"
               "Edges:\n"
               "  - B -> A\n"
               "Gaps:\n"
               "  - missing_service: <unknown-service> -> D\n"
               "  - missing_parent: <unknown-service> -> C\n",
        "non-empty service graph formatting changed");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"empty_graph_formats_all_sections", EmptyGraphFormatsAllSections},
    TestCase{"known_services_and_cross_service_edges_are_aggregated",
             KnownServicesAndCrossServiceEdgesAreAggregated},
    TestCase{"repeated_and_same_service_relationships_are_collapsed",
             RepeatedAndSameServiceRelationshipsAreCollapsed},
    TestCase{"isolated_services_and_components_are_retained",
             IsolatedServicesAndComponentsAreRetained},
    TestCase{"duplicate_ids_follow_reconstruction_decisions",
             DuplicateIdsFollowReconstructionDecisions},
    TestCase{"missing_service_gaps_preserve_known_endpoints",
             MissingServiceGapsPreserveKnownEndpoints},
    TestCase{"endpointless_missing_service_is_not_redundant",
             EndpointlessMissingServiceIsNotRedundant},
    TestCase{"parent_resolution_gaps_preserve_reasons",
             ParentResolutionGapsPreserveReasons},
    TestCase{"cycle_gaps_keep_source_unknown_and_preserve_children",
             CycleGapsKeepSourceUnknownAndPreserveChildren},
    TestCase{"output_is_stable_across_input_permutations",
             OutputIsStableAcrossInputPermutations},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one service-graph test-case name.\n";
        return 1;
    }

    const std::string_view requested_case{argv[1]};
    for (const auto& test_case : kTestCases) {
        if (test_case.name != requested_case) {
            continue;
        }

        try {
            test_case.run();
            std::cout << test_case.name << " passed.\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << test_case.name << " failed: " << error.what() << '\n';
            return 1;
        }
    }

    std::cerr << "Unknown service-graph test case: " << requested_case << '\n';
    return 1;
}
