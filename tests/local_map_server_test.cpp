#include "silhouette/local_map_server.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "local_map_server_test_peer.h"

#include <httplib.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using silhouette::ActiveTraceManager;
using silhouette::LiveServiceMap;
using silhouette::LocalMapServer;
using silhouette::ServiceGraph;
using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;
using silhouette::rendering::ServiceGraphRenderer;
using silhouette::rendering::SvgRenderResult;
using silhouette::testing::LocalMapServerTestPeer;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

TraceId MakeTraceId(const std::uint8_t marker)
{
    TraceId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    return *TraceId::FromBytes(bytes);
}

SpanId MakeSpanId(const std::uint8_t marker)
{
    SpanId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    return *SpanId::FromBytes(bytes);
}

Span MakeSpan(
    const std::uint8_t span_marker,
    std::string service,
    const std::optional<std::uint8_t> parent_marker = std::nullopt)
{
    return Span{
        .trace_id = MakeTraceId(1),
        .span_id = MakeSpanId(span_marker),
        .parent_span_id = parent_marker
            ? std::optional<SpanId>{MakeSpanId(*parent_marker)}
            : std::nullopt,
        .service_name = std::move(service),
        .operation_name = "operation",
        .start_time_unix_nano = span_marker,
        .end_time_unix_nano = static_cast<std::uint64_t>(span_marker) + 1,
    };
}

class SvgRenderer final : public ServiceGraphRenderer {
public:
    SvgRenderResult RenderSvg(const ServiceGraph& graph) override
    {
        return {.svg = "<svg data-services=\""
                    + std::to_string(graph.services.size()) + "\"></svg>",
                .error = {}};
    }
};

httplib::Client MakeClient(const int port)
{
    httplib::Client client{"127.0.0.1", port};
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    return client;
}

void ServesViewerAndLatestOwnedSvg()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial SVG was not ready for HTTP serving");
    server.Start();

    auto client = MakeClient(server.selected_port());
    const auto viewer = client.Get("/");
    Require(viewer && viewer->status == 200,
            "viewer route did not return HTTP 200");
    Require(viewer->body.find("EventSource('/events')") != std::string::npos
                && viewer->body.find("Waiting for telemetry")
                    != std::string::npos,
            "viewer omitted the thin SSE client or empty-state UI");
    Require(viewer->get_header_value("Cache-Control") == "no-store",
            "viewer route allowed stale caching");

    const auto graph = client.Get("/graph.svg?v=0");
    Require(graph && graph->status == 200 && graph->body == initial->svg,
            "graph route did not return the latest owned SVG bytes");
    Require(graph->get_header_value("Content-Type").find("image/svg+xml")
                != std::string::npos,
            "graph route returned the wrong content type");
    Require(graph->get_header_value("Cache-Control") == "no-store",
            "graph route allowed stale caching");

    live_map.CloseAndDrain();
    server.Shutdown();
    server.Shutdown();
    bool rejected = false;
    try {
        server.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Require(rejected, "stopped HTTP server restarted");
}

void SsePublishesRenderedVersionAndServerShutdownWakesWaiter()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial SVG was not published");
    server.Start();

    std::string received_events;
    std::mutex received_mutex;
    std::latch received_version_one{1};
    std::latch received_heartbeat_after_version_one{1};
    bool request_finished = false;
    std::thread event_client{[&] {
        auto client = MakeClient(server.selected_port());
        bool version_signaled = false;
        bool heartbeat_signaled = false;
        const auto result = client.Get(
            "/events", [&](const char* data, const std::size_t size) {
                std::scoped_lock lock{received_mutex};
                received_events.append(data, size);
                const auto version_position = received_events.find("id: 1\n");
                if (!version_signaled && version_position != std::string::npos) {
                    version_signaled = true;
                    received_version_one.count_down();
                }
                if (version_signaled && !heartbeat_signaled
                    && received_events.find(": keep-alive\n\n", version_position)
                        != std::string::npos) {
                    heartbeat_signaled = true;
                    received_heartbeat_after_version_one.count_down();
                }
                return true;
            });
        request_finished = result && result->status == 200;
    }};

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, "frontend"),
        MakeSpan(2, "checkout", 1),
    }).affected_trace_ids);
    received_version_one.wait();
    // Exercise another callback after the version latch reaches zero.
    received_heartbeat_after_version_one.wait();

    // Deliberately stop HTTP first. Its stop token must release the provider
    // even though the live-map publisher is still running.
    server.Shutdown();
    event_client.join();
    live_map.CloseAndDrain();

    Require(request_finished, "SSE request did not complete cleanly");
    Require(received_events.find("event: graph-updated\n")
                != std::string::npos
                && received_events.find("\"version\":1")
                    != std::string::npos
                && received_events.find("\"hasTelemetry\":true")
                    != std::string::npos,
            "SSE did not publish the rendered topology version");
}

void SseHeartbeatWakesIdleStreamAndDetectsDisconnect()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    std::latch stream_released{1};
    std::atomic<bool> release_signaled{false};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    LocalMapServerTestPeer::UseTwoWorkers(server);
    LocalMapServerTestPeer::AfterSseRelease(server, [&] {
        if (!release_signaled.exchange(true)) {
            stream_released.count_down();
        }
    });
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial SVG was not published");
    server.Start();

    std::latch received_heartbeat{1};
    std::thread event_client{[&] {
        auto client = MakeClient(server.selected_port());
        std::string received;
        static_cast<void>(client.Get(
            "/events", [&](const char* data, const std::size_t size) {
                received.append(data, size);
                if (received.find(": keep-alive\n\n") != std::string::npos) {
                    received_heartbeat.count_down();
                    return false;
                }
                return true;
            }));
    }};

    received_heartbeat.wait();
    event_client.join();
    stream_released.wait();

    auto client = MakeClient(server.selected_port());
    const auto viewer = client.Get("/");
    int reopened_status = 0;
    std::string reopened_events;
    static_cast<void>(client.Get(
        "/events",
        [&](const httplib::Response& response) {
            reopened_status = response.status;
            return true;
        },
        [&](const char* data, const std::size_t size) {
            reopened_events.append(data, size);
            return reopened_events.find("id: 0\n") == std::string::npos;
        }));

    server.Shutdown();
    live_map.CloseAndDrain();
    Require(viewer && viewer->status == 200,
            "disconnected idle SSE stream prevented viewer requests");
    Require(reopened_status == 200
                && reopened_events.find("id: 0\n") != std::string::npos,
            "disconnected SSE stream did not release its admission slot");
}

void SseCapacityKeepsViewerAndGraphRoutesAvailable()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    // One persistent SSE stream fills the quota, leaving one HTTP worker.
    LocalMapServerTestPeer::UseTwoWorkers(server);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial SVG was not published");
    server.Start();

    std::latch stream_open{1};
    std::latch updated_event{1};
    bool stream_finished = false;
    std::thread event_client{[&] {
        auto client = MakeClient(server.selected_port());
        std::string received;
        bool open_signaled = false;
        bool update_signaled = false;
        const auto result = client.Get(
            "/events", [&](const char* data, const std::size_t size) {
                received.append(data, size);
                if (!open_signaled && received.find("id: 0\n") != std::string::npos) {
                    open_signaled = true;
                    stream_open.count_down();
                }
                if (!update_signaled && received.find("id: 1\n") != std::string::npos) {
                    update_signaled = true;
                    updated_event.count_down();
                }
                return true;
            });
        stream_finished = result && result->status == 200;
    }};
    stream_open.wait();
    auto client = MakeClient(server.selected_port());
    const auto excess = client.Get("/events");
    const auto viewer = client.Get("/");
    const auto initial_graph = client.Get("/graph.svg");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, "frontend"), MakeSpan(2, "checkout", 1),
    }).affected_trace_ids);
    const auto updated = live_map.WaitForPublishedAfter(initial->rendered_version);
    updated_event.wait();
    const auto updated_graph = client.Get("/graph.svg?v=1");
    server.Shutdown();
    event_client.join();
    live_map.CloseAndDrain();

    Require(excess && excess->status == 503
                && excess->get_header_value("Connection") == "close"
                && excess->body.find("capacity is full") != std::string::npos,
            "excess SSE stream was not rejected with a closed busy response");
    Require(viewer && viewer->status == 200
                && initial_graph && initial_graph->status == 200
                && initial_graph->body == initial->svg,
            "SSE capacity blocked viewer or initial SVG requests");
    Require(updated && updated->rendered_version == 1
                && updated_graph && updated_graph->status == 200
                && updated_graph->body == updated->svg,
            "SSE capacity prevented fetching an updated graph");
    Require(stream_finished, "quota-full SSE stream did not shut down cleanly");
}

void WorkerStartupFailureIsReportedAndReleasesPort()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer failed{"127.0.0.1", 0, live_map};
    LocalMapServerTestPeer::FailWorkerStartup(failed);
    bool reported = false;
    try {
        failed.Start();
    } catch (const std::exception& error) {
        reported = std::string_view{error.what()}.find(
            "injected HTTP worker startup failure") != std::string_view::npos;
    }
    Require(reported, "HTTP worker failure did not reach Start's caller");
    failed.Shutdown();
    failed.Shutdown();
    Require(failed.selected_port() > 0,
            "failure injection did not run after the socket was bound");

    LocalMapServer replacement{"127.0.0.1", failed.selected_port(), live_map};
    replacement.Start();
    auto client = MakeClient(replacement.selected_port());
    const auto response = client.Get("/");
    Require(response && response->status == 200,
            "worker startup failure leaked the bound socket or listener");
    replacement.Shutdown();
}

void EnqueueFailureRejectsOnlyThatRequest()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    std::size_t attempts = 0;
    LocalMapServerTestPeer::BeforeEnqueue(server, [&] {
        if (++attempts == 1) {
            throw std::bad_alloc{};
        }
    });
    server.Start();

    auto rejected_client = MakeClient(server.selected_port());
    const auto rejected = rejected_client.Get("/");
    auto next_client = MakeClient(server.selected_port());
    const auto next = next_client.Get("/");
    server.Shutdown();
    server.Shutdown();

    Require(!rejected, "failed enqueue did not close the rejected connection");
    Require(next && next->status == 200,
            "enqueue failure stopped the HTTP listener or lost the next request");
    Require(attempts == 2, "enqueue failure retried or duplicated a request");

    LocalMapServer replacement{"127.0.0.1", server.selected_port(), live_map};
    replacement.Start();
    auto client = MakeClient(replacement.selected_port());
    const auto response = client.Get("/");
    replacement.Shutdown();
    Require(response && response->status == 200,
            "enqueue failure leaked the bound socket or worker pool");
}

void EnqueueFailureWhileSseWorkerBusyShutsDownCleanly()
{
    ActiveTraceManager active_traces;
    SvgRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, live_map};
    LocalMapServerTestPeer::UseTwoWorkers(server);
    std::size_t attempts = 0;
    LocalMapServerTestPeer::BeforeEnqueue(server, [&] {
        if (++attempts == 2) {
            throw std::system_error{
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "injected HTTP enqueue failure"};
        }
    });
    live_map.Start();
    Require(live_map.WaitForPublishedAfter(std::nullopt).has_value(),
            "initial SVG was not published");
    server.Start();

    std::latch stream_open{1};
    bool stream_finished = false;
    std::thread event_client{[&] {
        auto client = MakeClient(server.selected_port());
        std::string received;
        bool signaled = false;
        const auto result = client.Get(
            "/events", [&](const char* data, const std::size_t size) {
                received.append(data, size);
                if (!signaled && received.find("id: 0\n") != std::string::npos) {
                    signaled = true;
                    stream_open.count_down();
                }
                return true;
            });
        stream_finished = result && result->status == 200;
    }};
    // Establish that the SSE worker is busy before admission fails for another
    // request; the second worker remains reserved for ordinary HTTP requests.
    stream_open.wait();
    auto client = MakeClient(server.selected_port());
    const auto rejected = client.Get("/");
    server.Shutdown();
    event_client.join();
    live_map.CloseAndDrain();

    Require(!rejected, "busy-pool enqueue failure did not reject the request");
    Require(stream_finished, "enqueue failure prevented clean SSE shutdown");
    Require(attempts == 2, "busy-pool request was duplicated or retried");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"sse_capacity_keeps_viewer_and_graph_routes_available",
             SseCapacityKeepsViewerAndGraphRoutesAvailable},
    TestCase{"enqueue_failure_rejects_only_that_request",
             EnqueueFailureRejectsOnlyThatRequest},
    TestCase{"enqueue_failure_while_sse_worker_busy_shuts_down_cleanly",
             EnqueueFailureWhileSseWorkerBusyShutsDownCleanly},
    TestCase{"worker_startup_failure_is_reported_and_releases_port",
             WorkerStartupFailureIsReportedAndReleasesPort},
    TestCase{"serves_viewer_and_latest_owned_svg",
             ServesViewerAndLatestOwnedSvg},
    TestCase{"sse_publishes_rendered_version_and_server_shutdown_wakes_waiter",
             SsePublishesRenderedVersionAndServerShutdownWakesWaiter},
    TestCase{"sse_heartbeat_wakes_idle_stream_and_detects_disconnect",
             SseHeartbeatWakesIdleStreamAndDetectsDisconnect},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one local-map-server test-case name.\n";
        return 1;
    }
    const std::string_view requested_case{argv[1]};
    for (const auto& test_case : kTestCases) {
        if (test_case.name != requested_case) {
            continue;
        }
        try {
            test_case.run();
            std::cout << test_case.name << " passed.\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << test_case.name << " failed: " << error.what() << '\n';
            return 1;
        }
    }
    std::cerr << "Unknown local-map-server test case: " << requested_case << '\n';
    return 1;
}
