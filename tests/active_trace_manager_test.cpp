#include "silhouette/active_trace_manager.h"
#include "active_trace_manager_test_peer.h"

#include <algorithm>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using silhouette::ActiveTraceManager;
using silhouette::FormatReconstructedTraces;
using silhouette::ParentResolution;
using silhouette::ReconstructedTrace;
using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;
using silhouette::testing::ActiveTraceManagerTestPeer;

static_assert(!std::is_copy_constructible_v<ActiveTraceManager>);
static_assert(!std::is_move_constructible_v<ActiveTraceManager>);

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
    std::string operation_name,
    const std::optional<std::uint8_t> parent_marker = std::nullopt)
{
    return Span{
        .trace_id = MakeTraceId(trace_marker),
        .span_id = MakeSpanId(span_marker),
        .parent_span_id = parent_marker
            ? std::optional<SpanId>{MakeSpanId(*parent_marker)}
            : std::nullopt,
        .service_name = std::string{"service"},
        .operation_name = std::move(operation_name),
        .start_time_unix_nano = span_marker,
        .end_time_unix_nano = static_cast<std::uint64_t>(span_marker) + 1,
    };
}

std::size_t RetainedSpanCount(
    const std::vector<ReconstructedTrace>& traces)
{
    std::size_t count = 0;
    for (const auto& trace : traces) {
        count += trace.spans.size();
    }
    return count;
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
    Require(found != traces.end(), "expected active trace was missing");
    return *found;
}

void OneTraceReceivesOneBatch()
{
    ActiveTraceManager manager;
    manager.AppendBatch({MakeSpan(1, 1, "root")});

    const auto snapshot = manager.Snapshot();
    Require(manager.active_trace_count() == 1,
            "single trace was not counted as active");
    Require(snapshot.size() == 1 && snapshot.front().spans.size() == 1,
            "single accepted span was not reconstructed");
}

void OneTraceReceivesMultipleBatches()
{
    ActiveTraceManager manager;
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    manager.AppendBatch({MakeSpan(1, 2, "child", 1)});

    const auto snapshot = manager.Snapshot();
    Require(manager.active_trace_count() == 1,
            "multiple batches created duplicate active traces");
    Require(snapshot.size() == 1 && snapshot.front().spans.size() == 2,
            "later batch was not retained with its trace");
}

void MultipleInterleavedTracesRemainIndependent()
{
    ActiveTraceManager manager;
    manager.AppendBatch({
        MakeSpan(2, 1, "trace-two-root"),
        MakeSpan(1, 2, "trace-one-child", 1),
    });
    manager.AppendBatch({
        MakeSpan(1, 1, "trace-one-root"),
        MakeSpan(2, 2, "trace-two-child", 1),
    });

    const auto snapshot = manager.Snapshot();
    Require(manager.active_trace_count() == 2,
            "interleaved traces produced the wrong active count");
    Require(snapshot.size() == 2,
            "interleaved traces were not reconstructed independently");
    Require(FindTrace(snapshot, 1).spans.size() == 2,
            "first interleaved trace lost a span");
    Require(FindTrace(snapshot, 2).spans.size() == 2,
            "second interleaved trace lost a span");
}

void ChildBeforeParentReconstructsAfterLaterBatch()
{
    ActiveTraceManager manager;
    manager.AppendBatch({MakeSpan(1, 2, "child", 1)});

    const auto incomplete = manager.Snapshot();
    Require(incomplete.front().spans.front().parent_resolution
                == ParentResolution::missing,
            "child without its parent did not preserve missing evidence");

    manager.AppendBatch({MakeSpan(1, 1, "root")});
    const auto complete = manager.Snapshot();
    const auto child = std::find_if(
        complete.front().spans.begin(),
        complete.front().spans.end(),
        [](const auto& span) { return span.span.operation_name == "child"; });
    Require(child != complete.front().spans.end(),
            "child disappeared after its parent arrived");
    Require(child->parent_resolution == ParentResolution::resolved,
            "late parent did not resolve the retained child");
}

void DuplicateSpanIdsArePreserved()
{
    ActiveTraceManager manager;
    manager.AppendBatch({
        MakeSpan(1, 1, "first-copy"),
        MakeSpan(1, 1, "second-copy"),
    });

    const auto snapshot = manager.Snapshot();
    Require(snapshot.front().spans.size() == 2,
            "duplicate span IDs were deduplicated");
    Require(snapshot.front().spans[0].duplicate_span_id
                && snapshot.front().spans[1].duplicate_span_id,
            "duplicate span IDs were not marked by reconstruction");
}

void MissingParentEvidenceIsPreserved()
{
    ActiveTraceManager manager;
    manager.AppendBatch({MakeSpan(1, 2, "orphan", 9)});

    const auto snapshot = manager.Snapshot();
    Require(snapshot.front().spans.front().parent_resolution
                == ParentResolution::missing,
            "missing parent evidence was changed or discarded");
    Require(snapshot.front().spans.front().span.parent_span_id == MakeSpanId(9),
            "original missing parent ID was changed");
}

void ActiveTraceCountTracksDistinctIds()
{
    ActiveTraceManager manager;
    Require(manager.active_trace_count() == 0,
            "empty manager reported an active trace");

    manager.AppendBatch({});
    manager.AppendBatch({
        MakeSpan(1, 1, "one"),
        MakeSpan(2, 1, "two"),
        MakeSpan(1, 2, "one-again"),
    });
    manager.AppendBatch({MakeSpan(2, 2, "two-again")});

    Require(manager.active_trace_count() == 2,
            "active count did not track distinct trace IDs");
}

void EquivalentArrivalOrdersProduceStableSnapshots()
{
    ActiveTraceManager forward;
    forward.AppendBatch({
        MakeSpan(1, 1, "root"),
        MakeSpan(1, 2, "first-child", 1),
    });
    forward.AppendBatch({MakeSpan(1, 3, "second-child", 1)});

    ActiveTraceManager reverse;
    reverse.AppendBatch({MakeSpan(1, 3, "second-child", 1)});
    reverse.AppendBatch({
        MakeSpan(1, 2, "first-child", 1),
        MakeSpan(1, 1, "root"),
    });

    Require(
        FormatReconstructedTraces(forward.Snapshot())
            == FormatReconstructedTraces(reverse.Snapshot()),
        "equivalent arrival orders produced different snapshots");
}

void ReturnedSnapshotsRemainOwnedAndUnchanged()
{
    ActiveTraceManager manager;
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    const auto first_snapshot = manager.Snapshot();
    const auto first_text = FormatReconstructedTraces(first_snapshot);

    manager.AppendBatch({MakeSpan(1, 2, "child", 1)});
    const auto second_snapshot = manager.Snapshot();

    Require(RetainedSpanCount(first_snapshot) == 1,
            "earlier snapshot changed after later ingestion");
    Require(FormatReconstructedTraces(first_snapshot) == first_text,
            "earlier snapshot contents were not stable");
    Require(RetainedSpanCount(second_snapshot) == 2,
            "later snapshot omitted newly ingested evidence");
}

void ConcurrentAppendBatchesLoseNoSpans()
{
    constexpr std::size_t kBatchSize = 3;
    constexpr std::size_t kWriterCount = 8;
    constexpr std::size_t kExpectedSpanCount = kBatchSize * kWriterCount;

    ActiveTraceManager manager;
    std::latch writers_ready{kWriterCount};
    std::latch start_writers{1};
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);

    for (std::size_t index = 0; index < kWriterCount; ++index) {
        writers.emplace_back([&, index] {
            writers_ready.count_down();
            start_writers.wait();

            const auto span_marker = static_cast<std::uint8_t>(index * 3 + 1);
            manager.AppendBatch({
                MakeSpan(1, span_marker, "one"),
                MakeSpan(1, span_marker + 1, "two"),
                MakeSpan(1, span_marker + 2, "three"),
            });
        });
    }

    writers_ready.wait();
    start_writers.count_down();
    for (auto& writer : writers) {
        writer.join();
    }

    Require(manager.active_trace_count() == 1,
            "concurrent batches split one trace into multiple active traces");
    Require(RetainedSpanCount(manager.Snapshot()) == kExpectedSpanCount,
            "concurrent publication lost accepted spans");
}

struct AtomicPublicationSync final {
    std::latch append_holds_lock{1};
    std::latch snapshot_attempted{1};
};

void PauseAppendBeforeCommit(void* context) noexcept
{
    auto& sync = *static_cast<AtomicPublicationSync*>(context);
    sync.append_holds_lock.count_down();
    sync.snapshot_attempted.wait();
}

void SignalSnapshotAttempt(void* context) noexcept
{
    auto& sync = *static_cast<AtomicPublicationSync*>(context);
    sync.snapshot_attempted.count_down();
}

void ConcurrentBatchesPublishAtomically()
{
    ActiveTraceManager manager;
    AtomicPublicationSync sync;
    ActiveTraceManagerTestPeer::SetBeforeCommitHook(
        manager, PauseAppendBeforeCommit, &sync);

    std::vector<ReconstructedTrace> observed_snapshot;
    std::thread writer{[&] {
        manager.AppendBatch({
            MakeSpan(1, 1, "one-batch"),
            MakeSpan(2, 1, "one-batch"),
            MakeSpan(3, 1, "one-batch"),
        });
    }};

    sync.append_holds_lock.wait();
    std::thread reader{[&] {
        observed_snapshot =
            ActiveTraceManagerTestPeer::SnapshotWithBeforeLock(
                manager, SignalSnapshotAttempt, &sync);
    }};

    writer.join();
    reader.join();

    Require(observed_snapshot.size() == 3,
            "snapshot did not observe the complete multi-trace batch");
    Require(RetainedSpanCount(observed_snapshot) == 3,
            "snapshot observed only part of the accepted batch");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"one_trace_receives_one_batch", OneTraceReceivesOneBatch},
    TestCase{"one_trace_receives_multiple_batches",
             OneTraceReceivesMultipleBatches},
    TestCase{"multiple_interleaved_traces_remain_independent",
             MultipleInterleavedTracesRemainIndependent},
    TestCase{"child_before_parent_reconstructs_after_later_batch",
             ChildBeforeParentReconstructsAfterLaterBatch},
    TestCase{"duplicate_span_ids_are_preserved", DuplicateSpanIdsArePreserved},
    TestCase{"missing_parent_evidence_is_preserved",
             MissingParentEvidenceIsPreserved},
    TestCase{"active_trace_count_tracks_distinct_ids",
             ActiveTraceCountTracksDistinctIds},
    TestCase{"equivalent_arrival_orders_produce_stable_snapshots",
             EquivalentArrivalOrdersProduceStableSnapshots},
    TestCase{"returned_snapshots_remain_owned_and_unchanged",
             ReturnedSnapshotsRemainOwnedAndUnchanged},
    TestCase{"concurrent_append_batches_lose_no_spans",
             ConcurrentAppendBatchesLoseNoSpans},
    TestCase{"concurrent_batches_publish_atomically",
             ConcurrentBatchesPublishAtomically},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one active-trace test-case name.\n";
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

    std::cerr << "Unknown active-trace test case: " << requested_case << '\n';
    return 1;
}
