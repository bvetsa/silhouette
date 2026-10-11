#include "silhouette/active_trace_manager.h"
#include "active_trace_manager_test_peer.h"
#include "trace_lifecycle_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
using silhouette::testing::FakeTraceClock;
using silhouette::testing::LifecycleSweepSync;
using silhouette::testing::AdvanceAndWakeLifecycle;
using namespace std::chrono_literals;

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

void AppendBatchReturnsSortedUniqueAffectedTraceIds()
{
    ActiveTraceManager manager;
    const auto affected = manager.AppendBatch({
        MakeSpan(3, 1, "three"),
        MakeSpan(1, 1, "one"),
        MakeSpan(3, 2, "three-again"),
        MakeSpan(2, 1, "two"),
    });

    Require(affected == std::vector<TraceId>{
                            MakeTraceId(1),
                            MakeTraceId(2),
                            MakeTraceId(3),
                        },
            "append did not return sorted unique affected trace IDs");
    Require(manager.AppendBatch({}).empty(),
            "empty append reported affected trace IDs");
}

void SnapshotTracesSelectsLatestOwnedState()
{
    ActiveTraceManager manager;
    manager.AppendBatch({
        MakeSpan(1, 1, "one-root"),
        MakeSpan(2, 1, "two-root"),
    });

    const std::array requested{
        MakeTraceId(2), MakeTraceId(2), MakeTraceId(9)};
    const auto selected = manager.SnapshotTraces(requested);
    const auto selected_text = FormatReconstructedTraces(selected);

    Require(selected.size() == 1 && selected.front().trace_id == MakeTraceId(2),
            "targeted snapshot duplicated or invented requested traces");
    Require(selected.front().spans.size() == 1,
            "targeted snapshot omitted retained evidence");

    manager.AppendBatch({MakeSpan(2, 2, "two-child", 1)});
    const auto newest = manager.SnapshotTraces(requested);

    Require(FormatReconstructedTraces(selected) == selected_text,
            "targeted snapshot changed after later ingestion");
    Require(newest.size() == 1 && newest.front().spans.size() == 2,
            "later targeted snapshot did not use newest authoritative state");
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

void RequireCounts(const ActiveTraceManager& manager,
                   const std::size_t active, const std::size_t quiescent)
{
    const auto counts = manager.lifecycle_counts();
    Require(counts.active == active && counts.quiescent == quiescent
                && counts.retained == active + quiescent,
            "coherent lifecycle counts violated the retained-state invariant");
    Require(manager.active_trace_count() == active
                && manager.quiescent_trace_count() == quiescent
                && manager.retained_trace_count() == active + quiescent,
            "individual count queries did not match lifecycle state");
}

void RejectsInvalidLifecycleConfiguration()
{
    for (const auto interval : {0s, -1s}) {
        bool rejected = false;
        try {
            ActiveTraceManager manager{interval};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "non-positive quiet interval was accepted");
    }
    bool rejected = false;
    try {
        ActiveTraceManager manager{10s, nullptr, nullptr};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, "null observation clock was accepted");
}

void ExactDeadlinePreservesAllSnapshots()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    const auto original = manager.Snapshot();
    const auto original_text = FormatReconstructedTraces(original);
    RequireCounts(manager, 1, 0);
    clock.Advance(10s - 1ns);
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 1ns,
            "trace became quiet before its exact deadline");
    RequireCounts(manager, 1, 0);
    clock.Advance(1ns);
    Require(!ActiveTraceManagerTestPeer::SweepLifecycle(manager),
            "quiescent trace retained an active deadline");
    RequireCounts(manager, 0, 1);
    const std::array ids{MakeTraceId(1), MakeTraceId(1), MakeTraceId(9)};
    Require(FormatReconstructedTraces(manager.Snapshot()) == original_text
                && FormatReconstructedTraces(manager.SnapshotTraces(ids)) == original_text,
            "quiescence changed or hid retained evidence");
    clock.Advance(1h);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 1);
    Require(FormatReconstructedTraces(original) == original_text,
            "quiescence mutated an earlier owned snapshot");
}

void IndependentDeadlinesSampleClockOnce()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "one"), MakeSpan(3, 1, "three")});
    Require(clock.samples() == 1, "multi-trace publication sampled time repeatedly");
    clock.Advance(5s);
    manager.AppendBatch({MakeSpan(2, 1, "two")});
    clock.Advance(5s);
    const auto samples = clock.samples();
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 5s,
            "nearest remaining independent deadline was incorrect");
    Require(clock.samples() == samples + 1, "sweep sampled time more than once");
    RequireCounts(manager, 1, 2);
    clock.Advance(5s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 3);
}

void DuplicateRefreshesAndEmptyBatchDoesNothing()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    clock.Advance(9s);
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    clock.Advance(1s);
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 9s,
            "duplicate arrival did not refresh the quiet deadline");
    clock.Advance(5s);
    const auto samples = clock.samples();
    Require(manager.AppendBatch({}).empty() && clock.samples() == samples,
            "empty publication sampled time or reported affected traces");
    clock.Advance(4s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 1);
    manager.AppendBatch({});
    RequireCounts(manager, 0, 1);
    Require(RetainedSpanCount(manager.Snapshot()) == 2,
            "duplicate evidence was discarded");
}

void LateEvidenceReopensAndResetsDeadline()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    const auto original = manager.Snapshot();
    clock.Advance(10s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    clock.Advance(2s);
    manager.AppendBatch({MakeSpan(1, 2, "child", 1)});
    RequireCounts(manager, 1, 0);
    Require(RetainedSpanCount(manager.Snapshot()) == 2
                && RetainedSpanCount(original) == 1,
            "reopening lost evidence or mutated an earlier snapshot");
    clock.Advance(9s);
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 1s,
            "reopened trace used its old deadline");
    clock.Advance(1s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 1);
}

void OneBatchReopensAllAffectedTraces()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "one"), MakeSpan(2, 1, "two")});
    clock.Advance(10s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 2);
    clock.Advance(5s);
    const auto samples = clock.samples();
    manager.AppendBatch({MakeSpan(1, 2, "one-child", 1),
                         MakeSpan(2, 2, "two-child", 1),
                         MakeSpan(3, 1, "new")});
    Require(clock.samples() == samples + 1,
            "reopening a batch used inconsistent observation times");
    RequireCounts(manager, 3, 0);
    clock.Advance(10s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    RequireCounts(manager, 0, 3);
    Require(RetainedSpanCount(manager.Snapshot()) == 5,
            "multi-trace reopening lost accepted spans");
}

void ConcurrentLifecyclePublicationIsAtomic()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{10s, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "one"), MakeSpan(2, 1, "two")});
    clock.Advance(10s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(manager));
    AtomicPublicationSync sync;
    ActiveTraceManagerTestPeer::SetBeforeCommitHook(
        manager, PauseAppendBeforeCommit, &sync);
    ActiveTraceManager::LifecycleCounts observed;
    std::thread writer{[&] {
        manager.AppendBatch({MakeSpan(1, 2, "late-one", 1),
                             MakeSpan(2, 2, "late-two", 1),
                             MakeSpan(3, 1, "new")});
    }};
    sync.append_holds_lock.wait();
    std::thread reader{[&] {
        observed = ActiveTraceManagerTestPeer::CountsWithBeforeLock(
            manager, SignalSnapshotAttempt, &sync);
    }};
    writer.join();
    reader.join();
    Require(observed == ActiveTraceManager::LifecycleCounts{3, 0, 3}
                && RetainedSpanCount(manager.Snapshot()) == 5,
            "count snapshot observed only part of an atomic lifecycle publication");
}

void WorkerWakesForPublicationAndFakeTime()
{
    FakeTraceClock clock;
    LifecycleSweepSync sweeps;
    ActiveTraceManager manager{1h, FakeTraceClock::Now, &clock};
    ActiveTraceManagerTestPeer::SetAfterSweepHook(
        manager, LifecycleSweepSync::Signal, &sweeps);
    manager.StartLifecycle();
    sweeps.WaitFor(1);
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    sweeps.WaitFor(2);
    RequireCounts(manager, 1, 0);
    sweeps.WaitFor(AdvanceAndWakeLifecycle(manager, clock, sweeps, 1h));
    RequireCounts(manager, 0, 1);
    manager.AppendBatch({MakeSpan(1, 2, "late", 1)});
    RequireCounts(manager, 1, 0);
    sweeps.WaitFor(AdvanceAndWakeLifecycle(manager, clock, sweeps, 1h));
    RequireCounts(manager, 0, 1);
    manager.StopLifecycle();
}

struct PauseNextSweep final {
    std::atomic<bool> armed{false};
    std::latch paused{1};
    std::latch release{1};

    static void Pause(void* context) noexcept
    {
        auto& sync = *static_cast<PauseNextSweep*>(context);
        if (sync.armed.exchange(false)) {
            sync.paused.count_down();
            sync.release.wait();
        }
    }
};

void AppendWinsAgainstStaleDeadlineWake()
{
    FakeTraceClock clock;
    LifecycleSweepSync sweeps;
    PauseNextSweep pause;
    ActiveTraceManager manager{1h, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    ActiveTraceManagerTestPeer::SetBeforeSweepLockHook(
        manager, PauseNextSweep::Pause, &pause);
    ActiveTraceManagerTestPeer::SetAfterSweepHook(
        manager, LifecycleSweepSync::Signal, &sweeps);
    manager.StartLifecycle();
    sweeps.WaitFor(1);
    pause.armed = true;
    const auto acknowledgment = AdvanceAndWakeLifecycle(manager, clock, sweeps, 1h);
    pause.paused.wait();
    // The old deadline has elapsed, but append commits before the worker locks.
    manager.AppendBatch({MakeSpan(1, 2, "new", 1)});
    pause.release.count_down();
    sweeps.WaitFor(acknowledgment);
    manager.StopLifecycle();
    RequireCounts(manager, 1, 0);
    clock.Advance(1h - 1ns);
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 1ns,
            "stale wake finalized refreshed evidence or retained the old deadline");
    Require(RetainedSpanCount(manager.Snapshot()) == 2,
            "append-wins race lost evidence");
}

void QuiescenceWinsThenAppendReopens()
{
    FakeTraceClock clock;
    LifecycleSweepSync sweeps;
    ActiveTraceManager manager{1h, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    ActiveTraceManagerTestPeer::SetAfterSweepHook(
        manager, LifecycleSweepSync::Signal, &sweeps);
    manager.StartLifecycle();
    sweeps.WaitFor(1);
    sweeps.WaitFor(AdvanceAndWakeLifecycle(manager, clock, sweeps, 1h));
    RequireCounts(manager, 0, 1);
    std::thread writer{[&] { manager.AppendBatch({MakeSpan(1, 2, "late", 1)}); }};
    writer.join();
    manager.StopLifecycle();
    RequireCounts(manager, 1, 0);
    Require(ActiveTraceManagerTestPeer::SweepLifecycle(manager) == 1h,
            "reopen after quiescence did not establish a fresh deadline");
}

void LifecycleStartIsOneShotAndStopIsIdempotent()
{
    ActiveTraceManager manager;
    manager.StopLifecycle();
    manager.StopLifecycle();
    manager.StartLifecycle();
    bool rejected = false;
    try {
        manager.StartLifecycle();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Require(rejected, "second lifecycle start was accepted");
    manager.StopLifecycle();
    manager.StopLifecycle();
    rejected = false;
    try {
        manager.StartLifecycle();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Require(rejected, "lifecycle restart after stop was accepted");
}

void StopWhileWaitingPreservesActiveEvidence()
{
    FakeTraceClock clock;
    LifecycleSweepSync sweeps;
    ActiveTraceManager manager{1h, FakeTraceClock::Now, &clock};
    manager.AppendBatch({MakeSpan(1, 1, "root")});
    ActiveTraceManagerTestPeer::SetAfterSweepHook(
        manager, LifecycleSweepSync::Signal, &sweeps);
    manager.StartLifecycle();
    sweeps.WaitFor(1);
    manager.StopLifecycle();
    RequireCounts(manager, 1, 0);
    Require(RetainedSpanCount(manager.Snapshot()) == 1,
            "stopping the worker discarded retained evidence");
}

void DestructorJoinsIdleAndDeadlineWorkers()
{
    FakeTraceClock clock;
    for (const bool with_evidence : {false, true}) {
        LifecycleSweepSync sweeps;
        ActiveTraceManager manager{1h, FakeTraceClock::Now, &clock};
        if (with_evidence) {
            manager.AppendBatch({MakeSpan(1, 1, "root")});
        }
        ActiveTraceManagerTestPeer::SetAfterSweepHook(
            manager, LifecycleSweepSync::Signal, &sweeps);
        manager.StartLifecycle();
        sweeps.WaitFor(1);
        // Scope exit must wake and join both kinds of physical wait.
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"rejects_invalid_lifecycle_configuration",
             RejectsInvalidLifecycleConfiguration},
    TestCase{"exact_deadline_preserves_all_snapshots",
             ExactDeadlinePreservesAllSnapshots},
    TestCase{"independent_deadlines_sample_clock_once",
             IndependentDeadlinesSampleClockOnce},
    TestCase{"duplicate_refreshes_and_empty_batch_does_nothing",
             DuplicateRefreshesAndEmptyBatchDoesNothing},
    TestCase{"late_evidence_reopens_and_resets_deadline",
             LateEvidenceReopensAndResetsDeadline},
    TestCase{"one_batch_reopens_all_affected_traces",
             OneBatchReopensAllAffectedTraces},
    TestCase{"concurrent_lifecycle_publication_is_atomic",
             ConcurrentLifecyclePublicationIsAtomic},
    TestCase{"worker_wakes_for_publication_and_fake_time",
             WorkerWakesForPublicationAndFakeTime},
    TestCase{"append_wins_against_stale_deadline_wake",
             AppendWinsAgainstStaleDeadlineWake},
    TestCase{"quiescence_wins_then_append_reopens",
             QuiescenceWinsThenAppendReopens},
    TestCase{"lifecycle_start_is_one_shot_and_stop_is_idempotent",
             LifecycleStartIsOneShotAndStopIsIdempotent},
    TestCase{"stop_while_waiting_preserves_active_evidence",
             StopWhileWaitingPreservesActiveEvidence},
    TestCase{"destructor_joins_idle_and_deadline_workers",
             DestructorJoinsIdleAndDeadlineWorkers},
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
    TestCase{"append_batch_returns_sorted_unique_affected_trace_ids",
             AppendBatchReturnsSortedUniqueAffectedTraceIds},
    TestCase{"snapshot_traces_selects_latest_owned_state",
             SnapshotTracesSelectsLatestOwnedState},
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
