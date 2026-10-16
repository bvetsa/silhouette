#include "silhouette/active_trace_manager.h"
#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/live_service_map.h"
#include "silhouette/local_map_server.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "silhouette/service_graph.h"
#include "silhouette/trace_reconstruction.h"
#include "rendering/service_graph_artifacts.h"
#include "application_shutdown_guard.h"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

namespace {

constexpr auto kListenAddress = "127.0.0.1:4317";
constexpr auto kMapListenAddress = "127.0.0.1";
constexpr int kMapPort = 8008;
constexpr auto kTraceQuietInterval = std::chrono::seconds{30};
volatile std::sig_atomic_t shutdown_signal = 0;

extern "C" void HandleShutdownSignal(const int signal)
{
    shutdown_signal = signal;
}

bool InstallSignalHandlers()
{
    return std::signal(SIGINT, HandleShutdownSignal) != SIG_ERR
        && std::signal(SIGTERM, HandleShutdownSignal) != SIG_ERR;
}

} // namespace

int main()
{
    if (!InstallSignalHandlers()) {
        std::cerr << "Silhouette failed to install shutdown signal handlers.\n";
        return 1;
    }

    try {
        silhouette::ActiveTraceManager active_traces{kTraceQuietInterval};
        silhouette::rendering::GraphvizServiceGraphRenderer live_renderer;
        silhouette::ObservabilityQueryService queries{active_traces, std::cerr};
        silhouette::LiveServiceMap live_map{
            queries, live_renderer, std::cerr};
        silhouette::LocalMapServer map_server{
            kMapListenAddress, kMapPort, live_map, queries};
        silhouette::ingestion::OtlpGrpcReceiver receiver{
            kListenAddress, active_traces, &queries};
        silhouette::ApplicationShutdownGuard shutdown{
            receiver, active_traces, queries, live_map, map_server};

        queries.Start();
        active_traces.StartLifecycle(&queries, &queries);
        live_map.Start();
        bool map_server_started = false;
        try {
            map_server.Start();
            map_server_started = true;
        } catch (const std::exception& error) {
            std::cerr << "Silhouette live map server failed to start: "
                      << error.what() << ". OTLP ingestion will continue.\n";
        }
        receiver.Start();

        std::cout << "Silhouette is listening for OTLP/gRPC traces on "
                  << kListenAddress << ".\n";
        if (map_server_started) {
            std::cout << "Service map available at http://"
                      << kMapListenAddress << ':' << kMapPort << ".\n";
        }
        std::cout << "Press Ctrl-C to stop." << std::endl;

        std::uint64_t reported_span_count = 0;
        silhouette::ActiveTraceManager::LifecycleCounts reported_lifecycle_counts;
        while (shutdown_signal == 0) {
            const auto accepted_span_count = receiver.accepted_span_count();
            const auto counts = active_traces.lifecycle_counts();
            if (accepted_span_count != reported_span_count
                || counts != reported_lifecycle_counts) {
                std::cout << "Live state: "
                          << counts.active << " active trace(s), "
                          << counts.quiescent << " quiescent trace(s), "
                          << accepted_span_count
                          << " accepted span(s)." << std::endl;
                reported_span_count = accepted_span_count;
                reported_lifecycle_counts = counts;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        shutdown.Shutdown();
        const auto traces = active_traces.Snapshot();
        const auto service_graph = silhouette::BuildServiceGraph(traces);

        std::cout << "Silhouette stopped after accepting "
                  << receiver.accepted_request_count() << " export request(s) containing "
                  << receiver.accepted_span_count() << " span(s).\n"
                  << silhouette::FormatReconstructedTraces(traces) << '\n'
                  << silhouette::FormatServiceGraph(service_graph);

        const auto artifacts =
            silhouette::rendering::WriteServiceGraphArtifacts(service_graph);
        std::cout << "Wrote service graph DOT to "
                  << silhouette::rendering::kServiceGraphDotFilename << ".\n";
        if (!artifacts.svg_rendered) {
            std::cerr << "Capture, reconstruction, aggregation, and DOT succeeded, "
                         "but SVG rendering failed: "
                      << artifacts.render_error << ". DOT retained at "
                      << silhouette::rendering::kServiceGraphDotFilename << ".\n";
            return 1;
        }
        std::cout << "Rendered service graph SVG to "
                  << silhouette::rendering::kServiceGraphSvgFilename << ".\n";
    } catch (const std::exception& error) {
        std::cerr << "Silhouette failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
