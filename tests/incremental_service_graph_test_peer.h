#pragma once

#include "silhouette/incremental_service_graph.h"

namespace silhouette::testing {

class IncrementalServiceGraphTestPeer final {
public:
    using Hook = void (*)(void*) noexcept;

    static void SetBeforeCommitHook(
        IncrementalServiceGraph& graph, Hook hook, void* context) noexcept
    {
        graph.before_commit_hook_ = hook;
        graph.before_commit_context_ = context;
    }
};

} // namespace silhouette::testing
