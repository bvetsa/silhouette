#pragma once

#include "silhouette/active_trace_manager.h"

#include <vector>

namespace silhouette::testing {

class ActiveTraceManagerTestPeer final {
public:
    static void SetRecordCounters(ActiveTraceManager& manager,
                                 TraceIncarnationId incarnation,
                                 std::uint64_t publication_sequence)
    {
        std::scoped_lock lock{manager.mutex_};
        manager.last_incarnation_ = incarnation;
        manager.publication_sequence_ = publication_sequence;
    }
    using Hook = void (*)(void*) noexcept;

    static void SetBeforeCommitHook(
        ActiveTraceManager& manager, Hook hook, void* context) noexcept
    {
        manager.before_commit_hook_ = hook;
        manager.before_commit_context_ = context;
    }

    [[nodiscard]] static std::vector<ReconstructedTrace> SnapshotWithBeforeLock(
        const ActiveTraceManager& manager, Hook hook, void* context)
    {
        return manager.SnapshotWithHook(hook, context);
    }

    // Configure hooks only before starting the worker or after stopping it.
    static void SetBeforeSweepLockHook(
        ActiveTraceManager& manager, Hook hook, void* context) noexcept
    {
        manager.before_sweep_lock_hook_ = hook;
        manager.before_sweep_lock_context_ = context;
    }

    static void SetAfterSweepHook(
        ActiveTraceManager& manager, Hook hook, void* context) noexcept
    {
        manager.after_sweep_hook_ = hook;
        manager.after_sweep_context_ = context;
    }

    static void WakeLifecycle(
        ActiveTraceManager& manager, Hook before_wake = nullptr,
        void* context = nullptr)
    {
        std::scoped_lock lock{manager.mutex_};
        if (before_wake != nullptr) {
            before_wake(context);
        }
        manager.lifecycle_cv_.notify_one();
    }

    [[nodiscard]] static std::optional<ActiveTraceManager::Clock::duration>
    SweepLifecycle(ActiveTraceManager& manager)
    {
        ActiveTraceManager::SweepResult result;
        {
            std::scoped_lock lock{manager.mutex_};
            result = manager.SweepLifecycleLocked();
        }
        manager.PublishEvictions(std::move(result.evicted_ids));
        manager.PublishQuiescence(std::move(result.quiesced_ids));
        return result.next_wait;
    }

    static void SetBeforeRetentionCommitHook(
        ActiveTraceManager& manager, void (*hook)(void*), void* context)
    {
        manager.before_retention_commit_hook_ = hook;
        manager.before_retention_commit_context_ = context;
    }

    static void SetBeforeEvictionNotifyHook(
        ActiveTraceManager& manager, Hook hook, void* context)
    {
        manager.before_eviction_notify_hook_ = hook;
        manager.before_eviction_notify_context_ = context;
    }

    static void SetAfterStopRequestHook(
        ActiveTraceManager& manager, Hook hook, void* context)
    {
        manager.after_stop_request_hook_ = hook;
        manager.after_stop_request_context_ = context;
    }

    [[nodiscard]] static ActiveTraceManager::LifecycleCounts
    CountsWithBeforeLock(const ActiveTraceManager& manager,
                         Hook hook, void* context)
    {
        hook(context);
        return manager.lifecycle_counts();
    }
};

} // namespace silhouette::testing
