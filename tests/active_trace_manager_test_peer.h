#pragma once

#include "silhouette/active_trace_manager.h"

#include <vector>

namespace silhouette::testing {

class ActiveTraceManagerTestPeer final {
public:
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
};

} // namespace silhouette::testing
