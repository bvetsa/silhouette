#include "silhouette/active_trace_manager.h"

#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace silhouette {

static_assert(
    noexcept(std::declval<const TraceId&>() < std::declval<const TraceId&>()));
static_assert(std::is_nothrow_move_constructible_v<std::vector<Span>>);

ActiveTraceManager::ActiveTraceManager(const Clock::duration quiet_interval)
    : ActiveTraceManager{quiet_interval, ProductionNow, nullptr}
{
}

ActiveTraceManager::ActiveTraceManager(
    const Clock::duration quiet_interval, const ClockNow now, void* const context)
    : quiet_interval_{quiet_interval}
    , clock_now_{now}
    , clock_context_{context}
{
    if (quiet_interval_ <= Clock::duration::zero()) {
        throw std::invalid_argument{"trace quiet interval must be positive"};
    }
    if (clock_now_ == nullptr) {
        throw std::invalid_argument{"trace observation clock cannot be null"};
    }
}

ActiveTraceManager::~ActiveTraceManager()
{
    StopLifecycle();
}

ActiveTraceManager::Clock::time_point ActiveTraceManager::ProductionNow(
    void*) noexcept
{
    return Clock::now();
}

void ActiveTraceManager::StartLifecycle()
{
    std::scoped_lock lock{mutex_};
    if (worker_state_ != WorkerState::not_started) {
        throw std::logic_error{"trace lifecycle worker can only be started once"};
    }
    // A failed thread construction leaves the manager unstarted.
    lifecycle_thread_ = std::thread{&ActiveTraceManager::LifecycleWorker, this};
    worker_state_ = WorkerState::running;
}

void ActiveTraceManager::StopLifecycle() noexcept
{
    {
        std::scoped_lock lock{mutex_};
        if (worker_state_ != WorkerState::running) {
            return;
        }
        worker_state_ = WorkerState::stopped;
    }
    lifecycle_cv_.notify_one();
    lifecycle_thread_.join();
}

std::optional<ActiveTraceManager::Clock::duration>
ActiveTraceManager::SweepLifecycleLocked() noexcept
{
    const auto now = clock_now_(clock_context_);
    std::optional<Clock::duration> next_wait;
    for (auto& [trace_id, state] : batches_by_trace_) {
        static_cast<void>(trace_id);
        if (state.lifecycle == TraceLifecycle::quiescent) {
            continue;
        }
        const auto elapsed = now - state.last_evidence_time;
        if (elapsed >= quiet_interval_) {
            state.lifecycle = TraceLifecycle::quiescent;
        } else {
            const auto remaining = quiet_interval_ - elapsed;
            if (!next_wait || remaining < *next_wait) {
                next_wait = remaining;
            }
        }
    }
    return next_wait;
}

void ActiveTraceManager::LifecycleWorker()
{
    for (;;) {
        if (before_sweep_lock_hook_ != nullptr) {
            before_sweep_lock_hook_(before_sweep_lock_context_);
        }
        std::unique_lock lock{mutex_};
        if (worker_state_ != WorkerState::running) {
            return;
        }
        const auto remaining = SweepLifecycleLocked();
        if (after_sweep_hook_ != nullptr) {
            after_sweep_hook_(after_sweep_context_);
        }
        // Publication and test wakes acquire the same mutex, so a wake cannot
        // be lost between evaluating eligibility and releasing it to wait.
        // Fake time controls eligibility; this wait uses real elapsed time.
        if (remaining) {
            lifecycle_cv_.wait_for(lock, *remaining);
        } else {
            lifecycle_cv_.wait(lock);
        }
    }
}

std::vector<TraceId> ActiveTraceManager::AppendBatch(std::vector<Span> spans)
{
    if (spans.empty()) {
        return {};
    }

    std::map<TraceId, TraceState> incoming_by_trace;
    for (auto& span : spans) {
        const auto trace_id = span.trace_id;
        auto& batches = incoming_by_trace[trace_id].batches;
        if (batches.empty()) {
            batches.emplace_back();
        }
        batches.front().push_back(std::move(span));
    }

    std::vector<TraceId> affected_trace_ids;
    affected_trace_ids.reserve(incoming_by_trace.size());
    for (const auto& [trace_id, state] : incoming_by_trace) {
        static_cast<void>(state);
        affected_trace_ids.push_back(trace_id);
    }

    std::unique_lock lock{mutex_};

    // Reserve every affected existing trace before changing logical state. A
    // failed allocation can change capacity but cannot publish part of a batch.
    for (const auto& [trace_id, incoming_state] : incoming_by_trace) {
        const auto existing = batches_by_trace_.find(trace_id);
        if (existing == batches_by_trace_.end()) {
            continue;
        }
        const auto& incoming_batches = incoming_state.batches;
        auto& existing_batches = existing->second.batches;
        if (incoming_batches.size()
            > existing_batches.max_size() - existing_batches.size()) {
            throw std::length_error{"active trace has too many span batches"};
        }
        existing_batches.reserve(existing_batches.size() + incoming_batches.size());
    }

    if (before_commit_hook_ != nullptr) {
        before_commit_hook_(before_commit_context_);
    }

    const auto now = clock_now_(clock_context_);

    // Existing traces append prepared chunks without allocating. New map nodes
    // were already allocated; merge allocates nothing, and TraceId ordering
    // cannot throw. No historical span is recopied during ingestion.
    for (auto incoming = incoming_by_trace.begin();
         incoming != incoming_by_trace.end();) {
        incoming->second.last_evidence_time = now;
        const auto existing = batches_by_trace_.find(incoming->first);
        if (existing == batches_by_trace_.end()) {
            ++incoming;
            continue;
        }

        for (auto& batch : incoming->second.batches) {
            existing->second.batches.push_back(std::move(batch));
        }
        existing->second.lifecycle = TraceLifecycle::active;
        existing->second.last_evidence_time = now;
        incoming = incoming_by_trace.erase(incoming);
    }
    batches_by_trace_.merge(incoming_by_trace);
    lock.unlock();
    lifecycle_cv_.notify_one();
    return affected_trace_ids;
}

std::vector<ReconstructedTrace> ActiveTraceManager::Snapshot() const
{
    return SnapshotWithHook(nullptr, nullptr);
}

std::vector<ReconstructedTrace> ActiveTraceManager::SnapshotTraces(
    const std::span<const TraceId> trace_ids) const
{
    const std::set<TraceId> unique_trace_ids{trace_ids.begin(), trace_ids.end()};
    std::vector<Span> spans;
    {
        std::scoped_lock lock{mutex_};

        std::size_t span_count = 0;
        for (const auto& trace_id : unique_trace_ids) {
            const auto found = batches_by_trace_.find(trace_id);
            if (found == batches_by_trace_.end()) {
                continue;
            }
            for (const auto& batch : found->second.batches) {
                span_count += batch.size();
            }
        }

        spans.reserve(span_count);
        for (const auto& trace_id : unique_trace_ids) {
            const auto found = batches_by_trace_.find(trace_id);
            if (found == batches_by_trace_.end()) {
                continue;
            }
            for (const auto& batch : found->second.batches) {
                spans.insert(spans.end(), batch.begin(), batch.end());
            }
        }
    }

    return ReconstructTraces(std::move(spans));
}

std::vector<ReconstructedTrace> ActiveTraceManager::SnapshotWithHook(
    const TestHook before_lock, void* const context) const
{
    if (before_lock != nullptr) {
        before_lock(context);
    }

    std::vector<Span> spans;
    {
        std::scoped_lock lock{mutex_};

        std::size_t span_count = 0;
        for (const auto& [trace_id, state] : batches_by_trace_) {
            static_cast<void>(trace_id);
            for (const auto& batch : state.batches) {
                span_count += batch.size();
            }
        }

        spans.reserve(span_count);
        for (const auto& [trace_id, state] : batches_by_trace_) {
            static_cast<void>(trace_id);
            for (const auto& batch : state.batches) {
                spans.insert(spans.end(), batch.begin(), batch.end());
            }
        }
    }

    return ReconstructTraces(std::move(spans));
}

std::size_t ActiveTraceManager::active_trace_count() const
{
    return lifecycle_counts().active;
}

std::size_t ActiveTraceManager::quiescent_trace_count() const
{
    return lifecycle_counts().quiescent;
}

std::size_t ActiveTraceManager::retained_trace_count() const
{
    std::scoped_lock lock{mutex_};
    return batches_by_trace_.size();
}

ActiveTraceManager::LifecycleCounts ActiveTraceManager::lifecycle_counts() const
{
    std::scoped_lock lock{mutex_};
    LifecycleCounts counts{.retained = batches_by_trace_.size()};
    for (const auto& [trace_id, state] : batches_by_trace_) {
        static_cast<void>(trace_id);
        if (state.lifecycle == TraceLifecycle::active) {
            ++counts.active;
        } else {
            ++counts.quiescent;
        }
    }
    return counts;
}

} // namespace silhouette
