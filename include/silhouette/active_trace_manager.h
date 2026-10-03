#pragma once

#include "silhouette/span.h"
#include "silhouette/trace_reconstruction.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

namespace silhouette {

namespace testing {
class ActiveTraceManagerTestPeer;
}

class ActiveTraceManager final {
public:
    ActiveTraceManager() = default;

    ActiveTraceManager(const ActiveTraceManager&) = delete;
    ActiveTraceManager& operator=(const ActiveTraceManager&) = delete;
    ActiveTraceManager(ActiveTraceManager&&) = delete;
    ActiveTraceManager& operator=(ActiveTraceManager&&) = delete;

    // Publishes either the complete non-empty batch or none of it.
    void AppendBatch(std::vector<Span> spans);

    // Returns an owned point-in-time reconstruction. Later ingestion cannot
    // change the returned traces.
    [[nodiscard]] std::vector<ReconstructedTrace> Snapshot() const;

    [[nodiscard]] std::size_t active_trace_count() const;

private:
    using TestHook = void (*)(void*) noexcept;
    using SpanBatches = std::vector<std::vector<Span>>;

    friend class testing::ActiveTraceManagerTestPeer;

    [[nodiscard]] std::vector<ReconstructedTrace> SnapshotWithHook(
        TestHook before_lock, void* context) const;

    mutable std::mutex mutex_;
    std::map<TraceId, SpanBatches> batches_by_trace_;
    TestHook before_commit_hook_{nullptr};
    void* before_commit_context_{nullptr};
};

} // namespace silhouette
