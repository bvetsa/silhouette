#pragma once

#include "silhouette/live_service_map.h"

namespace silhouette::testing {

// Existing presentation regressions exercise the same engine/renderer pair,
// now with their ownership and shutdown order explicit in this test fixture.
class QueryLiveMap final : public TraceUpdateSink, public TraceEvictionSink {
public:
    QueryLiveMap(ActiveTraceManager& traces,
                 rendering::ServiceGraphRenderer& renderer, std::ostream& diagnostics)
        : queries_{traces, diagnostics}, map_{queries_, renderer, diagnostics} {}
    ~QueryLiveMap() override { CloseAndDrain(); }
    void Start() { queries_.Start(); map_.Start(); }
    void CloseAndDrain() noexcept { queries_.CloseAndDrain(); map_.CloseAndDrain(); }
    void NotifyTracesDirty(std::vector<TraceId> ids) noexcept override
    { queries_.NotifyTracesDirty(std::move(ids)); }
    void NotifyTracesEvicted(std::vector<TraceId> ids) noexcept override
    { queries_.NotifyTracesEvicted(std::move(ids)); }
    ServiceGraph SnapshotGraph() const { return map_.SnapshotGraph(); }
    std::uint64_t topology_version() const { return map_.topology_version(); }
    auto SnapshotPublished() const { return map_.SnapshotPublished(); }
    template <typename... Args> auto WaitForPublishedAfter(Args&&... args) const
    { return map_.WaitForPublishedAfter(std::forward<Args>(args)...); }
    bool publisher_stopped() const { return map_.publisher_stopped(); }
    operator LiveServiceMap&() { return map_; }
    operator const LiveServiceMap&() const { return map_; }
    ObservabilityQueryService& queries() { return queries_; }
private:
    ObservabilityQueryService queries_;
    LiveServiceMap map_;
};

} // namespace silhouette::testing
