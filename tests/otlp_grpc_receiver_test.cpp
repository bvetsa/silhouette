#include "silhouette/active_trace_manager.h"
#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/span.h"
#include "active_trace_manager_test_peer.h"

#include <grpcpp/grpcpp.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace otlp = opentelemetry::proto::collector::trace::v1;
using Receiver = silhouette::ingestion::OtlpGrpcReceiver;
using silhouette::testing::ActiveTraceManagerTestPeer;

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
    silhouette::ActiveTraceManager active_traces;
    Receiver receiver{kEphemeralAddress, active_traces};
};

std::size_t RetainedSpanCount(const ReceiverFixture& fixture)
{
    const auto snapshot = fixture.active_traces.Snapshot();
    std::size_t count = 0;
    for (const auto& trace : snapshot) {
        count += trace.spans.size();
    }
    return count;
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
    silhouette::ActiveTraceManager active_traces;
    bool rejected = false;
    try {
        Receiver receiver{"", active_traces};
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

void EmptyExportIsAcceptedWithoutActiveTrace()
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
    Require(fixture.active_traces.active_trace_count() == 0,
            "empty export created an active trace");
    Require(fixture.active_traces.Snapshot().empty(),
            "empty export published active trace state");
}

void FullyValidExportPublishesActiveTraceState()
{
    ReceiverFixture fixture;
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);

    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 2, "checkout");
    const auto response = ExportSuccessfully(*stub, request);

    const auto snapshot = fixture.active_traces.Snapshot();
    Require(!response.has_partial_success(),
            "valid export unexpectedly set partial_success");
    Require(fixture.receiver.accepted_request_count() == 1,
            "valid export did not increment the request count");
    Require(fixture.receiver.accepted_span_count() == 1,
            "valid export did not increment the accepted-span count");
    Require(snapshot.size() == 1 && snapshot.front().spans.size() == 1,
            "valid export was not retained as one active trace");
    Require(snapshot.front().spans.front().span.operation_name == "checkout",
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
    const auto snapshot = fixture.active_traces.Snapshot();

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
    Require(snapshot.size() == 1 && snapshot.front().spans.size() == 1
                && snapshot.front().spans.front().span.operation_name == "valid",
            "partial export did not atomically publish only its valid span");
}

struct PublicationOrderSync final {
    std::latch manager_before_commit{1};
    std::latch allow_manager_commit{1};
};

void PauseManagerBeforeCommit(void* context) noexcept
{
    auto& sync = *static_cast<PublicationOrderSync*>(context);
    sync.manager_before_commit.count_down();
    sync.allow_manager_commit.wait();
}

void AcceptedSpanCountNeverLeadsPublishedState()
{
    ReceiverFixture fixture;
    PublicationOrderSync sync;
    ActiveTraceManagerTestPeer::SetBeforeCommitHook(
        fixture.active_traces, PauseManagerBeforeCommit, &sync);
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);

    std::exception_ptr export_error;
    std::thread exporter{[&] {
        try {
            otlp::ExportTraceServiceRequest request;
            AddValidSpan(request, 1, 1, "operation");
            ExportSuccessfully(*stub, request);
        } catch (...) {
            export_error = std::current_exception();
        }
    }};

    sync.manager_before_commit.wait();
    const auto span_count_before_publication =
        fixture.receiver.accepted_span_count();
    const auto request_count_before_publication =
        fixture.receiver.accepted_request_count();
    sync.allow_manager_commit.count_down();
    exporter.join();

    if (export_error) {
        std::rethrow_exception(export_error);
    }

    Require(span_count_before_publication == 0,
            "accepted-span count advanced before manager publication");
    Require(request_count_before_publication == 0,
            "accepted-request count advanced before manager publication");

    const auto observed_count = fixture.receiver.accepted_span_count();
    Require(observed_count == 1,
            "receiver reported the wrong accepted-span count after publication");
    Require(RetainedSpanCount(fixture) >= observed_count,
            "published manager state did not cover the accepted-span count");
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
    silhouette::ActiveTraceManager first_active_traces;
    {
        Receiver receiver{kEphemeralAddress, first_active_traces};
        receiver.Start();
        selected_address =
            "127.0.0.1:" + std::to_string(receiver.selected_port());
    }

    silhouette::ActiveTraceManager second_active_traces;
    Receiver replacement{selected_address, second_active_traces};
    replacement.Start();
}

void RejectsDuplicateBind()
{
    silhouette::ActiveTraceManager first_active_traces;
    Receiver first_receiver{kEphemeralAddress, first_active_traces};
    first_receiver.Start();
    const auto selected_address =
        "127.0.0.1:" + std::to_string(first_receiver.selected_port());

    silhouette::ActiveTraceManager second_active_traces;
    bool rejected = false;
    try {
        Receiver second_receiver{selected_address, second_active_traces};
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
    TestCase{"empty_export_is_accepted_without_active_trace",
             EmptyExportIsAcceptedWithoutActiveTrace},
    TestCase{"fully_valid_export_publishes_active_trace_state",
             FullyValidExportPublishesActiveTraceState},
    TestCase{"partially_valid_export_reports_rejection",
             PartiallyValidExportReportsRejection},
    TestCase{"accepted_span_count_never_leads_published_state",
             AcceptedSpanCountNeverLeadsPublishedState},
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
