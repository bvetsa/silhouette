#include "silhouette/live_service_map.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include "diagnostics_mutex.h"
#include <utility>

namespace silhouette {

LiveServiceMap::LiveServiceMap(
    ObservabilityQueryService& queries,
    rendering::ServiceGraphRenderer& renderer,
    std::ostream& diagnostics)
    : queries_{queries}
    , renderer_{renderer}
    , diagnostics_{diagnostics}
{
    queries_.SetChangeSink(this);
}

LiveServiceMap::~LiveServiceMap()
{
    CloseAndDrain();
}

void LiveServiceMap::Start()
{
    if (started_) {
        throw std::logic_error{"live service map can only be started once"};
    }
    started_ = true;
    render_thread_ = std::thread{[this] {
        try {
            RenderWorker();
        } catch (const std::exception& error) {
            Report("render worker stopped unexpectedly", error.what());
        } catch (...) {
            Report("render worker stopped with an unknown error");
        }
    }};
}

void LiveServiceMap::NotifyQueryChanged(QueryChange change) noexcept
{
    {
        std::scoped_lock lock{published_mutex_};
        observation_version_ = change.observation_version;
    }
    published_cv_.notify_all();
    if (change.topology) {
        try { PublishTopology(std::move(*change.topology), change.has_telemetry); }
        catch (const std::exception& error) { Report("topology handoff failed", error.what()); }
        catch (...) { Report("topology handoff failed"); }
    }
}

PresentationVersions LiveServiceMap::WaitForVersionsAfter(
    const std::optional<std::uint64_t> rendered_version,
    const std::optional<std::uint64_t> observation_version,
    const std::chrono::milliseconds timeout, const std::stop_token stop_token) const
{
    std::unique_lock lock{published_mutex_};
    published_cv_.wait_for(lock, stop_token, timeout, [&] {
        return publisher_stopped_ || !observation_version
            || observation_version_ > *observation_version
            || (published_ && (!rendered_version || published_->rendered_version > *rendered_version));
    });
    return {published_ ? std::optional{published_->rendered_version} : std::nullopt,
            observation_version_, published_ && published_->has_telemetry, publisher_stopped_};
}

void LiveServiceMap::CloseAndDrain() noexcept
{
    if (!started_ || closed_) {
        return;
    }
    {
        std::scoped_lock lock{topology_mutex_};
        topology_input_closed_ = true;
    }
    topology_cv_.notify_one();
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    {
        std::scoped_lock lock{published_mutex_};
        publisher_stopped_ = true;
    }
    published_cv_.notify_all();
    closed_ = true;
}

ServiceGraph LiveServiceMap::SnapshotGraph() const
{
    return queries_.SnapshotGraph();
}

std::uint64_t LiveServiceMap::topology_version() const
{
    std::scoped_lock lock{topology_mutex_};
    return topology_version_;
}

std::optional<PublishedServiceMap> LiveServiceMap::SnapshotPublished() const
{
    std::scoped_lock lock{published_mutex_};
    return published_;
}

std::optional<PublishedServiceMap> LiveServiceMap::WaitForPublishedAfter(
    const std::optional<std::uint64_t> rendered_version,
    const std::stop_token stop_token) const
{
    std::unique_lock lock{published_mutex_};
    const auto ready = published_cv_.wait(lock, stop_token, [&] {
        return publisher_stopped_
            || (published_ && (!rendered_version
                || published_->rendered_version > *rendered_version));
    });
    if (!ready || publisher_stopped_) {
        return std::nullopt;
    }
    return published_;
}

std::optional<PublishedServiceMap> LiveServiceMap::WaitForPublishedAfter(
    const std::optional<std::uint64_t> rendered_version,
    const std::chrono::milliseconds timeout,
    const std::stop_token stop_token) const
{
    std::unique_lock lock{published_mutex_};
    const auto ready = published_cv_.wait_for(
        lock, stop_token, timeout, [&] {
            return publisher_stopped_
                || (published_ && (!rendered_version
                    || published_->rendered_version > *rendered_version));
        });
    if (!ready || publisher_stopped_) {
        return std::nullopt;
    }
    return published_;
}

bool LiveServiceMap::publisher_stopped() const
{
    std::scoped_lock lock{published_mutex_};
    return publisher_stopped_;
}

void LiveServiceMap::RenderWorker()
{
    for (;;) {
        ServiceGraph graph;
        std::uint64_t version = 0;
        bool has_telemetry = false;
        {
            std::unique_lock lock{topology_mutex_};
            if (!render_dirty_ && !topology_input_closed_
                && before_render_wait_hook_) {
                before_render_wait_hook_(before_render_wait_context_);
            }
            topology_cv_.wait(lock, [&] {
                return render_dirty_ || topology_input_closed_;
            });
            if (topology_input_closed_ && !render_dirty_) {
                break;
            }
            graph = latest_topology_;
            version = topology_version_;
            has_telemetry = topology_has_telemetry_;
            render_dirty_ = false;
        }

        rendering::SvgRenderResult result;
        try {
            result = renderer_.RenderSvg(graph);
        } catch (const std::exception& error) {
            Report("live SVG rendering failed", error.what());
            continue;
        } catch (...) {
            Report("live SVG rendering failed with an unknown error");
            continue;
        }
        if (!result.succeeded()) {
            Report(result.error);
            continue;
        }

        bool stale = false;
        {
            // Keep topology validation and SVG publication atomic with respect
            // to PublishTopology(). Lock order is topology, then published.
            std::scoped_lock topology_lock{topology_mutex_};
            if (topology_version_ != version) {
                render_dirty_ = true;
                stale = true;
            } else {
                std::scoped_lock published_lock{published_mutex_};
                published_ = PublishedServiceMap{
                    .rendered_version = version,
                    .has_telemetry = has_telemetry,
                    .svg = std::move(result.svg),
                };
                latest_successful_rendered_version_ = version;
                // Equivalent events during this attempt may have queued a
                // retry. This successful current SVG makes that work redundant.
                render_dirty_ = false;
            }
        }
        if (stale) {
            topology_cv_.notify_one();
            continue;
        }
        published_cv_.notify_all();
    }
}

void LiveServiceMap::PublishTopology(
    ServiceGraph graph, const bool has_telemetry)
{
    {
        std::scoped_lock lock{topology_mutex_};
        // Contributions/refcounts are already updated. Equivalent evidence
        // keeps its version, but a new event retries a not-yet-rendered state.
        if (latest_topology_ == graph
            && topology_has_telemetry_ == has_telemetry) {
            if (latest_successful_rendered_version_ == topology_version_) {
                return;
            }
        } else {
            latest_topology_ = std::move(graph);
            ++topology_version_;
            topology_has_telemetry_ = has_telemetry;
        }
        render_dirty_ = true;
    }
    topology_cv_.notify_one();
}

void LiveServiceMap::Report(
    const std::string_view message,
    const std::string_view detail) noexcept
{
    try {
        std::scoped_lock lock{detail::DiagnosticsMutex()};
        diagnostics_ << "Silhouette live map: " << message;
        if (!detail.empty()) {
            diagnostics_ << ": " << detail;
        }
        diagnostics_ << '\n';
    } catch (...) {
    }
}

} // namespace silhouette
