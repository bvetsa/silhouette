#include "silhouette/trace_reconstruction.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace silhouette {

namespace {

bool CanonicalSpanLess(const Span& left, const Span& right)
{
    return std::tie(
               left.start_time_unix_nano,
               left.span_id,
               left.end_time_unix_nano,
               left.operation_name,
               left.service_name,
               left.parent_span_id)
        < std::tie(
               right.start_time_unix_nano,
               right.span_id,
               right.end_time_unix_nano,
               right.operation_name,
               right.service_name,
               right.parent_span_id);
}

void MarkCycleMembers(std::vector<ReconstructedSpan>& spans)
{
    enum class VisitState {
        unvisited,
        visiting,
        finished,
    };

    std::vector<VisitState> state(spans.size(), VisitState::unvisited);
    std::vector<bool> cycle_member(spans.size(), false);

    for (std::size_t start = 0; start < spans.size(); ++start) {
        if (state[start] != VisitState::unvisited) {
            continue;
        }

        std::vector<std::size_t> path;
        auto current = start;

        while (state[current] == VisitState::unvisited
               && spans[current].parent_resolution
                   == ParentResolution::resolved) {
            state[current] = VisitState::visiting;
            path.push_back(current);
            current = *spans[current].parent_index;
        }

        if (state[current] == VisitState::visiting) {
            const auto cycle_start = std::find(path.begin(), path.end(), current);
            for (auto member = cycle_start; member != path.end(); ++member) {
                cycle_member[*member] = true;
            }
        }

        for (const auto index : path) {
            state[index] = VisitState::finished;
        }

        if (path.empty()) {
            state[start] = VisitState::finished;
        }
    }

    for (std::size_t index = 0; index < spans.size(); ++index) {
        if (!cycle_member[index]) {
            continue;
        }

        spans[index].parent_resolution = ParentResolution::cycle;
        spans[index].parent_index.reset();
    }
}

void FinalizeRelationships(ReconstructedTrace& trace)
{
    const auto index_less = [&trace](const std::size_t left,
                                     const std::size_t right) {
        return CanonicalSpanLess(trace.spans[left].span, trace.spans[right].span);
    };

    for (std::size_t index = 0; index < trace.spans.size(); ++index) {
        auto& span = trace.spans[index];
        if (span.parent_resolution == ParentResolution::resolved) {
            trace.spans[*span.parent_index].child_indices.push_back(index);
        } else {
            trace.top_level_indices.push_back(index);
        }
    }

    std::sort(
        trace.top_level_indices.begin(), trace.top_level_indices.end(), index_less);
    for (auto& span : trace.spans) {
        std::sort(span.child_indices.begin(), span.child_indices.end(), index_less);
    }
}

ReconstructedTrace ReconstructTrace(TraceId trace_id, std::vector<Span> spans)
{
    std::sort(spans.begin(), spans.end(), CanonicalSpanLess);

    ReconstructedTrace trace{
        .trace_id = std::move(trace_id),
        .spans = {},
        .top_level_indices = {},
    };
    trace.spans.reserve(spans.size());
    for (auto& span : spans) {
        trace.spans.push_back(ReconstructedSpan{
            .span = std::move(span),
            .parent_resolution = ParentResolution::root,
            .parent_index = std::nullopt,
            .child_indices = {},
            .duplicate_span_id = false,
        });
    }

    std::map<SpanId, std::vector<std::size_t>> span_indices;
    for (std::size_t index = 0; index < trace.spans.size(); ++index) {
        span_indices[trace.spans[index].span.span_id].push_back(index);
    }

    for (const auto& [span_id, indices] : span_indices) {
        static_cast<void>(span_id);
        if (indices.size() < 2) {
            continue;
        }
        for (const auto index : indices) {
            trace.spans[index].duplicate_span_id = true;
        }
    }

    for (auto& span : trace.spans) {
        if (!span.span.parent_span_id) {
            continue;
        }

        const auto parent = span_indices.find(*span.span.parent_span_id);
        if (parent == span_indices.end()) {
            span.parent_resolution = ParentResolution::missing;
            continue;
        }
        if (parent->second.size() > 1) {
            span.parent_resolution = ParentResolution::ambiguous;
            continue;
        }

        span.parent_resolution = ParentResolution::resolved;
        span.parent_index = parent->second.front();
    }

    MarkCycleMembers(trace.spans);
    FinalizeRelationships(trace);
    return trace;
}

void AppendParentDiagnostic(const ReconstructedSpan& span, std::string& output)
{
    std::string_view label;
    switch (span.parent_resolution) {
    case ParentResolution::missing:
        label = "missing-parent";
        break;
    case ParentResolution::ambiguous:
        label = "ambiguous-parent";
        break;
    case ParentResolution::cycle:
        label = "cycle-parent";
        break;
    case ParentResolution::root:
    case ParentResolution::resolved:
        return;
    }

    output += " [";
    output += label;
    output += '=';
    output += span.span.parent_span_id->ToHex();
    output += ']';
}

void AppendNodeLabel(const ReconstructedSpan& span, std::string& output)
{
    output += span.span.service_name.value_or("<unknown-service>");
    output += ": ";
    output += span.span.operation_name;
    output += " [span=";
    output += span.span.span_id.ToHex();
    output += ']';

    if (span.duplicate_span_id) {
        output += " [duplicate-span-id]";
    }
    AppendParentDiagnostic(span, output);
    output += '\n';
}

void AppendTree(
    const ReconstructedTrace& trace,
    const std::size_t root_index,
    const bool root_is_last,
    std::string& output)
{
    struct Frame final {
        std::size_t index;
        std::string prefix;
        bool is_last;
    };

    std::vector<Frame> pending;
    pending.push_back(Frame{
        .index = root_index,
        .prefix = {},
        .is_last = root_is_last,
    });

    while (!pending.empty()) {
        auto frame = std::move(pending.back());
        pending.pop_back();

        output += frame.prefix;
        output += frame.is_last ? "└── " : "├── ";
        const auto& node = trace.spans[frame.index];
        AppendNodeLabel(node, output);

        auto child_prefix = std::move(frame.prefix);
        child_prefix += frame.is_last ? "    " : "│   ";
        for (auto position = node.child_indices.size(); position > 0; --position) {
            const auto child_position = position - 1;
            pending.push_back(Frame{
                .index = node.child_indices[child_position],
                .prefix = child_prefix,
                .is_last = child_position + 1 == node.child_indices.size(),
            });
        }
    }
}

} // namespace

std::vector<ReconstructedTrace> ReconstructTraces(std::vector<Span> spans)
{
    std::map<TraceId, std::vector<Span>> grouped_spans;
    for (auto& span : spans) {
        grouped_spans[span.trace_id].push_back(std::move(span));
    }

    std::vector<ReconstructedTrace> traces;
    traces.reserve(grouped_spans.size());
    for (auto& [trace_id, trace_spans] : grouped_spans) {
        traces.push_back(
            ReconstructTrace(std::move(trace_id), std::move(trace_spans)));
    }
    return traces;
}

std::string FormatReconstructedTraces(
    const std::span<const ReconstructedTrace> traces)
{
    if (traces.empty()) {
        return "No captured spans to reconstruct.\n";
    }

    std::string output;
    for (std::size_t trace_index = 0; trace_index < traces.size(); ++trace_index) {
        if (trace_index > 0) {
            output += '\n';
        }

        const auto& trace = traces[trace_index];
        output += "Trace ";
        output += trace.trace_id.ToHex();
        output += '\n';

        for (std::size_t root_position = 0;
             root_position < trace.top_level_indices.size();
             ++root_position) {
            AppendTree(
                trace,
                trace.top_level_indices[root_position],
                root_position + 1 == trace.top_level_indices.size(),
                output);
        }
    }
    return output;
}

} // namespace silhouette
