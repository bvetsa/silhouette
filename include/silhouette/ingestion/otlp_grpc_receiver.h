#pragma once

#include "silhouette/active_trace_manager.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace grpc {
class Server;
}

namespace silhouette::ingestion {

class OtlpGrpcReceiver final {
public:
    // active_traces must outlive this receiver and receives each accepted batch.
    OtlpGrpcReceiver(
        std::string listen_address, ActiveTraceManager& active_traces);
    ~OtlpGrpcReceiver();

    OtlpGrpcReceiver(const OtlpGrpcReceiver&) = delete;
    OtlpGrpcReceiver& operator=(const OtlpGrpcReceiver&) = delete;
    OtlpGrpcReceiver(OtlpGrpcReceiver&&) = delete;
    OtlpGrpcReceiver& operator=(OtlpGrpcReceiver&&) = delete;

    // Starts the gRPC server and returns after it is ready to receive requests.
    void Start();

    // Stops a started server, waits for in-flight work, and is safe to call repeatedly.
    void Shutdown() noexcept;

    [[nodiscard]] int selected_port() const noexcept;
    [[nodiscard]] std::uint64_t accepted_request_count() const noexcept;
    [[nodiscard]] std::uint64_t accepted_span_count() const noexcept;

private:
    class TraceService;

    enum class State {
        not_started,
        running,
        stopped,
    };

    std::string listen_address_;
    ActiveTraceManager& active_traces_;
    std::unique_ptr<TraceService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::atomic<std::uint64_t> accepted_request_count_{0};
    std::atomic<std::uint64_t> accepted_span_count_{0};
    int selected_port_{0};
    State state_{State::not_started};
};

} // namespace silhouette::ingestion
