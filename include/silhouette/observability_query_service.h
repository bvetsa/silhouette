#pragma once

#include "silhouette/active_trace_manager.h"
#include "silhouette/incremental_service_graph.h"
#include "silhouette/trace_provenance_index.h"
#include "silhouette/trace_update_sink.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <thread>

namespace silhouette {
namespace testing {
class ObservabilityQueryServiceTestPeer;
class LiveServiceMapTestPeer;
}

struct ObservationInstanceId final {
    std::string value;
    bool operator==(const ObservationInstanceId&) const = default;
};

struct TraceSummary final {
    TraceRecordMetadata metadata;
    std::size_t service_count;
};

struct SupportCursor final {
    ObservationInstanceId instance;
    std::uint64_t publication_sequence;
    TraceRecordKey key;
};

struct SupportPage final {
    ObservationInstanceId instance;
    std::uint64_t observation_version;
    std::vector<TraceSummary> traces;
    std::optional<SupportCursor> next_cursor;
};

struct TraceDetail final {
    ObservationInstanceId instance;
    std::uint64_t observation_version;
    TraceRecordMetadata metadata;
    ReconstructedTrace trace;
    ServiceGraph graph;
};

struct QueryChange final {
    std::uint64_t topology_version;
    std::uint64_t observation_version;
    bool has_telemetry;
    // Absent for a metadata-only invalidation. Present for evidence events,
    // including equivalent evidence which may retry a failed presentation.
    std::optional<ServiceGraph> topology;
};

class QueryChangeSink {
public:
    virtual ~QueryChangeSink() = default;
    virtual void NotifyQueryChanged(QueryChange change) noexcept = 0;
};

class ObservabilityQueryService final : public TraceUpdateSink,
                                        public TraceEvictionSink,
                                        public TraceLifecycleSink {
public:
    // Manager and diagnostics outlive the service. Sink is configured before
    // Start(), and remains alive until CloseAndDrain() joins the worker.
    ObservabilityQueryService(ActiveTraceManager& traces, std::ostream& diagnostics);
    ~ObservabilityQueryService() override;
    ObservabilityQueryService(const ObservabilityQueryService&) = delete;
    ObservabilityQueryService& operator=(const ObservabilityQueryService&) = delete;

    void SetChangeSink(QueryChangeSink* sink);
    void Start();
    void CloseAndDrain() noexcept;
    void NotifyTracesDirty(std::vector<TraceId> ids) noexcept override;
    void NotifyTracesEvicted(std::vector<TraceId> ids) noexcept override;
    void NotifyTracesQuiesced(std::vector<TraceId> ids) noexcept override;

    [[nodiscard]] SupportPage QuerySupport(
        const TopologyFact& fact, const std::optional<SupportCursor>& cursor = {}) const;
    [[nodiscard]] std::optional<TraceDetail> QueryTrace(const TraceRecordKey& key) const;
    [[nodiscard]] ServiceGraph SnapshotGraph() const;
    [[nodiscard]] std::uint64_t topology_version() const;
    [[nodiscard]] std::uint64_t observation_version() const;
    [[nodiscard]] const ObservationInstanceId& observation_instance_id() const noexcept;

private:
    friend class testing::ObservabilityQueryServiceTestPeer;
    friend class testing::LiveServiceMapTestPeer;
    using TestHook = void (*)(void*);
    void Notify(std::vector<TraceId> ids, bool evidence) noexcept;
    void Worker();
    void Reconcile(std::span<const TraceId> ids, bool full, bool evidence);
    void Report(std::string_view message, std::string_view detail = {}) noexcept;

    ActiveTraceManager& traces_;
    std::ostream& diagnostics_;
    const ObservationInstanceId instance_;
    QueryChangeSink* sink_{nullptr};
    mutable std::mutex publication_mutex_;
    IncrementalServiceGraph graph_;
    TraceProvenanceIndex provenance_;
    std::map<TraceId, TraceSummary> records_;
    std::uint64_t topology_version_{0};
    std::uint64_t observation_version_{0};

    mutable std::mutex dirty_mutex_;
    std::condition_variable dirty_cv_;
    std::set<TraceId> dirty_trace_ids_;
    std::atomic<std::uint64_t> resync_requests_{0};
    bool evidence_pending_{false};
    bool invalidation_input_closed_{false};
    std::thread worker_;
    bool started_{false};
    bool closed_{false};
    TestHook before_dirty_insert_hook_{nullptr};
    void* before_dirty_insert_context_{nullptr};
    TestHook after_dirty_insert_hook_{nullptr};
    void* after_dirty_insert_context_{nullptr};
    TestHook before_trace_snapshot_hook_{nullptr};
    void* before_trace_snapshot_context_{nullptr};
    TestHook after_trace_lookup_hook_{nullptr};
    void* after_trace_lookup_context_{nullptr};
    TestHook after_topology_update_hook_{nullptr};
    void* after_topology_update_context_{nullptr};
    TestHook before_provenance_prepare_hook_{nullptr};
    void* before_provenance_prepare_context_{nullptr};
    TestHook before_graph_commit_hook_{nullptr};
    void* before_graph_commit_context_{nullptr};
};

} // namespace silhouette
