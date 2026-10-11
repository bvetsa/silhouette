#pragma once

#include "silhouette/span.h"
#include "silhouette/trace_reconstruction.h"

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
    ~ActiveTraceManager();

    ActiveTraceManager(const ActiveTraceManager&) = delete;
    ActiveTraceManager& operator=(const ActiveTraceManager&) = delete;
    ActiveTraceManager(ActiveTraceManager&&) = delete;
    ActiveTraceManager& operator=(ActiveTraceManager&&) = delete;

    // Owner-thread operations, serialized with each other and destruction.
    // Start is one-shot; stopping before start does not consume that start.
    void StartLifecycle();
    void StopLifecycle() noexcept;

    // Publishes either the complete non-empty batch or none of it.
    std::vector<TraceId> AppendBatch(std::vector<Span> spans);

    // Returns an owned point-in-time reconstruction. Later ingestion cannot
    // change the returned traces.
    [[nodiscard]] std::vector<ReconstructedTrace> Snapshot() const;

    // Returns owned reconstructions for the requested trace IDs. Duplicate or
    // unknown IDs do not duplicate evidence or create empty traces.
    [[nodiscard]] std::vector<ReconstructedTrace> SnapshotTraces(
        std::span<const TraceId> trace_ids) const;

    // Counts derive from retained lifecycle state. Active is non-monotonic;
    // lifecycle_counts() returns all three values from the same lock interval.
    [[nodiscard]] std::size_t active_trace_count() const;
    [[nodiscard]] std::size_t quiescent_trace_count() const;
    [[nodiscard]] std::size_t retained_trace_count() const;
    [[nodiscard]] LifecycleCounts lifecycle_counts() const;

private:
    using TestHook = void (*)(void*) noexcept;
    using SpanBatches = std::vector<std::vector<Span>>;
    enum class TraceLifecycle { active, quiescent };
    struct TraceState final {
        SpanBatches batches;
        TraceLifecycle lifecycle{TraceLifecycle::active};
        Clock::time_point last_evidence_time{};
    };
    enum class WorkerState { not_started, running, stopped };

    friend class testing::ActiveTraceManagerTestPeer;

    [[nodiscard]] std::vector<ReconstructedTrace> SnapshotWithHook(
        TestHook before_lock, void* context) const;
    static Clock::time_point ProductionNow(void*) noexcept;
    void LifecycleWorker();
    // Samples once under mutex_; returns the remaining time to the next deadline.
    std::optional<Clock::duration> SweepLifecycleLocked() noexcept;

    mutable std::mutex mutex_;
    std::map<TraceId, TraceState> batches_by_trace_;
    const Clock::duration quiet_interval_;
    const ClockNow clock_now_;
    void* const clock_context_;
    std::condition_variable lifecycle_cv_;
    std::thread lifecycle_thread_;
    WorkerState worker_state_{WorkerState::not_started};
    TestHook before_commit_hook_{nullptr};
    void* before_commit_context_{nullptr};
    TestHook before_sweep_lock_hook_{nullptr};
    void* before_sweep_lock_context_{nullptr};
    TestHook after_sweep_hook_{nullptr};
    void* after_sweep_context_{nullptr};
};

} // namespace silhouette
