#include "silhouette/ingestion/otlp_grpc_receiver.h"

#include "ingestion/otlp_span_converter.h"

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace silhouette::ingestion {

namespace otlp = opentelemetry::proto::collector::trace::v1;

class OtlpGrpcReceiver::TraceService final : public otlp::TraceService::Service {
public:
    TraceService(ActiveTraceManager& active_traces,
                 std::atomic<std::uint64_t>& accepted_request_count,
                 std::atomic<std::uint64_t>& accepted_span_count)
        : active_traces_{active_traces}
        , accepted_request_count_{accepted_request_count}
        , accepted_span_count_{accepted_span_count}
    {
    }

    grpc::Status Export(
        grpc::ServerContext*,
        const otlp::ExportTraceServiceRequest* request,
        otlp::ExportTraceServiceResponse* response) override
    {
        try {
            auto conversion = detail::ConvertOtlpSpans(*request);
            const auto accepted_span_count =
                static_cast<std::uint64_t>(conversion.spans.size());

            if (conversion.rejected_span_count > 0) {
                auto* partial_success = response->mutable_partial_success();
                partial_success->set_rejected_spans(
                    conversion.rejected_span_count);
                partial_success->set_error_message(
                    "Silhouette rejected spans with an invalid trace, span, "
                    "or parent span ID.");
            }

            active_traces_.AppendBatch(std::move(conversion.spans));
            accepted_span_count_.fetch_add(
                accepted_span_count, std::memory_order_relaxed);
            accepted_request_count_.fetch_add(1, std::memory_order_relaxed);

            return grpc::Status::OK;
        } catch (const std::bad_alloc&) {
            return {grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "Silhouette could not retain the exported spans"};
        } catch (const std::exception&) {
            return {grpc::StatusCode::INTERNAL,
                    "Silhouette could not process the exported spans"};
        }
    }

private:
    ActiveTraceManager& active_traces_;
    std::atomic<std::uint64_t>& accepted_request_count_;
    std::atomic<std::uint64_t>& accepted_span_count_;
};

OtlpGrpcReceiver::OtlpGrpcReceiver(
    std::string listen_address, ActiveTraceManager& active_traces)
    : listen_address_{std::move(listen_address)}
    , active_traces_{active_traces}
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
        active_traces_, accepted_request_count_, accepted_span_count_);

    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
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
