#include "silhouette/local_map_server.h"
#include "silhouette/rendering/topology_fact_key.h"
#include "presentation/observability_json.h"
#include "viewer_assets.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace silhouette {
namespace {

constexpr auto kSseHeartbeatInterval = std::chrono::seconds{1};
constexpr std::string_view kSseHeartbeat = ": keep-alive\n\n";

void ReportHttpFailure(
    const std::string_view operation, const std::string_view detail) noexcept
{
    try {
        std::cerr << "Silhouette live map HTTP " << operation << ": "
                  << detail << '\n';
    } catch (...) {
    }
}

// Known-length providers avoid cpp-httplib's expensive default Brotli pass
// over every large live query response, without changing its media type.
void SetQueryContent(httplib::Response& response, std::string body)
{
    auto payload = std::make_shared<const std::string>(std::move(body));
    response.set_content_provider(payload->size(), "application/json",
        [payload](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            if (offset > payload->size() || length > payload->size() - offset) return false;
            return sink.write(payload->data() + offset, length);
        });
}

class SseSlot final {
public:
    SseSlot(std::atomic<std::size_t>& active, const std::size_t limit,
            std::function<void()> after_release)
        : active_{active}, after_release_{std::move(after_release)}
    {
        auto count = active_.load();
        while (count < limit) {
            if (active_.compare_exchange_weak(count, count + 1)) {
                acquired_ = true;
                break;
            }
        }
    }

    SseSlot(const SseSlot&) = delete;
    SseSlot& operator=(const SseSlot&) = delete;

    ~SseSlot()
    {
        if (acquired_) {
            active_.fetch_sub(1);
            if (after_release_) {
                try {
                    after_release_();
                } catch (...) {
                    ReportHttpFailure("SSE cleanup hook failed", "test hook threw");
                }
            }
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    std::atomic<std::size_t>& active_;
    std::function<void()> after_release_;
    bool acquired_{false};
};

class HttpTaskQueue final : public httplib::TaskQueue {
public:
    HttpTaskQueue(httplib::Server& server, const std::size_t workers,
                  std::stop_source stop_source,
                  std::function<void()> before_enqueue)
        : server_{server}
        , stop_source_{std::move(stop_source)}
        , before_enqueue_{std::move(before_enqueue)}
        , pool_{workers, workers}
    {
    }

    ~HttpTaskQueue() override { shutdown(); }

    bool enqueue(std::function<void()> task) override
    {
        try {
            if (before_enqueue_) {
                before_enqueue_();
            }
            // Keep the pool fixed-size: cpp-httplib's dynamic expansion can
            // throw AFTER accepting a job (or destroy a joinable temporary
            // thread). With no expansion, enqueue's allocating list insertion
            // either succeeds completely or throws before accepting the job.
            return pool_.enqueue(std::move(task));
        } catch (const std::exception& error) {
            ReportHttpFailure("request rejected", error.what());
        } catch (...) {
            ReportHttpFailure("request rejected", "unknown queue error");
        }
        // The job was not accepted; cpp-httplib still owns and closes its socket.
        return false;
    }

    void shutdown() override
    {
        if (!stopped_) {
            // Also release SSE waiters if listen_internal() unwinds before our
            // outer listener catch. Its queue destruction must join workers.
            stop_source_.request_stop();
            server_.stop();
            pool_.shutdown();
            stopped_ = true;
        }
    }

private:
    httplib::Server& server_;
    std::stop_source stop_source_;
    std::function<void()> before_enqueue_;
    httplib::ThreadPool pool_;
    bool stopped_{false};
};

} // namespace

class LocalMapServer::Impl final {
public:
    Impl(std::string listen_address, const int port,
         const LiveServiceMap& live_map, const ObservabilityQueryService& queries)
        : listen_address_{std::move(listen_address)}
        , requested_port_{port}
        , live_map_{live_map}
        , queries_{queries}
    {
        if (listen_address_.empty()) {
            throw std::invalid_argument{"live map listen address cannot be empty"};
        }
        if (requested_port_ < 0 || requested_port_ > 65535) {
            throw std::invalid_argument{"live map port is out of range"};
        }

        server_.new_task_queue = [this] {
            return new HttpTaskQueue{
                server_, worker_count_, stop_source_, before_enqueue_hook_};
        };

        server_.Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string{presentation::kViewerHtml}, "text/html; charset=utf-8");
            response.set_header("Cache-Control", "no-store");
        });
        server_.Get("/viewer.js", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string{presentation::kViewerJs}, "text/javascript; charset=utf-8");
            response.set_header("Cache-Control", "no-store");
        });
        server_.Get("/viewer.css", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string{presentation::kViewerCss}, "text/css; charset=utf-8");
            response.set_header("Cache-Control", "no-store");
        });
        server_.Get("/graph.svg", [this](const httplib::Request&,
                                          httplib::Response& response) {
            const auto published = live_map_.SnapshotPublished();
            response.set_header("Cache-Control", "no-store");
            if (!published) {
                response.status = 503;
                response.set_content(
                    "Live graph rendering is not currently available.\n",
                    "text/plain; charset=utf-8");
                return;
            }
            response.set_header("X-Silhouette-Rendered-Version", std::to_string(published->rendered_version));
            response.set_header("X-Silhouette-Observation-Instance", queries_.observation_instance_id().value);
            response.set_content(published->svg, "image/svg+xml");
        });
        server_.Post("/query/support", [this](const httplib::Request& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            try {
                std::optional<TopologyFact> fact;
                std::optional<SupportCursor> cursor;
                try {
                    const auto input = nlohmann::json::parse(request.body);
                    fact = rendering::DecodeTopologyFact(input.at("factKey").get<std::string>());
                    if (!fact) throw std::invalid_argument{"invalid topology fact key"};
                    cursor = presentation::ParseCursor(input.value("cursor", nlohmann::json(nullptr)));
                    if (cursor && cursor->instance != queries_.observation_instance_id())
                        throw std::invalid_argument{"cursor belongs to another observation instance"};
                } catch (const nlohmann::json::exception&) {
                    response.status = 400; response.set_content("Malformed support query.\n", "text/plain"); return;
                } catch (const std::invalid_argument& error) {
                    response.status = 400; response.set_content(error.what(), "text/plain"); return;
                }
                const auto page = queries_.QuerySupport(*fact, cursor);
                auto output = presentation::ToJson(page);
                output["factLabel"] = presentation::DescribeFact(*fact);
                SetQueryContent(response, output.dump());
            } catch (const std::exception& error) {
                ReportHttpFailure("support query failed", error.what());
                response.status = 500; response.set_content("Support query failed.\n", "text/plain");
            }
        });
        server_.Get(R"(/query/trace/([^/]+)/([^/]+))", [this](const httplib::Request& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            try {
                std::optional<TraceRecordKey> key;
                try {
                    key = presentation::ParseRecordKey(request.matches[1].str(), request.matches[2].str());
                    if (!request.has_param("instance")) throw std::invalid_argument{"missing observation instance"};
                } catch (const std::invalid_argument& error) {
                    response.status = 400; response.set_content(error.what(), "text/plain"); return;
                }
                const auto detail = request.get_param_value("instance") == queries_.observation_instance_id().value
                    ? queries_.QueryTrace(*key) : std::nullopt;
                if (!detail) {
                    response.status = 404; response.set_content("Trace record is no longer retained.\n", "text/plain"); return;
                }
                SetQueryContent(response, presentation::ToJson(*detail).dump());
            } catch (const std::exception& error) {
                ReportHttpFailure("trace query failed", error.what());
                response.status = 500; response.set_content("Trace query failed.\n", "text/plain");
            }
        });
        server_.Get("/events", [this](const httplib::Request&,
                                      httplib::Response& response) {
            // Persistent SSE responses must leave a worker available for the
            // page and graph fetches they trigger. Acquire before installing the
            // provider; RAII also releases admission if provider setup throws.
            auto slot = std::make_shared<SseSlot>(
                active_sse_, worker_count_ - 1, after_sse_release_hook_);
            response.set_header("Connection", "close");
            response.set_header("Cache-Control", "no-store");
            if (!slot->acquired()) {
                response.status = 503;
                response.set_content(
                    "Live map event-stream capacity is full. Try again later.\n",
                    "text/plain; charset=utf-8");
                return;
            }
            const auto stop_token = stop_source_.get_token();
            response.set_header("X-Accel-Buffering", "no");
            response.set_chunked_content_provider(
                "text/event-stream",
                [this, stop_token,
                 version = std::optional<std::uint64_t>{},
                 observation = std::optional<std::uint64_t>{}]
                (std::size_t, httplib::DataSink& sink) mutable {
                    if (!sink.is_writable()) {
                        sink.done();
                        return false;
                    }
                    const auto state = live_map_.WaitForVersionsAfter(
                        version, observation, kSseHeartbeatInterval, stop_token);
                    if (stop_token.stop_requested() || state.stopped || !sink.is_writable()) {
                        sink.done(); return false;
                    }
                    std::string event;
                    const auto& instance = queries_.observation_instance_id().value;
                    if (state.rendered_version && (!version || *state.rendered_version > *version)) {
                        event = "event: graph-updated\nid: " + std::to_string(*state.rendered_version)
                            + "\ndata: " + nlohmann::json{
                                {"version", *state.rendered_version},
                                {"renderedVersion", std::to_string(*state.rendered_version)},
                                {"instance", instance}, {"hasTelemetry", state.has_telemetry}}.dump() + "\n\n";
                        version = state.rendered_version;
                    }
                    if (!observation || state.observation_version > *observation) {
                        event += "event: observations-updated\ndata: " + nlohmann::json{
                            {"instance", instance},
                            {"observationVersion", std::to_string(state.observation_version)}}.dump() + "\n\n";
                        observation = state.observation_version;
                    }
                    if (event.empty()) return sink.write(kSseHeartbeat.data(), kSseHeartbeat.size());
                    return sink.write(event.data(), event.size());
                },
                [slot = std::move(slot)](bool) mutable { slot.reset(); });
        });
    }

    ~Impl() { Shutdown(); }

    void Start()
    {
        if (started_once_) {
            throw std::logic_error{"live map server can only be started once"};
        }
        if (requested_port_ == 0) {
            selected_port_ = server_.bind_to_any_port(listen_address_);
            if (selected_port_ <= 0) {
                throw std::runtime_error{
                    "could not bind live map server to " + listen_address_};
            }
        } else {
            if (!server_.bind_to_port(listen_address_, requested_port_)) {
                throw std::runtime_error{"could not bind live map server to "
                    + listen_address_ + ':' + std::to_string(requested_port_)};
            }
            selected_port_ = requested_port_;
        }
        started_once_ = true;
        started_ = true;
        try {
            auto ready = startup_result_.get_future();
            auto make_queue = std::move(server_.new_task_queue);
            server_.new_task_queue = [this, make_queue = std::move(make_queue)] {
                std::unique_ptr<httplib::TaskQueue> queue{make_queue()};
                // is_running() becomes true before pool construction, so it
                // cannot establish successful startup. Publish readiness here.
                startup_result_.set_value();
                startup_signaled_ = true;
                return queue.release();
            };
            thread_ = std::thread{[this] {
                try {
                    if (!server_.listen_after_bind()) {
                        throw std::runtime_error{"HTTP listen loop failed"};
                    }
                } catch (...) {
                    server_.stop();
                    if (!startup_signaled_) {
                        startup_result_.set_exception(std::current_exception());
                    } else {
                        try {
                            throw;
                        } catch (const std::exception& error) {
                            ReportHttpFailure("server stopped", error.what());
                        } catch (...) {
                            ReportHttpFailure(
                                "server stopped", "unknown listener error");
                        }
                    }
                }
            }};
            ready.get();
        } catch (const std::exception&) {
            Shutdown();
            throw;
        } catch (...) {
            Shutdown();
            throw std::runtime_error{"unknown HTTP worker startup failure"};
        }
    }

    void Shutdown() noexcept
    {
        if (!started_) {
            return;
        }
        stop_source_.request_stop();
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        started_ = false;
    }

    void UseTwoWorkersForTesting()
    {
        if (started_once_) {
            throw std::logic_error{
                "live map worker count cannot change after startup"};
        }
        worker_count_ = 2;
    }

    void FailWorkerStartupForTesting()
    {
        if (started_once_) {
            throw std::logic_error{
                "live map worker factory cannot change after startup"};
        }
        server_.new_task_queue = []() -> httplib::TaskQueue* {
            throw std::system_error{
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "injected HTTP worker startup failure"};
        };
    }

    void SetBeforeEnqueueHookForTesting(std::function<void()> hook)
    {
        if (started_once_) {
            throw std::logic_error{
                "live map enqueue hook cannot change after startup"};
        }
        before_enqueue_hook_ = std::move(hook);
    }

    void SetAfterSseReleaseHookForTesting(std::function<void()> hook)
    {
        if (started_once_) {
            throw std::logic_error{
                "live map SSE release hook cannot change after startup"};
        }
        after_sse_release_hook_ = std::move(hook);
    }

    [[nodiscard]] int selected_port() const noexcept { return selected_port_; }

private:
    std::string listen_address_;
    int requested_port_;
    const LiveServiceMap& live_map_;
    const ObservabilityQueryService& queries_;
    httplib::Server server_;
    std::stop_source stop_source_;
    std::size_t worker_count_{CPPHTTPLIB_THREAD_POOL_COUNT};
    std::function<void()> before_enqueue_hook_;
    std::atomic<std::size_t> active_sse_{0};
    std::function<void()> after_sse_release_hook_;
    std::thread thread_;
    std::promise<void> startup_result_;
    // Only the listener thread and its worker-factory callback access this flag.
    bool startup_signaled_{false};
    int selected_port_{0};
    bool started_once_{false};
    bool started_{false};
};

LocalMapServer::LocalMapServer(
    std::string listen_address, const int port,
    const LiveServiceMap& live_map, const ObservabilityQueryService& queries)
    : impl_{std::make_unique<Impl>(
          std::move(listen_address), port, live_map, queries)}
{
}

LocalMapServer::~LocalMapServer() = default;

void LocalMapServer::Start() { impl_->Start(); }
void LocalMapServer::Shutdown() noexcept { impl_->Shutdown(); }
int LocalMapServer::selected_port() const noexcept { return impl_->selected_port(); }
void LocalMapServer::UseTwoWorkersForTesting()
{
    impl_->UseTwoWorkersForTesting();
}
void LocalMapServer::FailWorkerStartupForTesting()
{
    impl_->FailWorkerStartupForTesting();
}
void LocalMapServer::SetBeforeEnqueueHookForTesting(std::function<void()> hook)
{
    impl_->SetBeforeEnqueueHookForTesting(std::move(hook));
}
void LocalMapServer::SetAfterSseReleaseHookForTesting(std::function<void()> hook)
{
    impl_->SetAfterSseReleaseHookForTesting(std::move(hook));
}

} // namespace silhouette
