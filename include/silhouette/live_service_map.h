#pragma once

#include "silhouette/observability_query_service.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "silhouette/trace_update_sink.h"

#include <atomic>
#include <chrono>
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
    // Version of the observable topology state used for this successful SVG.
    std::uint64_t rendered_version;
    bool has_telemetry;
    std::string svg;
};

struct PresentationVersions final {
    std::optional<std::uint64_t> rendered_version;
    std::uint64_t observation_version;
    bool has_telemetry;
    bool stopped;
};

class LiveServiceMap final : public QueryChangeSink {
public:
    // All referenced objects must outlive this live-map instance.
    LiveServiceMap(
        ObservabilityQueryService& queries,
        rendering::ServiceGraphRenderer& renderer,
        std::ostream& diagnostics);
    ~LiveServiceMap() override;

    LiveServiceMap(const LiveServiceMap&) = delete;
    LiveServiceMap& operator=(const LiveServiceMap&) = delete;
    LiveServiceMap(LiveServiceMap&&) = delete;
    LiveServiceMap& operator=(LiveServiceMap&&) = delete;

    void Start();
    void NotifyQueryChanged(QueryChange change) noexcept override;
    [[nodiscard]] PresentationVersions WaitForVersionsAfter(
        std::optional<std::uint64_t> rendered_version,
        std::optional<std::uint64_t> observation_version,
        std::chrono::milliseconds timeout, std::stop_token stop_token = {}) const;

    // Called only after OTLP and the lifecycle worker have stopped and joined.
    void CloseAndDrain() noexcept;

    [[nodiscard]] ServiceGraph SnapshotGraph() const;
    // Advances only when the deterministic graph or has_telemetry changes.
    [[nodiscard]] std::uint64_t topology_version() const;
    [[nodiscard]] std::optional<PublishedServiceMap> SnapshotPublished() const;

    // nullopt means the live-map publisher has shut down.
    [[nodiscard]] std::optional<PublishedServiceMap> WaitForPublishedAfter(
        std::optional<std::uint64_t> rendered_version,
        std::stop_token stop_token = {}) const;

    // Returns nullopt on timeout, cancellation, or publisher shutdown. Callers
    // that need to distinguish shutdown may query publisher_stopped().
    [[nodiscard]] std::optional<PublishedServiceMap> WaitForPublishedAfter(
        std::optional<std::uint64_t> rendered_version,
        std::chrono::milliseconds timeout,
        std::stop_token stop_token = {}) const;
    [[nodiscard]] bool publisher_stopped() const;

private:
    using TestHook = void (*)(void*);

    friend class testing::LiveServiceMapTestPeer;

    void RenderWorker();
    void PublishTopology(ServiceGraph graph, bool has_telemetry);
    void Report(
        std::string_view message,
        std::string_view detail = {}) noexcept;

    ObservabilityQueryService& queries_;
    rendering::ServiceGraphRenderer& renderer_;
    std::ostream& diagnostics_;
    mutable std::mutex topology_mutex_;
    std::condition_variable topology_cv_;
    ServiceGraph latest_topology_;
    std::uint64_t topology_version_{0};
    // Guarded by topology_mutex_; nullopt distinguishes no SVG from version 0.
    std::optional<std::uint64_t> latest_successful_rendered_version_;
    bool topology_has_telemetry_{false};
    bool render_dirty_{true};
    bool topology_input_closed_{false};

    mutable std::mutex published_mutex_;
    mutable std::condition_variable_any published_cv_;
    std::optional<PublishedServiceMap> published_;
    bool publisher_stopped_{false};
    std::uint64_t observation_version_{0};

    std::thread render_thread_;
    bool started_{false};
    bool closed_{false};
    TestHook before_render_wait_hook_{nullptr};
    void* before_render_wait_context_{nullptr};
};

} // namespace silhouette
