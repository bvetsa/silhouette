#pragma once

#include "silhouette/active_trace_manager.h"
#include "silhouette/incremental_service_graph.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "silhouette/trace_update_sink.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace silhouette {

namespace testing {
class LiveServiceMapTestPeer;
}

struct PublishedServiceMap final {
    std::uint64_t rendered_version;
    bool has_telemetry;
    std::string svg;
};

class LiveServiceMap final : public TraceUpdateSink {
public:
    // All referenced objects must outlive this live-map instance.
    LiveServiceMap(
        ActiveTraceManager& active_traces,
        rendering::ServiceGraphRenderer& renderer,
        std::ostream& diagnostics);
    ~LiveServiceMap() override;

    LiveServiceMap(const LiveServiceMap&) = delete;
    LiveServiceMap& operator=(const LiveServiceMap&) = delete;
    LiveServiceMap(LiveServiceMap&&) = delete;
    LiveServiceMap& operator=(LiveServiceMap&&) = delete;

    void Start();
    void NotifyTracesDirty(std::vector<TraceId> trace_ids) noexcept override;

    // Called only after the OTLP receiver has stopped and joined its handlers.
    void CloseAndDrain() noexcept;

    [[nodiscard]] ServiceGraph SnapshotGraph() const;
    [[nodiscard]] std::uint64_t topology_version() const;
    [[nodiscard]] std::optional<PublishedServiceMap> SnapshotPublished() const;

    // nullopt means the live-map publisher has shut down.
    [[nodiscard]] std::optional<PublishedServiceMap> WaitForPublishedAfter(
        std::optional<std::uint64_t> rendered_version,
        std::stop_token stop_token = {}) const;

private:
    using TestHook = void (*)(void*);

    friend class testing::LiveServiceMapTestPeer;

    void GraphWorker();
    void RenderWorker();
    void PublishTopology(ServiceGraph graph, bool has_telemetry);
    void Report(
        std::string_view message,
        std::string_view detail = {}) noexcept;

    ActiveTraceManager& active_traces_;
    rendering::ServiceGraphRenderer& renderer_;
    std::ostream& diagnostics_;
    mutable std::mutex diagnostics_mutex_;
    IncrementalServiceGraph graph_;

    mutable std::mutex dirty_mutex_;
    std::condition_variable dirty_cv_;
    std::set<TraceId> dirty_trace_ids_;
    std::atomic<std::uint64_t> resync_requests_{0};
    bool invalidation_input_closed_{false};

    mutable std::mutex topology_mutex_;
    std::condition_variable topology_cv_;
    ServiceGraph latest_topology_;
    std::uint64_t topology_version_{0};
    bool topology_has_telemetry_{false};
    bool render_dirty_{true};
    bool topology_input_closed_{false};

    mutable std::mutex published_mutex_;
    mutable std::condition_variable_any published_cv_;
    std::optional<PublishedServiceMap> published_;
    bool publisher_stopped_{false};

    std::thread graph_thread_;
    std::thread render_thread_;
    bool started_{false};
    bool closed_{false};
    TestHook before_dirty_merge_hook_{nullptr};
    void* before_dirty_merge_context_{nullptr};
};

} // namespace silhouette
