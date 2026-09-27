#include "silhouette/ingestion/otlp_grpc_receiver.h"

#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace silhouette::ingestion {

namespace otlp = opentelemetry::proto::collector::trace::v1;

class OtlpGrpcReceiver::TraceService final : public otlp::TraceService::Service {
public:
    TraceService(std::atomic<std::uint64_t>& accepted_request_count,
                 std::atomic<std::uint64_t>& accepted_span_count)
        : accepted_request_count_{accepted_request_count}
        , accepted_span_count_{accepted_span_count}
    {
    }

    grpc::Status Export(
        grpc::ServerContext*,
        const otlp::ExportTraceServiceRequest* request,
        otlp::ExportTraceServiceResponse*) override
    {
        std::uint64_t span_count = 0;

        for (const auto& resource_spans : request->resource_spans()) {
            for (const auto& scope_spans : resource_spans.scope_spans()) {
                span_count += static_cast<std::uint64_t>(scope_spans.spans_size());
            }
        }

        accepted_span_count_.fetch_add(span_count, std::memory_order_relaxed);
        accepted_request_count_.fetch_add(1, std::memory_order_relaxed);

        return grpc::Status::OK;
    }

private:
    std::atomic<std::uint64_t>& accepted_request_count_;
    std::atomic<std::uint64_t>& accepted_span_count_;
};

OtlpGrpcReceiver::OtlpGrpcReceiver(std::string listen_address)
    : listen_address_{std::move(listen_address)}
{
    if (listen_address_.empty()) {
        throw std::invalid_argument{"OTLP/gRPC listen address cannot be empty"};
    }
}

OtlpGrpcReceiver::~OtlpGrpcReceiver()
{
    Shutdown();
}

void OtlpGrpcReceiver::Start()
{
    if (state_ != State::not_started) {
        throw std::logic_error{"OTLP/gRPC receiver can only be started once"};
    }

    auto service = std::make_unique<TraceService>(
        accepted_request_count_, accepted_span_count_);

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(
        listen_address_, grpc::InsecureServerCredentials(), &selected_port);
    builder.RegisterService(service.get());

    auto server = builder.BuildAndStart();
    if (!server) {
        throw std::runtime_error{
            "could not bind OTLP/gRPC receiver to " + listen_address_};
    }
    if (selected_port == 0) {
        server->Shutdown();
        server->Wait();
        throw std::runtime_error{
            "OTLP/gRPC receiver started without a usable port for " + listen_address_};
    }

    service_ = std::move(service);
    server_ = std::move(server);
    selected_port_ = selected_port;
    state_ = State::running;
}

void OtlpGrpcReceiver::Shutdown() noexcept
{
    if (state_ != State::running) {
        return;
    }

    server_->Shutdown();
    server_->Wait();
    server_.reset();
    service_.reset();
    state_ = State::stopped;
}

int OtlpGrpcReceiver::selected_port() const noexcept
{
    return selected_port_;
}

std::uint64_t OtlpGrpcReceiver::accepted_request_count() const noexcept
{
    return accepted_request_count_.load(std::memory_order_relaxed);
}

std::uint64_t OtlpGrpcReceiver::accepted_span_count() const noexcept
{
    return accepted_span_count_.load(std::memory_order_relaxed);
}

} // namespace silhouette::ingestion
