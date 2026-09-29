#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/span_capture.h"
#include "silhouette/trace_reconstruction.h"

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

        std::cout << "Silhouette stopped after accepting "
                  << receiver.accepted_request_count() << " export request(s) containing "
                  << receiver.accepted_span_count() << " span(s).\n"
                  << silhouette::FormatReconstructedTraces(traces);
    } catch (const std::exception& error) {
        std::cerr << "Silhouette failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
