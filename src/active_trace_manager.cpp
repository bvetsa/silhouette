#include "silhouette/active_trace_manager.h"

#include <algorithm>
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
    : ActiveTraceManager{quiet_interval, TraceRetentionLimits{}, ProductionNow, nullptr}
{
}

ActiveTraceManager::ActiveTraceManager(
    const Clock::duration quiet_interval, const ClockNow now, void* const context)
    : ActiveTraceManager{quiet_interval, TraceRetentionLimits{}, now, context}
{
}

ActiveTraceManager::ActiveTraceManager(
    const Clock::duration quiet_interval, const TraceRetentionLimits limits)
    : ActiveTraceManager{quiet_interval, limits, ProductionNow, nullptr}
{
}

ActiveTraceManager::ActiveTraceManager(
    const Clock::duration quiet_interval, const TraceRetentionLimits limits,
    const ClockNow now, void* const context)
    : quiet_interval_{quiet_interval}
    , retention_limits_{limits}
    , clock_now_{now}
    , clock_context_{context}
{
    if (quiet_interval_ <= Clock::duration::zero()) {
        throw std::invalid_argument{"trace quiet interval must be positive"};
    }
    if (retention_limits_.max_quiescent_age <= Clock::duration::zero()
        || retention_limits_.max_spans_per_trace == 0) {
        throw std::invalid_argument{"trace retention age and span limit must be positive"};
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

void ActiveTraceManager::StartLifecycle(TraceEvictionSink* const eviction_sink)
{
    std::scoped_lock lock{mutex_};
    if (worker_state_ != WorkerState::not_started) {
        throw std::logic_error{"trace lifecycle worker can only be started once"};
    }
    // A failed thread construction leaves the manager unstarted.
    lifecycle_thread_ = std::thread{&ActiveTraceManager::LifecycleWorker, this};
    eviction_sink_ = eviction_sink;
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
    if (after_stop_request_hook_) {
        after_stop_request_hook_(after_stop_request_context_);
    }
    lifecycle_cv_.notify_one();
    lifecycle_thread_.join();
}

ActiveTraceManager::SweepResult ActiveTraceManager::SweepLifecycleLocked()
{
    const auto now = clock_now_(clock_context_);
    std::vector<std::pair<Clock::time_point, TraceId>> quiescent;
    for (const auto& [trace_id, state] : batches_by_trace_) {
        if (state.lifecycle == TraceLifecycle::quiescent
            || now - state.last_evidence_time >= quiet_interval_) {
            quiescent.emplace_back(state.last_evidence_time, trace_id);
        }
    }
    std::sort(quiescent.begin(), quiescent.end());
    std::size_t expired = 0;
    while (expired < quiescent.size()
           && now - quiescent[expired].first >= retention_limits_.max_quiescent_age) {
        ++expired;
    }
    const auto remaining = quiescent.size() - expired;
    const auto overflow = remaining > retention_limits_.max_quiescent_traces
        ? remaining - retention_limits_.max_quiescent_traces : 0;
    SweepResult result;
    result.evicted_ids.reserve(expired + overflow);
    for (std::size_t i = 0; i < expired + overflow; ++i) {
        result.evicted_ids.push_back(quiescent[i].second);
    }
    if (before_retention_commit_hook_) {
        before_retention_commit_hook_(before_retention_commit_context_);
    }

    // Everything below is non-allocating. A preparation failure commits
    // neither quiescence nor eviction, and the next sweep reevaluates all state.
    for (auto& [trace_id, state] : batches_by_trace_) {
        static_cast<void>(trace_id);
        const auto elapsed = now - state.last_evidence_time;
        if (state.lifecycle == TraceLifecycle::active && elapsed >= quiet_interval_) {
            state.lifecycle = TraceLifecycle::quiescent;
        }
    }
    for (const auto& trace_id : result.evicted_ids) {
        batches_by_trace_.erase(trace_id);
    }
    for (const auto& [trace_id, state] : batches_by_trace_) {
        static_cast<void>(trace_id);
        const auto elapsed = now - state.last_evidence_time;
        const auto deadline_age = state.lifecycle == TraceLifecycle::active
            ? quiet_interval_ : retention_limits_.max_quiescent_age;
        const auto wait = deadline_age - elapsed;
        if (!result.next_wait || wait < *result.next_wait) {
            result.next_wait = wait;
        }
    }
    return result;
}

void ActiveTraceManager::PublishEvictions(std::vector<TraceId> trace_ids) noexcept
{
    if (trace_ids.empty() || eviction_sink_ == nullptr) {
        return;
    }
    if (before_eviction_notify_hook_) {
        before_eviction_notify_hook_(before_eviction_notify_context_);
    }
    eviction_sink_->NotifyTracesEvicted(std::move(trace_ids));
}

void ActiveTraceManager::LifecycleWorker()
{
    for (;;) {
        if (before_sweep_lock_hook_) {
            before_sweep_lock_hook_(before_sweep_lock_context_);
        }
        std::unique_lock lock{mutex_};
        if (worker_state_ != WorkerState::running) {
            return;
        }
        SweepResult result;
        try {
            result = SweepLifecycleLocked();
        } catch (...) {
            // No state was committed. Resource exhaustion can delay lifecycle
            // and retention; retry without terminating or spinning.
            if (after_sweep_hook_) {
                after_sweep_hook_(after_sweep_context_);
            }
            lifecycle_cv_.wait_for(lock, std::chrono::seconds{1});
            continue;
        }
        if (after_sweep_hook_) {
            after_sweep_hook_(after_sweep_context_);
        }
        if (!result.evicted_ids.empty() && eviction_sink_ != nullptr) {
            lock.unlock();
            PublishEvictions(std::move(result.evicted_ids));
            lock.lock();
            // Publication may have happened during the callback. Reevaluate
            // immediately rather than sleeping on a stale pre-callback wait.
            continue;
        }
        if (result.next_wait) {
            lifecycle_cv_.wait_for(lock, *result.next_wait);
        } else {
            lifecycle_cv_.wait(lock);
        }
    }
}

AppendResult ActiveTraceManager::AppendBatch(std::vector<Span> spans)
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

    AppendResult result;
    result.affected_trace_ids.reserve(incoming_by_trace.size());
    std::unique_lock lock{mutex_};
    for (auto incoming = incoming_by_trace.begin(); incoming != incoming_by_trace.end();) {
        std::size_t retained = 0;
        const auto existing = batches_by_trace_.find(incoming->first);
        if (existing != batches_by_trace_.end()) {
            for (const auto& batch : existing->second.batches) {
                retained += batch.size();
            }
        }
        const auto count = incoming->second.batches.front().size();
        if (count > retention_limits_.max_spans_per_trace - retained) {
            result.capacity_rejected_span_count += count;
            incoming = incoming_by_trace.erase(incoming);
        } else {
            result.accepted_span_count += count;
            result.affected_trace_ids.push_back(incoming->first);
            ++incoming;
        }
    }
    if (incoming_by_trace.empty()) {
        return result;
    }

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
    return result;
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

std::vector<TraceLookup> ActiveTraceManager::LookupTraces(
    const std::span<const TraceId> trace_ids) const
{
    const std::set<TraceId> unique_ids{trace_ids.begin(), trace_ids.end()};
    // Retained records always own non-empty evidence. This one owned snapshot
    // therefore captures both presence and evidence at the same lock boundary.
    auto traces = SnapshotTraces(trace_ids);
    std::vector<TraceLookup> result;
    result.reserve(unique_ids.size());
    auto trace = traces.begin();
    for (const auto& id : unique_ids) {
        if (trace != traces.end() && trace->trace_id == id) {
            result.push_back({id, std::move(*trace)});
            ++trace;
        } else {
            result.push_back({id, std::nullopt});
        }
    }
    return result;
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
