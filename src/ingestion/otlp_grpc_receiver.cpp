#include "silhouette/ingestion/otlp_grpc_receiver.h"

#include "ingestion/otlp_span_converter.h"

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <limits>
#include <new>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace silhouette::ingestion {

namespace otlp = opentelemetry::proto::collector::trace::v1;

class OtlpGrpcReceiver::TraceService final : public otlp::TraceService::Service {
public:
    TraceService(ActiveTraceManager& active_traces,
                 TraceUpdateSink* update_sink,
                 std::atomic<std::uint64_t>& accepted_request_count,
                 std::atomic<std::uint64_t>& accepted_span_count,
                 std::mutex& publication_mutex,
                 void (*before_response_prepare)(void*), void* prepare_context)
        : active_traces_{active_traces}
        , update_sink_{update_sink}
        , accepted_request_count_{accepted_request_count}
        , accepted_span_count_{accepted_span_count}
        , publication_mutex_{publication_mutex}
        , before_response_prepare_{before_response_prepare}
        , prepare_context_{prepare_context}
    {
    }

    grpc::Status Export(
        grpc::ServerContext*,
        const otlp::ExportTraceServiceRequest* request,
        otlp::ExportTraceServiceResponse* response) override
    {
        try {
            auto conversion = detail::ConvertOtlpSpans(*request);
            if (before_response_prepare_) {
                before_response_prepare_(prepare_context_);
            }
            // Prepare the protobuf object AND its string storage before any
            // admission. The longest fixed text plus two 64-bit decimal counts
            // fits within 256 bytes; formatting below uses only this buffer.
            constexpr std::size_t kMessageCapacity = 256;
            constexpr std::string_view kPrefix = "Silhouette rejected ";
            constexpr std::string_view kInvalid = " span(s) with invalid identifiers";
            constexpr std::string_view kAnd = " and ";
            constexpr std::string_view kCapacity =
                " span(s) because a retained trace would exceed its span limit";
            constexpr auto kDigits = std::max(std::numeric_limits<std::size_t>::digits10,
                                              std::numeric_limits<std::int64_t>::digits10) + 1;
            static_assert(kPrefix.size() + kInvalid.size() + kAnd.size()
                              + kCapacity.size() + 2 * kDigits + 1 <= kMessageCapacity);
            auto* partial = response->mutable_partial_success();
            auto* message = partial->mutable_error_message();
            message->reserve(kMessageCapacity);

            {
                std::scoped_lock publication_lock{publication_mutex_};
                auto result =
                    active_traces_.AppendBatch(std::move(conversion.spans));
                const auto invalid = conversion.rejected_span_count;
                const auto capacity = result.capacity_rejected_span_count;
                if (invalid != 0 || capacity != 0) {
                    std::array<char, kMessageCapacity> buffer{};
                    auto* cursor = buffer.data();
                    const auto text = [&](const std::string_view value) {
                        cursor = std::copy(value.begin(), value.end(), cursor);
                    };
                    const auto count = [&](const auto value) {
                        const auto formatted = std::to_chars(
                            cursor, buffer.data() + buffer.size(), value);
                        assert(formatted.ec == std::errc{});
                        cursor = formatted.ptr;
                    };
                    text(kPrefix);
                    if (invalid != 0) {
                        count(invalid);
                        text(kInvalid);
                        if (capacity != 0) {
                            text(kAnd);
                        }
                    }
                    if (capacity != 0) {
                        count(capacity);
                        text(kCapacity);
                    }
                    text(".");
                    partial->set_rejected_spans(
                        invalid + static_cast<std::int64_t>(capacity));
                    message->assign(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
                } else {
                    response->clear_partial_success();
                }
                accepted_span_count_.fetch_add(
                    result.accepted_span_count, std::memory_order_relaxed);
                accepted_request_count_.fetch_add(1, std::memory_order_relaxed);
                if (update_sink_ != nullptr && !result.affected_trace_ids.empty()) {
                    update_sink_->NotifyTracesDirty(
                        std::move(result.affected_trace_ids));
                }
            }

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
    TraceUpdateSink* update_sink_;
    std::atomic<std::uint64_t>& accepted_request_count_;
    std::atomic<std::uint64_t>& accepted_span_count_;
    std::mutex& publication_mutex_;
    void (*before_response_prepare_)(void*);
    void* prepare_context_;
};

OtlpGrpcReceiver::OtlpGrpcReceiver(
    std::string listen_address,
    ActiveTraceManager& active_traces,
    TraceUpdateSink* update_sink)
    : listen_address_{std::move(listen_address)}
    , active_traces_{active_traces}
    , update_sink_{update_sink}
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
        active_traces_, update_sink_, accepted_request_count_,
        accepted_span_count_, publication_mutex_, before_response_prepare_hook_,
        before_response_prepare_context_);

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
