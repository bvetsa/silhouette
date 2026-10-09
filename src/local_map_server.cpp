#include "silhouette/local_map_server.h"

#include <httplib.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace silhouette {
namespace {

constexpr auto kViewerHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Silhouette</title>
  <style>
    :root { color-scheme: light; font-family: system-ui, sans-serif; }
    body { margin: 0; background: #f8fafc; color: #0f172a; }
    header { display: flex; justify-content: space-between; padding: 1rem 1.5rem;
             background: white; border-bottom: 1px solid #e2e8f0; }
    #status { color: #64748b; font-size: .85rem; letter-spacing: .08em; }
    main { min-height: calc(100vh - 4rem); display: grid; place-items: center; padding: 1.5rem; }
    #waiting { color: #64748b; }
    #graph { max-width: 100%; height: auto; display: none; }
  </style>
</head>
<body>
  <header><strong>Silhouette</strong><span id="status">CONNECTING</span></header>
  <main><p id="waiting">Waiting for telemetry...</p><img id="graph" alt="Live service map"></main>
  <script>
    const status = document.getElementById('status');
    const waiting = document.getElementById('waiting');
    const graph = document.getElementById('graph');
    const events = new EventSource('/events');
    events.onopen = () => { status.textContent = 'LIVE'; };
    events.onerror = () => { status.textContent = 'RECONNECTING'; };
    events.addEventListener('graph-updated', event => {
      const update = JSON.parse(event.data);
      if (update.hasTelemetry) {
        graph.src = '/graph.svg?v=' + update.version;
        graph.style.display = 'block';
        waiting.style.display = 'none';
      } else {
        graph.style.display = 'none';
        waiting.style.display = 'block';
      }
    });
  </script>
</body>
</html>)HTML";

} // namespace

class LocalMapServer::Impl final {
public:
    Impl(std::string listen_address, const int port,
         const LiveServiceMap& live_map)
        : listen_address_{std::move(listen_address)}
        , requested_port_{port}
        , live_map_{live_map}
    {
        if (listen_address_.empty()) {
            throw std::invalid_argument{"live map listen address cannot be empty"};
        }
        if (requested_port_ < 0 || requested_port_ > 65535) {
            throw std::invalid_argument{"live map port is out of range"};
        }

        server_.Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(kViewerHtml, "text/html; charset=utf-8");
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
            response.set_content(published->svg, "image/svg+xml");
        });
        server_.Get("/events", [this](const httplib::Request&,
                                      httplib::Response& response) {
            const auto stop_token = stop_source_.get_token();
            response.set_header("Cache-Control", "no-store");
            response.set_header("X-Accel-Buffering", "no");
            response.set_chunked_content_provider(
                "text/event-stream",
                [this, stop_token,
                 version = std::optional<std::uint64_t>{}]
                (std::size_t, httplib::DataSink& sink) mutable {
                    const auto published =
                        live_map_.WaitForPublishedAfter(version, stop_token);
                    if (!published) {
                        sink.done();
                        return false;
                    }
                    const auto event = "event: graph-updated\nid: "
                        + std::to_string(published->rendered_version)
                        + "\ndata: {\"version\":"
                        + std::to_string(published->rendered_version)
                        + ",\"hasTelemetry\":"
                        + (published->has_telemetry ? "true" : "false")
                        + "}\n\n";
                    version = published->rendered_version;
                    return sink.write(event.data(), event.size());
                });
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
        thread_ = std::thread{[this] { server_.listen_after_bind(); }};
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

    [[nodiscard]] int selected_port() const noexcept { return selected_port_; }

private:
    std::string listen_address_;
    int requested_port_;
    const LiveServiceMap& live_map_;
    httplib::Server server_;
    std::stop_source stop_source_;
    std::thread thread_;
    int selected_port_{0};
    bool started_once_{false};
    bool started_{false};
};

LocalMapServer::LocalMapServer(
    std::string listen_address, const int port,
    const LiveServiceMap& live_map)
    : impl_{std::make_unique<Impl>(
          std::move(listen_address), port, live_map)}
{
}

LocalMapServer::~LocalMapServer() = default;

void LocalMapServer::Start() { impl_->Start(); }
void LocalMapServer::Shutdown() noexcept { impl_->Shutdown(); }
int LocalMapServer::selected_port() const noexcept { return impl_->selected_port(); }

} // namespace silhouette
