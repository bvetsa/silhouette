#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/span.h"
#include "silhouette/span_capture.h"

#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace otlp = opentelemetry::proto::collector::trace::v1;
using Receiver = silhouette::ingestion::OtlpGrpcReceiver;

constexpr auto kEphemeralAddress = "127.0.0.1:0";

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::string MakeIdBytes(const std::size_t size, const std::uint8_t marker)
{
    std::string bytes(size, '\0');
    bytes.back() = static_cast<char>(marker);
    return bytes;
}

struct ReceiverFixture final {
    silhouette::SpanCapture capture;
    Receiver receiver{kEphemeralAddress, capture};
};

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

void AddValidSpan(
    otlp::ExportTraceServiceRequest& request,
    const std::uint8_t trace_marker,
    const std::uint8_t span_marker,
    std::string name)
{
    auto* span = request.add_resource_spans()
                     ->add_scope_spans()
                     ->add_spans();
    span->set_trace_id(MakeIdBytes(silhouette::TraceId::kSize, trace_marker));
    span->set_span_id(MakeIdBytes(silhouette::SpanId::kSize, span_marker));
    span->set_name(std::move(name));
}

void RejectsEmptyAddress()
{
    silhouette::SpanCapture capture;
    bool rejected = false;
    try {
        Receiver receiver{"", capture};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(rejected, "empty listen address was not rejected");
}

void StartsOnEphemeralPort()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();

    Require(fixture.receiver.selected_port() > 0,
            "receiver did not select a usable port");
}

void EmptyExportIsAcceptedWithoutCapture()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);

    const auto response = ExportSuccessfully(*stub, {});

    Require(!response.has_partial_success(),
            "empty successful export unexpectedly set partial_success");
    Require(fixture.receiver.accepted_request_count() == 1,
            "empty export did not increment the request count");
    Require(fixture.receiver.accepted_span_count() == 0,
            "empty export incremented the accepted-span count");
    Require(fixture.capture.size() == 0,
            "empty export published a capture batch");
}

void FullyValidExportPublishesCapture()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);

    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 2, "checkout");
    const auto response = ExportSuccessfully(*stub, request);

    const auto snapshot = fixture.capture.Snapshot();
    Require(!response.has_partial_success(),
            "valid export unexpectedly set partial_success");
    Require(fixture.receiver.accepted_request_count() == 1,
            "valid export did not increment the request count");
    Require(fixture.receiver.accepted_span_count() == 1,
            "valid export did not increment the accepted-span count");
    Require(snapshot.size() == 1, "valid export was not retained");
    Require(snapshot[0].operation_name == "checkout",
            "receiver published the wrong converted span");
}

void PartiallyValidExportReportsRejection()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);

    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 1, "valid");
    AddValidSpan(request, 2, 2, "invalid");
    request.mutable_resource_spans(1)
        ->mutable_scope_spans(0)
        ->mutable_spans(0)
        ->set_span_id(std::string(silhouette::SpanId::kSize, '\0'));

    const auto response = ExportSuccessfully(*stub, request);
    const auto snapshot = fixture.capture.Snapshot();

    Require(response.has_partial_success(),
            "partially valid export omitted partial_success");
    Require(response.partial_success().rejected_spans() == 1,
            "partial success reported the wrong rejected-span count");
    Require(!response.partial_success().error_message().empty(),
            "partial success omitted its explanation");
    Require(fixture.receiver.accepted_request_count() == 1,
            "partial export did not increment the request count");
    Require(fixture.receiver.accepted_span_count() == 1,
            "partial export counted a rejected span");
    Require(snapshot.size() == 1 && snapshot[0].operation_name == "valid",
            "partial export did not atomically publish only its valid span");
}

void RejectsSecondStart()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();

    bool rejected = false;
    try {
        fixture.receiver.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "receiver instance accepted a second Start call");
}

void AllowsShutdownBeforeStart()
{
    ReceiverFixture fixture;
    fixture.receiver.Shutdown();
    fixture.receiver.Start();
}

void AllowsRepeatedShutdown()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    fixture.receiver.Shutdown();
    fixture.receiver.Shutdown();
}

void RejectsRestartAfterShutdown()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    fixture.receiver.Shutdown();

    bool rejected = false;
    try {
        fixture.receiver.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }

    Require(rejected, "stopped receiver instance restarted");
}

void DestructorReleasesPort()
{
    std::string selected_address;
    silhouette::SpanCapture first_capture;
    {
        Receiver receiver{kEphemeralAddress, first_capture};
        receiver.Start();
        selected_address =
            "127.0.0.1:" + std::to_string(receiver.selected_port());
    }

    silhouette::SpanCapture second_capture;
    Receiver replacement{selected_address, second_capture};
    replacement.Start();
}

void RejectsDuplicateBind()
{
    silhouette::SpanCapture first_capture;
    Receiver first_receiver{kEphemeralAddress, first_capture};
    first_receiver.Start();
    const auto selected_address =
        "127.0.0.1:" + std::to_string(first_receiver.selected_port());

    silhouette::SpanCapture second_capture;
    bool rejected = false;
    try {
        Receiver second_receiver{selected_address, second_capture};
        second_receiver.Start();
    } catch (const std::runtime_error& error) {
        rejected =
            std::string{error.what()}.find(selected_address) != std::string::npos;
    }

    Require(rejected, "second receiver did not report the duplicate-port bind failure");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"rejects_empty_address", RejectsEmptyAddress},
    TestCase{"starts_on_ephemeral_port", StartsOnEphemeralPort},
    TestCase{"empty_export_is_accepted_without_capture",
             EmptyExportIsAcceptedWithoutCapture},
    TestCase{"fully_valid_export_publishes_capture",
             FullyValidExportPublishesCapture},
    TestCase{"partially_valid_export_reports_rejection",
             PartiallyValidExportReportsRejection},
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
