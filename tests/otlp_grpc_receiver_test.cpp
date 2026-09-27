#include "silhouette/ingestion/otlp_grpc_receiver.h"

#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace otlp = opentelemetry::proto::collector::trace::v1;
using Receiver = silhouette::ingestion::OtlpGrpcReceiver;

constexpr auto kEphemeralAddress = "127.0.0.1:0";
constexpr auto kBindConflictAddress = "127.0.0.1:54321";

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::unique_ptr<otlp::TraceService::Stub> Connect(const Receiver& receiver)
{
    const auto endpoint = "127.0.0.1:" + std::to_string(receiver.selected_port());
    auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());

    Require(channel->WaitForConnected(
                std::chrono::system_clock::now() + std::chrono::seconds{5}),
            "client did not connect to the receiver");

    return otlp::TraceService::NewStub(channel);
}

otlp::ExportTraceServiceResponse ExportSuccessfully(
    otlp::TraceService::Stub& stub,
    const otlp::ExportTraceServiceRequest& request)
{
    grpc::ClientContext client_context;
    client_context.set_deadline(
        std::chrono::system_clock::now() + std::chrono::seconds{5});

    otlp::ExportTraceServiceResponse response;
    const auto status = stub.Export(&client_context, request, &response);
    Require(status.ok(), "receiver did not acknowledge the export with gRPC OK");
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

void RejectsEmptyAddress()
{
    bool rejected = false;
    try {
        Receiver receiver{""};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(rejected, "empty listen address was not rejected");
}

void StartsOnEphemeralPort()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();

    Require(receiver.selected_port() > 0, "receiver did not select a usable port");
}

void AcknowledgesEmptyExport()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    auto stub = Connect(receiver);

    ExportSuccessfully(*stub, {});
}

void SuccessfulExportOmitsPartialSuccess()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    auto stub = Connect(receiver);

    const auto response = ExportSuccessfully(*stub, {});
    Require(!response.has_partial_success(),
            "successful export unexpectedly set partial_success");
}

void CountsExportRequest()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    auto stub = Connect(receiver);
    ExportSuccessfully(*stub, {});

    Require(receiver.accepted_request_count() == 1,
            "export did not increment the request count once");
}

void EmptyExportCountsZeroSpans()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    auto stub = Connect(receiver);
    ExportSuccessfully(*stub, {});

    Require(receiver.accepted_span_count() == 0,
            "empty export contributed spans");
}

void CountsNestedSpans()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    auto stub = Connect(receiver);
    ExportSuccessfully(*stub, MakeFiveSpanRequest());

    Require(receiver.accepted_span_count() == 5,
            "receiver did not count spans across resources and scopes");
}

void RejectsSecondStart()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();

    bool rejected = false;
    try {
        receiver.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "receiver instance accepted a second Start call");
}

void AllowsShutdownBeforeStart()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Shutdown();
    receiver.Start();
}

void AllowsRepeatedShutdown()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    receiver.Shutdown();
    receiver.Shutdown();
}

void RejectsRestartAfterShutdown()
{
    Receiver receiver{kEphemeralAddress};
    receiver.Start();
    receiver.Shutdown();

    bool rejected = false;
    try {
        receiver.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "stopped receiver instance restarted");
}

void DestructorReleasesPort()
{
    std::string selected_address;
    {
        Receiver receiver{kEphemeralAddress};
        receiver.Start();
        selected_address =
            "127.0.0.1:" + std::to_string(receiver.selected_port());
    }

    Receiver replacement{selected_address};
    replacement.Start();
}

void RejectsDuplicateBind()
{
    Receiver first_receiver{kBindConflictAddress};
    first_receiver.Start();

    bool rejected = false;
    try {
        Receiver second_receiver{kBindConflictAddress};
        second_receiver.Start();
    } catch (const std::runtime_error& error) {
        rejected =
            std::string{error.what()}.find(kBindConflictAddress) != std::string::npos;
    }

    Require(rejected, "second receiver did not report the fixed-port bind failure");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"rejects_empty_address", RejectsEmptyAddress},
    TestCase{"starts_on_ephemeral_port", StartsOnEphemeralPort},
    TestCase{"acknowledges_empty_export", AcknowledgesEmptyExport},
    TestCase{"successful_export_omits_partial_success",
             SuccessfulExportOmitsPartialSuccess},
    TestCase{"counts_export_request", CountsExportRequest},
    TestCase{"empty_export_counts_zero_spans", EmptyExportCountsZeroSpans},
    TestCase{"counts_nested_spans", CountsNestedSpans},
    TestCase{"rejects_second_start", RejectsSecondStart},
    TestCase{"allows_shutdown_before_start", AllowsShutdownBeforeStart},
    TestCase{"allows_repeated_shutdown", AllowsRepeatedShutdown},
    TestCase{"rejects_restart_after_shutdown", RejectsRestartAfterShutdown},
    TestCase{"destructor_releases_port", DestructorReleasesPort},
    TestCase{"rejects_duplicate_bind", RejectsDuplicateBind},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one receiver test-case name.\n";
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

    std::cerr << "Unknown receiver test case: " << requested_case << '\n';
    return 1;
}
