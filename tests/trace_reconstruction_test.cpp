#include "silhouette/trace_reconstruction.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using silhouette::FormatReconstructedTraces;
using silhouette::ParentResolution;
using silhouette::ReconstructedSpan;
using silhouette::ReconstructedTrace;
using silhouette::ReconstructTraces;
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
    std::optional<std::string> service_name = std::string{"service"})
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

const ReconstructedTrace& FindTrace(
    const std::vector<ReconstructedTrace>& traces,
    const std::uint8_t trace_marker)
{
    const auto trace_id = MakeTraceId(trace_marker);
    const auto found = std::find_if(
        traces.begin(), traces.end(), [&trace_id](const auto& trace) {
            return trace.trace_id == trace_id;
        });
    Require(found != traces.end(), "expected reconstructed trace was missing");
    return *found;
}

const ReconstructedSpan& FindSpan(
    const ReconstructedTrace& trace,
    const std::string_view operation_name)
{
    const auto found = std::find_if(
        trace.spans.begin(),
        trace.spans.end(),
        [operation_name](const auto& span) {
            return span.span.operation_name == operation_name;
        });
    Require(found != trace.spans.end(), "expected reconstructed span was missing");
    return *found;
}

std::vector<std::string> ChildNames(
    const ReconstructedTrace& trace,
    const ReconstructedSpan& parent)
{
    std::vector<std::string> names;
    names.reserve(parent.child_indices.size());
    for (const auto index : parent.child_indices) {
        names.push_back(trace.spans[index].span.operation_name);
    }
    return names;
}

std::vector<std::string> TopLevelNames(const ReconstructedTrace& trace)
{
    std::vector<std::string> names;
    names.reserve(trace.top_level_indices.size());
    for (const auto index : trace.top_level_indices) {
        names.push_back(trace.spans[index].span.operation_name);
    }
    return names;
}

void RequireValidStructure(const ReconstructedTrace& trace)
{
    std::vector<std::size_t> child_appearances(trace.spans.size(), 0);
    std::vector<std::size_t> top_level_appearances(trace.spans.size(), 0);

    for (const auto top_level : trace.top_level_indices) {
        Require(top_level < trace.spans.size(), "top-level index was invalid");
        ++top_level_appearances[top_level];
    }

    for (std::size_t parent = 0; parent < trace.spans.size(); ++parent) {
        for (const auto child : trace.spans[parent].child_indices) {
            Require(child < trace.spans.size(), "child index was invalid");
            Require(
                trace.spans[child].parent_resolution
                    == ParentResolution::resolved,
                "unresolved span appeared as a child");
            Require(
                trace.spans[child].parent_index == parent,
                "child did not point back to its reconstructed parent");
            ++child_appearances[child];
        }
    }

    for (std::size_t index = 0; index < trace.spans.size(); ++index) {
        const auto& span = trace.spans[index];
        if (span.parent_resolution == ParentResolution::resolved) {
            Require(span.parent_index.has_value(), "resolved span had no parent");
            Require(child_appearances[index] == 1,
                    "resolved span did not appear under exactly one parent");
            Require(top_level_appearances[index] == 0,
                    "resolved span also appeared at the top level");
        } else {
            Require(!span.parent_index, "unresolved span retained a parent index");
            Require(child_appearances[index] == 0,
                    "unresolved span appeared under a parent");
            Require(top_level_appearances[index] == 1,
                    "unresolved span did not appear exactly once at the top level");
        }
    }
}

std::size_t CountSubstring(
    const std::string_view text,
    const std::string_view substring)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(substring, position)) != std::string_view::npos) {
        ++count;
        position += substring.size();
    }
    return count;
}

void IdOrderingAndHexFormattingAreCanonical()
{
    const auto lower_trace = MakeTraceId(1);
    const auto higher_trace = MakeTraceId(2);
    const auto span_id = MakeSpanId(0xab);

    Require(lower_trace < higher_trace, "trace IDs were not lexicographically ordered");
    Require(MakeSpanId(1) < MakeSpanId(2),
            "span IDs were not lexicographically ordered");
    Require(lower_trace.ToHex() == std::string(30, '0') + "01",
            "trace ID hex formatting was not canonical");
    Require(span_id.ToHex() == std::string(14, '0') + "ab",
            "span ID hex formatting was not canonical");
}

void EmptyAndSingleRootFormattingIsReadable()
{
    const std::vector<ReconstructedTrace> empty;
    Require(
        FormatReconstructedTraces(empty)
            == "No captured spans to reconstruct.\n",
        "empty reconstruction message changed");

    auto traces = ReconstructTraces(
        {MakeSpan(1, 2, std::nullopt, 10, "root", std::nullopt)});
    Require(traces.size() == 1, "single trace was not reconstructed");
    RequireValidStructure(traces.front());

    const auto expected = std::string{"Trace "} + std::string(30, '0') + "01\n"
        + "└── <unknown-service>: root [span=" + std::string(14, '0')
        + "02]\n";
    const auto formatted = FormatReconstructedTraces(traces);
    Require(formatted == expected, "single-root text output changed");
    Require(formatted.find("missing-service") == std::string::npos,
            "missing service was reported redundantly");
}

void OutOfOrderBranchingReconstructsByIdentity()
{
    auto traces = ReconstructTraces({
        MakeSpan(1, 3, 1, 30, "second-child"),
        MakeSpan(1, 1, std::nullopt, 10, "root"),
        MakeSpan(1, 2, 1, 20, "first-child"),
    });
    const auto& trace = FindTrace(traces, 1);
    RequireValidStructure(trace);

    const auto& root = FindSpan(trace, "root");
    Require(root.parent_resolution == ParentResolution::root,
            "root was not classified as a root");
    Require(ChildNames(trace, root)
                == std::vector<std::string>{"first-child", "second-child"},
            "siblings were not canonically ordered");

    for (const auto name : {"first-child", "second-child"}) {
        const auto& child = FindSpan(trace, name);
        Require(child.parent_resolution == ParentResolution::resolved,
                "child parent was not resolved");
        Require(child.span.parent_span_id == MakeSpanId(1),
                "reconstruction changed the original parent span ID");
        Require(trace.spans[*child.parent_index].span.operation_name == "root",
                "child resolved to the wrong parent");
    }
}

void InterleavedTracesRemainIndependentAndOrdered()
{
    auto traces = ReconstructTraces({
        MakeSpan(2, 7, std::nullopt, 20, "trace-two"),
        MakeSpan(1, 7, std::nullopt, 10, "trace-one"),
    });

    Require(traces.size() == 2, "interleaved traces were not separated");
    Require(traces[0].trace_id == MakeTraceId(1)
                && traces[1].trace_id == MakeTraceId(2),
            "traces were not ordered by trace ID");
    Require(!traces[0].spans.front().duplicate_span_id
                && !traces[1].spans.front().duplicate_span_id,
            "same span ID in different traces was treated as a duplicate");
    RequireValidStructure(traces[0]);
    RequireValidStructure(traces[1]);
}

void MissingParentsAndMultipleRootsRemainTopLevel()
{
    auto traces = ReconstructTraces({
        MakeSpan(1, 3, 9, 30, "orphan"),
        MakeSpan(1, 1, std::nullopt, 20, "later-root"),
        MakeSpan(1, 2, std::nullopt, 10, "earlier-root"),
    });
    const auto& trace = traces.front();
    RequireValidStructure(trace);
    Require(TopLevelNames(trace)
                == std::vector<std::string>{
                    "earlier-root", "later-root", "orphan"},
            "top-level fragments were not canonically ordered");

    const auto& orphan = FindSpan(trace, "orphan");
    Require(orphan.parent_resolution == ParentResolution::missing,
            "missing parent was not classified");
    Require(orphan.span.parent_span_id == MakeSpanId(9),
            "missing parent ID was not preserved");
    Require(FormatReconstructedTraces(traces).find(
                "[missing-parent=0000000000000009]")
                != std::string::npos,
            "missing parent diagnostic was absent");
}

void DuplicateIdsRetainSpansAndMakeChildrenAmbiguous()
{
    auto traces = ReconstructTraces({
        MakeSpan(1, 3, 2, 30, "ambiguous-child"),
        MakeSpan(1, 2, 1, 20, "duplicate-b"),
        MakeSpan(1, 1, std::nullopt, 5, "root"),
        MakeSpan(1, 2, 1, 10, "duplicate-a"),
    });
    const auto& trace = traces.front();
    RequireValidStructure(trace);

    const auto& root = FindSpan(trace, "root");
    Require(ChildNames(trace, root)
                == std::vector<std::string>{"duplicate-a", "duplicate-b"},
            "duplicate spans did not retain their unambiguous parent");
    Require(FindSpan(trace, "duplicate-a").duplicate_span_id
                && FindSpan(trace, "duplicate-b").duplicate_span_id,
            "duplicate spans were not marked");

    const auto& child = FindSpan(trace, "ambiguous-child");
    Require(child.parent_resolution == ParentResolution::ambiguous,
            "child of duplicate ID was not marked ambiguous");
    Require(child.span.parent_span_id == MakeSpanId(2),
            "ambiguous parent ID was not preserved");

    const auto formatted = FormatReconstructedTraces(traces);
    Require(CountSubstring(formatted, "[duplicate-span-id]") == 2,
            "duplicate diagnostics had the wrong count");
    Require(formatted.find("[ambiguous-parent=0000000000000002]")
                != std::string::npos,
            "ambiguous parent diagnostic was absent");
}

void CyclesDetachOnlyCycleEdgesAndPreserveChildren()
{
    auto traces = ReconstructTraces({
        MakeSpan(1, 5, 4, 50, "self-child"),
        MakeSpan(1, 2, 1, 20, "cycle-b"),
        MakeSpan(1, 3, 1, 30, "cycle-child"),
        MakeSpan(1, 4, 4, 40, "self-cycle"),
        MakeSpan(1, 1, 2, 10, "cycle-a"),
    });
    const auto& trace = traces.front();
    RequireValidStructure(trace);

    for (const auto name : {"cycle-a", "cycle-b", "self-cycle"}) {
        const auto& member = FindSpan(trace, name);
        Require(member.parent_resolution == ParentResolution::cycle,
                "cycle member was not marked");
        Require(member.span.parent_span_id.has_value(),
                "cycle handling erased the claimed parent ID");
    }

    const auto& cycle_a = FindSpan(trace, "cycle-a");
    const auto& cycle_b = FindSpan(trace, "cycle-b");
    const auto& self_cycle = FindSpan(trace, "self-cycle");
    Require(cycle_a.span.parent_span_id == MakeSpanId(2)
                && cycle_b.span.parent_span_id == MakeSpanId(1)
                && self_cycle.span.parent_span_id == MakeSpanId(4),
            "cycle handling changed an original parent span ID");
    Require(ChildNames(trace, cycle_a)
                == std::vector<std::string>{"cycle-child"},
            "non-cycle child of a cycle member was detached");
    Require(ChildNames(trace, self_cycle)
                == std::vector<std::string>{"self-child"},
            "child of a self-cycle member was detached");
    Require(FindSpan(trace, "cycle-child").parent_resolution
                == ParentResolution::resolved,
            "non-cycle child was incorrectly marked as cyclic");
    Require(FindSpan(trace, "self-child").parent_resolution
                == ParentResolution::resolved,
            "self-cycle child was incorrectly marked as cyclic");

    const auto formatted = FormatReconstructedTraces(traces);
    Require(CountSubstring(formatted, "[cycle-parent=") == 3,
            "cycle diagnostics had the wrong count");
    Require(CountSubstring(formatted, "cycle-child [span=") == 1
                && CountSubstring(formatted, "self-child [span=") == 1,
            "cycle-safe formatting omitted or repeated a child");
}

void UnresolvedEdgesDoNotParticipateInCycleDetection()
{
    auto traces = ReconstructTraces({
        MakeSpan(1, 2, 2, 10, "ambiguous-a"),
        MakeSpan(1, 2, 2, 20, "ambiguous-b"),
        MakeSpan(1, 3, 9, 30, "missing"),
    });
    const auto& trace = traces.front();
    RequireValidStructure(trace);

    Require(FindSpan(trace, "ambiguous-a").parent_resolution
                == ParentResolution::ambiguous
                && FindSpan(trace, "ambiguous-b").parent_resolution
                    == ParentResolution::ambiguous,
            "ambiguous parent claims were treated as cycles");
    Require(FindSpan(trace, "missing").parent_resolution
                == ParentResolution::missing,
            "missing parent claim was treated as a cycle");
}

void OptionalTieBreakersHaveExplicitOrdering()
{
    auto missing_service = MakeSpan(
        1, 1, std::nullopt, 10, "service-order", std::nullopt);
    auto present_service = MakeSpan(
        1, 1, std::nullopt, 10, "service-order", std::string{"service"});
    auto absent_parent = MakeSpan(1, 2, std::nullopt, 20, "parent-order");
    auto present_parent = MakeSpan(1, 2, 9, 20, "parent-order");

    auto traces = ReconstructTraces({
        std::move(present_parent),
        std::move(present_service),
        std::move(absent_parent),
        std::move(missing_service),
    });
    const auto& trace = traces.front();
    Require(trace.spans.size() == 4, "ordering test lost spans");
    Require(trace.top_level_indices.size() == 4,
            "ordering test did not retain four top-level spans");

    const auto& missing_service_node =
        trace.spans[trace.top_level_indices[0]];
    const auto& present_service_node =
        trace.spans[trace.top_level_indices[1]];
    const auto& absent_parent_node =
        trace.spans[trace.top_level_indices[2]];
    const auto& present_parent_node =
        trace.spans[trace.top_level_indices[3]];
    Require(!missing_service_node.span.service_name
                && present_service_node.span.service_name,
            "missing service did not sort before present service");
    Require(!absent_parent_node.span.parent_span_id
                && present_parent_node.span.parent_span_id,
            "absent parent did not sort before present parent");
    RequireValidStructure(trace);
}

void OutputIsStableAcrossInputPermutations()
{
    const std::vector<Span> spans{
        MakeSpan(2, 1, std::nullopt, 10, "second-trace-root"),
        MakeSpan(1, 3, 1, 30, "later-child"),
        MakeSpan(1, 1, std::nullopt, 10, "root"),
        MakeSpan(1, 4, 9, 40, "missing"),
        MakeSpan(1, 2, 1, 20, "earlier-child", std::nullopt),
    };

    auto reversed = spans;
    std::reverse(reversed.begin(), reversed.end());
    auto rotated = spans;
    std::rotate(rotated.begin(), rotated.begin() + 2, rotated.end());

    const auto first = ReconstructTraces(spans);
    const auto second = ReconstructTraces(std::move(reversed));
    const auto third = ReconstructTraces(std::move(rotated));
    Require(FormatReconstructedTraces(first) == FormatReconstructedTraces(second)
                && FormatReconstructedTraces(first)
                    == FormatReconstructedTraces(third),
            "formatted output depended on arrival order");
    for (const auto& trace : first) {
        RequireValidStructure(trace);
    }
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"id_ordering_and_hex_formatting_are_canonical",
             IdOrderingAndHexFormattingAreCanonical},
    TestCase{"empty_and_single_root_formatting_is_readable",
             EmptyAndSingleRootFormattingIsReadable},
    TestCase{"out_of_order_branching_reconstructs_by_identity",
             OutOfOrderBranchingReconstructsByIdentity},
    TestCase{"interleaved_traces_remain_independent_and_ordered",
             InterleavedTracesRemainIndependentAndOrdered},
    TestCase{"missing_parents_and_multiple_roots_remain_top_level",
             MissingParentsAndMultipleRootsRemainTopLevel},
    TestCase{"duplicate_ids_retain_spans_and_make_children_ambiguous",
             DuplicateIdsRetainSpansAndMakeChildrenAmbiguous},
    TestCase{"cycles_detach_only_cycle_edges_and_preserve_children",
             CyclesDetachOnlyCycleEdgesAndPreserveChildren},
    TestCase{"unresolved_edges_do_not_participate_in_cycle_detection",
             UnresolvedEdgesDoNotParticipateInCycleDetection},
    TestCase{"optional_tie_breakers_have_explicit_ordering",
             OptionalTieBreakersHaveExplicitOrdering},
    TestCase{"output_is_stable_across_input_permutations",
             OutputIsStableAcrossInputPermutations},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one reconstruction test-case name.\n";
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

    std::cerr << "Unknown reconstruction test case: " << requested_case << '\n';
    return 1;
}
