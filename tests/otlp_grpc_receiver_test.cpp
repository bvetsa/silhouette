#include "silhouette/ingestion/otlp_grpc_receiver.h"

#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

namespace otlp = opentelemetry::proto::collector::trace::v1;

class TestContext final {
public:
    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failure_count_;
        }
    }

    [[nodiscard]] int failure_count() const noexcept
    {
        return failure_count_;
    }

private:
    int failure_count_{0};
};

std::unique_ptr<otlp::TraceService::Stub> Connect(
    const silhouette::ingestion::OtlpGrpcReceiver& receiver,
    TestContext& context)
{
    const auto endpoint = "127.0.0.1:" + std::to_string(receiver.selected_port());
    auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());

    context.Expect(
        channel->WaitForConnected(
            std::chrono::system_clock::now() + std::chrono::seconds{5}),
        "client connects to the started receiver");

    return otlp::TraceService::NewStub(channel);
}

otlp::ExportTraceServiceResponse Export(
    otlp::TraceService::Stub& stub,
    const otlp::ExportTraceServiceRequest& request,
    TestContext& context)
{
    grpc::ClientContext client_context;
    client_context.set_deadline(
        std::chrono::system_clock::now() + std::chrono::seconds{5});

    otlp::ExportTraceServiceResponse response;
    const auto status = stub.Export(&client_context, request, &response);
    context.Expect(status.ok(), "receiver acknowledges the export with gRPC OK");
    return response;
}

otlp::ExportTraceServiceRequest MakeFiveSpanRequest()
{
    otlp::ExportTraceServiceRequest request;

    auto* first_resource = request.add_resource_spans();
    auto* first_scope = first_resource->add_scope_spans();
    first_scope->add_spans();
    first_scope->add_spans();
    first_resource->add_scope_spans()->add_spans();

    auto* second_scope = request.add_resource_spans()->add_scope_spans();
    second_scope->add_spans();
    second_scope->add_spans();

    return request;
}

} // namespace

int main()
{
    TestContext context;

    bool empty_address_rejected = false;
    try {
        silhouette::ingestion::OtlpGrpcReceiver receiver{""};
    } catch (const std::invalid_argument&) {
        empty_address_rejected = true;
    }
    context.Expect(empty_address_rejected,
                   "an empty listen address is rejected");

    {
        silhouette::ingestion::OtlpGrpcReceiver receiver{"127.0.0.1:0"};
        receiver.Shutdown();
        receiver.Start();

        context.Expect(receiver.selected_port() > 0,
                       "ephemeral listen address selects a real port");

        auto stub = Connect(receiver, context);

        const auto empty_response = Export(*stub, {}, context);
        context.Expect(!empty_response.has_partial_success(),
                       "empty successful export leaves partial_success unset");
        context.Expect(receiver.accepted_request_count() == 1,
                       "empty export increments the request count");
        context.Expect(receiver.accepted_span_count() == 0,
                       "empty export contributes no spans");

        const auto populated_response = Export(*stub, MakeFiveSpanRequest(), context);
        context.Expect(!populated_response.has_partial_success(),
                       "populated successful export leaves partial_success unset");
        context.Expect(receiver.accepted_request_count() == 2,
                       "both exports are counted");
        context.Expect(receiver.accepted_span_count() == 5,
                       "spans are counted across resources and scopes");

        bool second_start_rejected = false;
        try {
            receiver.Start();
        } catch (const std::logic_error&) {
            second_start_rejected = true;
        }
        context.Expect(second_start_rejected,
                       "a receiver instance cannot be started twice");

        receiver.Shutdown();
        receiver.Shutdown();

        second_start_rejected = false;
        try {
            receiver.Start();
        } catch (const std::logic_error&) {
            second_start_rejected = true;
        }
        context.Expect(second_start_rejected,
                       "a stopped receiver instance cannot be restarted");
    }

    {
        silhouette::ingestion::OtlpGrpcReceiver receiver{"127.0.0.1:0"};
        receiver.Start();
    }

    if (context.failure_count() != 0) {
        std::cerr << context.failure_count() << " receiver test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "OTLP/gRPC receiver tests passed.\n";
    return 0;
}
