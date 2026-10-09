#include "silhouette/local_map_server.h"
#include "silhouette/rendering/service_graph_renderer.h"

#include <httplib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    bool request_finished = false;
    std::thread event_client{[&] {
        auto client = MakeClient(server.selected_port());
        const auto result = client.Get(
            "/events", [&](const char* data, const std::size_t size) {
                std::scoped_lock lock{received_mutex};
                received_events.append(data, size);
                if (received_events.find("id: 1\n") != std::string::npos) {
                    received_version_one.count_down();
                }
                return true;
            });
        request_finished = result && result->status == 200;
    }};

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, "frontend"),
        MakeSpan(2, "checkout", 1),
    }));
    received_version_one.wait();

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

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"serves_viewer_and_latest_owned_svg",
             ServesViewerAndLatestOwnedSvg},
    TestCase{"sse_publishes_rendered_version_and_server_shutdown_wakes_waiter",
             SsePublishesRenderedVersionAndServerShutdownWakesWaiter},
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
