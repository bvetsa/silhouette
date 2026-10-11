#pragma once

#include "silhouette/service_graph.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <span>
#include <string>

namespace silhouette {

namespace testing {
class IncrementalServiceGraphTestPeer;
}

class IncrementalServiceGraph final {
public:
    IncrementalServiceGraph() = default;

    IncrementalServiceGraph(const IncrementalServiceGraph&) = delete;
    IncrementalServiceGraph& operator=(const IncrementalServiceGraph&) = delete;
    IncrementalServiceGraph(IncrementalServiceGraph&&) = delete;
    IncrementalServiceGraph& operator=(IncrementalServiceGraph&&) = delete;

    // Atomically replaces the observable contributions of every supplied
    // trace. If preparation fails, Snapshot() continues to return the previous
    // graph; invisible empty or zero-count bookkeeping entries may remain.
    void UpdateTraces(std::span<const ReconstructedTrace> traces);

    // Atomically replaces present contributions and removes absent ones.
    // The two input ID sets must be disjoint. Unknown/duplicate removals are
    // harmless. Preparation failure preserves the observable previous graph.
    void ReconcileTraces(std::span<const ReconstructedTrace> present_traces,
                         std::span<const TraceId> absent_ids);

    // Failure-recovery path: atomically rebuilds every contribution.
    void ReplaceAll(std::span<const ReconstructedTrace> traces);

    [[nodiscard]] ServiceGraph Snapshot() const;

private:
    using TestHook = void (*)(void*) noexcept;

    friend class testing::IncrementalServiceGraphTestPeer;

    struct State final {
        std::map<TraceId, ServiceGraph> contributions;
        std::map<std::string, std::size_t> service_refcounts;
        std::map<ServiceEdge, std::size_t> edge_refcounts;
        std::map<ServiceGraphGap, std::size_t> gap_refcounts;
    };

    mutable std::mutex mutex_;
    State state_;
    TestHook before_commit_hook_{nullptr};
    void* before_commit_context_{nullptr};
    void (*before_reconcile_commit_hook_)(void*){nullptr};
    void* before_reconcile_commit_context_{nullptr};
};

} // namespace silhouette
