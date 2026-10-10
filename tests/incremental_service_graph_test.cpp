#include "silhouette/incremental_service_graph.h"
#include "incremental_service_graph_test_peer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using silhouette::BuildServiceGraph;
using silhouette::FormatServiceGraph;
using silhouette::IncrementalServiceGraph;
using silhouette::ReconstructedTrace;
using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;
using silhouette::testing::IncrementalServiceGraphTestPeer;

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
    return *TraceId::FromBytes(bytes);
}

SpanId MakeSpanId(const std::uint8_t marker)
{
    SpanId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    return *SpanId::FromBytes(bytes);
}

Span MakeSpan(
    const std::uint8_t trace_marker,
    const std::uint8_t span_marker,
    std::optional<std::string> service,
    const std::optional<std::uint8_t> parent_marker = std::nullopt)
{
    return Span{
        .trace_id = MakeTraceId(trace_marker),
        .span_id = MakeSpanId(span_marker),
        .parent_span_id = parent_marker
            ? std::optional<SpanId>{MakeSpanId(*parent_marker)}
            : std::nullopt,
        .service_name = std::move(service),
        .operation_name = "operation",
        .start_time_unix_nano = span_marker,
        .end_time_unix_nano = static_cast<std::uint64_t>(span_marker) + 1,
    };
}

ReconstructedTrace MakeEdgeTrace(
    const std::uint8_t trace_marker,
    std::string source,
    std::string destination)
{
    auto traces = silhouette::ReconstructTraces({
        MakeSpan(trace_marker, 1, std::move(source)),
        MakeSpan(trace_marker, 2, std::move(destination), 1),
    });
    return std::move(traces.front());
}

void FirstContributionMatchesFullBuilder()
{
    std::vector traces{
        MakeEdgeTrace(1, "frontend", "checkout"),
        MakeEdgeTrace(2, "checkout", "database"),
    };
    auto unresolved = silhouette::ReconstructTraces({
        MakeSpan(3, 2, std::string{"worker"}, 9),
    });
    traces.push_back(std::move(unresolved.front()));
    auto missing_service = silhouette::ReconstructTraces({
        MakeSpan(4, 1, std::nullopt),
        MakeSpan(4, 2, std::string{"cache"}, 1),
    });
    traces.push_back(std::move(missing_service.front()));
    IncrementalServiceGraph incremental;
    incremental.UpdateTraces(traces);

    Require(FormatServiceGraph(incremental.Snapshot())
                == FormatServiceGraph(BuildServiceGraph(traces)),
            "initial incremental topology differed from the full builder");
}

void LateEvidenceReplacesStaleContribution()
{
    IncrementalServiceGraph incremental;
    auto incomplete = silhouette::ReconstructTraces({
        MakeSpan(1, 2, std::string{"checkout"}, 1),
    });
    incremental.UpdateTraces(incomplete);
    const auto first = incremental.Snapshot();
    Require(first.edges.empty() && first.gaps.size() == 1,
            "incomplete trace did not contribute its parent-resolution gap");

    auto resolved = silhouette::ReconstructTraces({
        MakeSpan(1, 1, std::string{"frontend"}),
        MakeSpan(1, 2, std::string{"checkout"}, 1),
    });
    incremental.UpdateTraces(resolved);
    const auto second = incremental.Snapshot();

    Require(second.gaps.empty(),
            "resolved late evidence left its stale gap in the aggregate");
    Require(second.edges
                == std::vector<silhouette::ServiceEdge>{
                    {"frontend", "checkout"}},
            "resolved late evidence did not replace the trace edge");
}

void SharedReferencesDisappearOnlyAfterLastContributor()
{
    IncrementalServiceGraph incremental;
    incremental.UpdateTraces(std::vector{
        MakeEdgeTrace(1, "A", "B"),
        MakeEdgeTrace(2, "A", "B"),
    });

    const std::array first_replacement{MakeEdgeTrace(1, "A", "C")};
    incremental.UpdateTraces(first_replacement);
    auto graph = incremental.Snapshot();
    Require(graph.edges == std::vector<silhouette::ServiceEdge>{
                               {"A", "B"}, {"A", "C"}},
            "replacing one contributor removed a still-shared edge");

    const std::array second_replacement{MakeEdgeTrace(2, "A", "C")};
    incremental.UpdateTraces(second_replacement);
    graph = incremental.Snapshot();
    Require(graph.edges
                == std::vector<silhouette::ServiceEdge>{{"A", "C"}},
            "edge remained after its final trace contribution was replaced");
}

void ReplaceAllRestoresAuthoritativeState()
{
    IncrementalServiceGraph incremental;
    incremental.UpdateTraces(std::vector{
        MakeEdgeTrace(1, "stale", "edge"),
        MakeEdgeTrace(2, "shared", "old"),
    });

    const std::vector authoritative{
        MakeEdgeTrace(2, "shared", "new"),
        MakeEdgeTrace(3, "new", "trace"),
    };
    incremental.ReplaceAll(authoritative);

    Require(FormatServiceGraph(incremental.Snapshot())
                == FormatServiceGraph(BuildServiceGraph(authoritative)),
            "full replacement retained stale contribution state");

    incremental.ReplaceAll({});
    Require(FormatServiceGraph(incremental.Snapshot())
                == FormatServiceGraph({}),
            "empty full replacement did not clear aggregate state");
}

struct AtomicUpdateSync final {
    std::latch update_holds_lock{1};
    std::latch snapshot_attempted{1};
};

void PauseBeforeCommit(void* context) noexcept
{
    auto& sync = *static_cast<AtomicUpdateSync*>(context);
    sync.update_holds_lock.count_down();
    sync.snapshot_attempted.wait();
}

void ConcurrentSnapshotSeesCompleteContributionReplacement()
{
    IncrementalServiceGraph incremental;
    const std::array initial{MakeEdgeTrace(1, "A", "B")};
    incremental.UpdateTraces(initial);

    AtomicUpdateSync sync;
    IncrementalServiceGraphTestPeer::SetBeforeCommitHook(
        incremental, PauseBeforeCommit, &sync);

    const std::array replacement{MakeEdgeTrace(1, "A", "C")};
    std::thread writer{[&] { incremental.UpdateTraces(replacement); }};
    sync.update_holds_lock.wait();

    silhouette::ServiceGraph observed;
    std::thread reader{[&] {
        sync.snapshot_attempted.count_down();
        observed = incremental.Snapshot();
    }};

    writer.join();
    reader.join();
    Require(observed.edges
                == std::vector<silhouette::ServiceEdge>{{"A", "C"}},
            "snapshot observed a partial contribution replacement");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"first_contribution_matches_full_builder",
             FirstContributionMatchesFullBuilder},
    TestCase{"late_evidence_replaces_stale_contribution",
             LateEvidenceReplacesStaleContribution},
    TestCase{"shared_references_disappear_only_after_last_contributor",
             SharedReferencesDisappearOnlyAfterLastContributor},
    TestCase{"replace_all_restores_authoritative_state",
             ReplaceAllRestoresAuthoritativeState},
    TestCase{"concurrent_snapshot_sees_complete_contribution_replacement",
             ConcurrentSnapshotSeesCompleteContributionReplacement},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one incremental-graph test-case name.\n";
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

    std::cerr << "Unknown incremental-graph test case: " << requested_case << '\n';
    return 1;
}
