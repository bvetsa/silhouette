#pragma once

#include "silhouette/span.h"
#include "silhouette/trace_reconstruction.h"
#include "silhouette/trace_eviction_sink.h"
#include "silhouette/trace_lifecycle_sink.h"
#include "silhouette/trace_record.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace silhouette {

namespace testing {
class ActiveTraceManagerTestPeer;
}

struct TraceRetentionLimits final {
    std::size_t max_quiescent_traces{1000};
    std::chrono::steady_clock::duration max_quiescent_age{std::chrono::minutes{10}};
    std::size_t max_spans_per_trace{10000};
};

struct AppendResult final {
    std::vector<TraceId> affected_trace_ids;
    std::size_t accepted_span_count{0};
    std::size_t capacity_rejected_span_count{0};
};

struct TraceLookup final {
    TraceId trace_id;
    std::optional<ReconstructedTrace> trace;
};

struct TraceRecordLookup final {
    TraceId trace_id;
    std::optional<TraceRecordMetadata> metadata;
    // Absent for an unknown record or an unchanged supplied revision. Metadata
    // distinguishes these cases. An existing record's evidence is non-empty.
    std::optional<std::vector<Span>> evidence;
};

class ActiveTraceManager final {
public:
    using Clock = std::chrono::steady_clock;
    using ClockNow = Clock::time_point (*)(void*) noexcept;

    struct LifecycleCounts final {
        std::size_t active{0};
        std::size_t quiescent{0};
        std::size_t retained{0};

        bool operator==(const LifecycleCounts&) const = default;
    };

    explicit ActiveTraceManager(
        Clock::duration quiet_interval = std::chrono::seconds{30});
    // Called under the manager mutex: now must be monotonic, non-blocking,
    // noexcept, and must not re-enter this manager. Context must outlive it.
    ActiveTraceManager(
        Clock::duration quiet_interval, ClockNow now, void* context);
    ActiveTraceManager(Clock::duration quiet_interval, TraceRetentionLimits limits);
    ActiveTraceManager(Clock::duration quiet_interval, TraceRetentionLimits limits,
                       ClockNow now, void* context);
    ~ActiveTraceManager();

    ActiveTraceManager(const ActiveTraceManager&) = delete;
    ActiveTraceManager& operator=(const ActiveTraceManager&) = delete;
    ActiveTraceManager(ActiveTraceManager&&) = delete;
    ActiveTraceManager& operator=(ActiveTraceManager&&) = delete;

    // Owner-thread operations, serialized with each other and destruction.
    // Start is one-shot; stopping before start does not consume that start.
    // Sink must remain alive until StopLifecycle() returns.
    void StartLifecycle(TraceEvictionSink* eviction_sink = nullptr,
                        TraceLifecycleSink* lifecycle_sink = nullptr);
    void StopLifecycle() noexcept;

    // Rejects whole over-cap contributions per TraceId; publishes the complete
    // admitted subset atomically. Rejections do not refresh lifecycle.
    AppendResult AppendBatch(std::vector<Span> spans);

    // Returns an owned point-in-time reconstruction. Later ingestion cannot
    // change the returned traces.
    [[nodiscard]] std::vector<ReconstructedTrace> Snapshot() const;

    // Returns owned reconstructions for the requested trace IDs. Duplicate or
    // unknown IDs do not duplicate evidence or create empty traces.
    [[nodiscard]] std::vector<ReconstructedTrace> SnapshotTraces(
        std::span<const TraceId> trace_ids) const;

    // One owned entry per unique requested ID, including absent IDs. Queries
    // never refresh retention; presence/evidence are captured under one lock.
    [[nodiscard]] std::vector<TraceLookup> LookupTraces(
        std::span<const TraceId> trace_ids) const;

    [[nodiscard]] std::vector<TraceRecordSnapshot> SnapshotRecords() const;
    [[nodiscard]] std::vector<TraceRecordLookup> LookupRecords(
        std::span<const TraceId> trace_ids,
        std::span<const TraceRecordRevision> known = {},
        bool copy_changed_evidence = true) const;
    [[nodiscard]] std::optional<TraceRecordSnapshot> LookupRecord(
        const TraceRecordKey& key) const;

    // Counts derive from retained lifecycle state. Active is non-monotonic;
    // lifecycle_counts() returns all three values from the same lock interval.
    [[nodiscard]] std::size_t active_trace_count() const;
    [[nodiscard]] std::size_t quiescent_trace_count() const;
    [[nodiscard]] std::size_t retained_trace_count() const;
    [[nodiscard]] LifecycleCounts lifecycle_counts() const;

private:
    using TestHook = void (*)(void*) noexcept;
    using PreparationHook = void (*)(void*);
    using SpanBatches = std::vector<std::vector<Span>>;
    struct TraceState final {
        SpanBatches batches;
        TraceLifecycle lifecycle{TraceLifecycle::active};
        Clock::time_point last_evidence_time{};
        TraceIncarnationId incarnation{0};
        std::uint64_t publication_sequence{0};
    };
    enum class WorkerState { not_started, running, stopped };

    friend class testing::ActiveTraceManagerTestPeer;

    [[nodiscard]] std::vector<ReconstructedTrace> SnapshotWithHook(
        TestHook before_lock, void* context) const;
    static Clock::time_point ProductionNow(void*) noexcept;
    void LifecycleWorker();
    struct SweepResult final {
        std::optional<Clock::duration> next_wait;
        std::vector<TraceId> evicted_ids;
        std::vector<TraceId> quiesced_ids;
    };
    // Prepare everything before committing lifecycle/retention state.
    SweepResult SweepLifecycleLocked();
    void PublishEvictions(std::vector<TraceId> trace_ids) noexcept;
    void PublishQuiescence(std::vector<TraceId> trace_ids) noexcept;
    [[nodiscard]] TraceRecordMetadata MetadataLocked(
        const TraceId& id, const TraceState& state, Clock::time_point now) const;
    [[nodiscard]] static std::vector<Span> CopyEvidence(const TraceState& state);

    mutable std::mutex mutex_;
    std::map<TraceId, TraceState> batches_by_trace_;
    const Clock::duration quiet_interval_;
    const TraceRetentionLimits retention_limits_;
    const ClockNow clock_now_;
    void* const clock_context_;
    std::condition_variable lifecycle_cv_;
    std::thread lifecycle_thread_;
    WorkerState worker_state_{WorkerState::not_started};
    TraceEvictionSink* eviction_sink_{nullptr};
    TraceLifecycleSink* lifecycle_sink_{nullptr};
    TraceIncarnationId last_incarnation_{0};
    std::uint64_t publication_sequence_{0};
    TestHook before_commit_hook_{nullptr};
    void* before_commit_context_{nullptr};
    TestHook before_sweep_lock_hook_{nullptr};
    void* before_sweep_lock_context_{nullptr};
    TestHook after_sweep_hook_{nullptr};
    void* after_sweep_context_{nullptr};
    PreparationHook before_retention_commit_hook_{nullptr};
    void* before_retention_commit_context_{nullptr};
    TestHook before_eviction_notify_hook_{nullptr};
    void* before_eviction_notify_context_{nullptr};
    TestHook after_stop_request_hook_{nullptr};
    void* after_stop_request_context_{nullptr};
};

} // namespace silhouette
