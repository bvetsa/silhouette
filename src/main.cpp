#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/service_graph.h"
#include "silhouette/span_capture.h"
#include "silhouette/trace_reconstruction.h"
#include "rendering/service_graph_artifacts.h"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

namespace {

constexpr auto kListenAddress = "127.0.0.1:4317";
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
        silhouette::SpanCapture capture;
        silhouette::ingestion::OtlpGrpcReceiver receiver{kListenAddress, capture};
        receiver.Start();

        std::cout << "Silhouette is listening for OTLP/gRPC traces on "
                  << kListenAddress << ". Press Ctrl-C to stop." << std::endl;

        while (shutdown_signal == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        receiver.Shutdown();
        const auto traces = silhouette::ReconstructTraces(capture.Snapshot());
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
