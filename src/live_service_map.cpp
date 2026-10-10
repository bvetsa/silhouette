#include "silhouette/live_service_map.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace silhouette {

LiveServiceMap::LiveServiceMap(
    ActiveTraceManager& active_traces,
    rendering::ServiceGraphRenderer& renderer,
    std::ostream& diagnostics)
    : active_traces_{active_traces}
    , renderer_{renderer}
    , diagnostics_{diagnostics}
{
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
    graph_thread_ = std::thread{[this] {
        try {
            GraphWorker();
        } catch (const std::exception& error) {
            Report("graph worker stopped unexpectedly", error.what());
        } catch (...) {
            Report("graph worker stopped with an unknown error");
        }

        {
            std::scoped_lock lock{dirty_mutex_};
            invalidation_input_closed_ = true;
        }
        {
            std::scoped_lock lock{topology_mutex_};
            topology_input_closed_ = true;
        }
        topology_cv_.notify_one();
    }};
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

void LiveServiceMap::NotifyTracesDirty(
    std::vector<TraceId> trace_ids) noexcept
{
    std::scoped_lock lock{dirty_mutex_};
    if (invalidation_input_closed_) {
        return;
    }
    try {
        for (const auto& trace_id : trace_ids) {
            if (before_dirty_insert_hook_) {
                before_dirty_insert_hook_(before_dirty_insert_context_);
            }
            dirty_trace_ids_.insert(trace_id);
            if (after_dirty_insert_hook_) {
                after_dirty_insert_hook_(after_dirty_insert_context_);
            }
        }
    } catch (...) {
        // Once dirty-set insertion fails, the dirty set is advisory only until
        // ReplaceAll() successfully resynchronizes from ActiveTraceManager.
        // Publish the wait predicate under its mutex so the worker cannot miss
        // the recovery notification when even the first insertion failed.
        resync_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    dirty_cv_.notify_one();
}

void LiveServiceMap::CloseAndDrain() noexcept
{
    if (!started_ || closed_) {
        return;
    }
    {
        std::scoped_lock lock{dirty_mutex_};
        invalidation_input_closed_ = true;
    }
    dirty_cv_.notify_one();
    if (graph_thread_.joinable()) {
        graph_thread_.join();
    }
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
    return graph_.Snapshot();
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

void LiveServiceMap::GraphWorker()
{
    std::uint64_t completed_resync = 0;
    std::uint64_t failed_resync = 0;

    for (;;) {
        std::vector<TraceId> dirty_ids;
        bool should_resync = false;
        std::uint64_t resync_target = completed_resync;
        {
            std::unique_lock lock{dirty_mutex_};
            dirty_cv_.wait(lock, [&] {
                const auto requested =
                    resync_requests_.load(std::memory_order_relaxed);
                return invalidation_input_closed_ || !dirty_trace_ids_.empty()
                    || requested > std::max(completed_resync, failed_resync);
            });

            const auto requested =
                resync_requests_.load(std::memory_order_relaxed);
            should_resync = requested > completed_resync;
            resync_target = requested;
            dirty_ids.assign(
                dirty_trace_ids_.begin(), dirty_trace_ids_.end());
            dirty_trace_ids_.clear();

            if (invalidation_input_closed_ && dirty_ids.empty()
                && !should_resync) {
                break;
            }
        }

        try {
            if (should_resync) {
                const auto traces = active_traces_.Snapshot();
                graph_.ReplaceAll(traces);
                // The authoritative snapshot and complete replacement make any
                // partial dirty-set state claimed with this request obsolete.
                // Only now is this resynchronization considered complete.
                completed_resync = resync_target;
                failed_resync = 0;
            } else if (!dirty_ids.empty()) {
                if (before_trace_snapshot_hook_) {
                    before_trace_snapshot_hook_(
                        before_trace_snapshot_context_);
                }
                const auto traces = active_traces_.SnapshotTraces(dirty_ids);
                graph_.UpdateTraces(traces);
            }

            PublishTopology(
                graph_.Snapshot(), active_traces_.active_trace_count() > 0);
            if (after_topology_update_hook_) {
                after_topology_update_hook_(after_topology_update_context_);
            }
        } catch (const std::exception& error) {
            if (should_resync) {
                failed_resync = resync_target;
            } else {
                resync_requests_.fetch_add(1, std::memory_order_relaxed);
            }
            Report("live topology update failed", error.what());
        } catch (...) {
            if (should_resync) {
                failed_resync = resync_target;
            } else {
                resync_requests_.fetch_add(1, std::memory_order_relaxed);
            }
            Report("live topology update failed with an unknown error");
        }

        if (should_resync && failed_resync == resync_target) {
            std::scoped_lock lock{dirty_mutex_};
            if (invalidation_input_closed_) {
                break;
            }
        }
    }

    {
        std::scoped_lock lock{topology_mutex_};
        topology_input_closed_ = true;
    }
    topology_cv_.notify_one();
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
        std::scoped_lock lock{diagnostics_mutex_};
        diagnostics_ << "Silhouette live map: " << message;
        if (!detail.empty()) {
            diagnostics_ << ": " << detail;
        }
        diagnostics_ << '\n';
    } catch (...) {
    }
}

} // namespace silhouette
