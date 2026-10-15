#include "query_live_map_test_support.h"
#include "silhouette/active_trace_manager.h"
#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/local_map_server.h"
#include "silhouette/span.h"
#include "active_trace_manager_test_peer.h"
#include "local_map_server_test_peer.h"
#include "trace_lifecycle_test_support.h"
#include "otlp_grpc_receiver_test_peer.h"
#include "application_shutdown_guard.h"
#include "rendering/service_graph_artifacts.h"

#include <grpcpp/grpcpp.h>
#include <httplib.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <atomic>
#include <fstream>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

silhouette::TraceId MakeTraceId(const std::uint8_t marker)
{
    silhouette::TraceId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    const auto trace_id = silhouette::TraceId::FromBytes(bytes);
    Require(trace_id.has_value(), "test trace ID was invalid");
    return *trace_id;
}

struct ReceiverFixture final {
    silhouette::ActiveTraceManager active_traces;
    Receiver receiver{kEphemeralAddress, active_traces};
};

std::size_t RetainedSpanCount(
    const silhouette::ActiveTraceManager& active_traces)
{
    const auto snapshot = active_traces.Snapshot();
    std::size_t count = 0;
    for (const auto& trace : snapshot) {
        count += trace.spans.size();
    }
    return count;
}

std::size_t RetainedSpanCount(const ReceiverFixture& fixture)
{
    return RetainedSpanCount(fixture.active_traces);
}

class RecordingUpdateSink final : public silhouette::TraceUpdateSink {
public:
    explicit RecordingUpdateSink(
        const silhouette::ActiveTraceManager& active_traces)
        : active_traces_{active_traces}
    {
    }

    void SetReceiver(const Receiver& receiver) { receiver_ = &receiver; }

    void NotifyTracesDirty(
        std::vector<silhouette::TraceId> trace_ids) noexcept override
    {
        std::scoped_lock lock{mutex_};
        notifications_.push_back(std::move(trace_ids));
        retained_counts_.push_back(RetainedSpanCount(active_traces_));
        accepted_counts_.push_back(receiver_->accepted_span_count());
    }

    [[nodiscard]] std::vector<std::vector<silhouette::TraceId>>
    notifications() const
    {
        std::scoped_lock lock{mutex_};
        return notifications_;
    }

    [[nodiscard]] std::vector<std::size_t> retained_counts() const
    {
        std::scoped_lock lock{mutex_};
        return retained_counts_;
    }

    [[nodiscard]] std::vector<std::uint64_t> accepted_counts() const
    {
        std::scoped_lock lock{mutex_};
        return accepted_counts_;
    }

private:
    const silhouette::ActiveTraceManager& active_traces_;
    const Receiver* receiver_{nullptr};
    mutable std::mutex mutex_;
    std::vector<std::vector<silhouette::TraceId>> notifications_;
    std::vector<std::size_t> retained_counts_;
    std::vector<std::uint64_t> accepted_counts_;
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
    silhouette::ActiveTraceManager active_traces;
    bool rejected = false;
    try {
        Receiver receiver{"", active_traces};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    Require(rejected, "empty listen address was not rejected");
}

void HttpWorkerStartupFailureDoesNotDisableIngestion()
{
    ReceiverFixture fixture;
    silhouette::rendering::GraphvizServiceGraphRenderer renderer;
    std::ostringstream diagnostics;
    silhouette::testing::QueryLiveMap live_map{
        fixture.active_traces, renderer, diagnostics};
    silhouette::LocalMapServer server{"127.0.0.1", 0, live_map, live_map.queries()};
    silhouette::testing::LocalMapServerTestPeer::FailWorkerStartup(server);
    bool reported = false;
    try {
        server.Start();
    } catch (const std::exception& error) {
        reported = std::string_view{error.what()}.find(
            "injected HTTP worker startup failure") != std::string_view::npos;
    }
    Require(reported, "HTTP worker failure did not reach the startup handler");

    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);
    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 1, "after HTTP worker failure");
    ExportSuccessfully(*stub, request);
    Require(fixture.receiver.accepted_span_count() == 1
                && RetainedSpanCount(fixture) == 1,
            "failed HTTP worker startup disabled OTLP ingestion");
    fixture.receiver.Shutdown();
    server.Shutdown();
}

void HttpEnqueueFailureDoesNotDisableIngestion()
{
    ReceiverFixture fixture;
    silhouette::rendering::GraphvizServiceGraphRenderer renderer;
    std::ostringstream diagnostics;
    silhouette::testing::QueryLiveMap live_map{
        fixture.active_traces, renderer, diagnostics};
    silhouette::LocalMapServer server{"127.0.0.1", 0, live_map, live_map.queries()};
    std::size_t attempts = 0;
    silhouette::testing::LocalMapServerTestPeer::BeforeEnqueue(server, [&] {
        if (++attempts == 1) {
            throw std::bad_alloc{};
        }
    });
    server.Start();
    fixture.receiver.Start();
    auto stub = Connect(fixture.receiver);
    httplib::Client client{"127.0.0.1", server.selected_port()};
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    Require(!client.Get("/"), "HTTP admission failure was not injected");

    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 1, "after HTTP enqueue failure");
    ExportSuccessfully(*stub, request);
    Require(fixture.receiver.accepted_span_count() == 1
                && RetainedSpanCount(fixture) == 1,
            "HTTP enqueue failure disabled OTLP ingestion");
    const auto response = client.Get("/");
    Require(response && response->status == 200,
            "HTTP enqueue failure disabled later presentation requests");
    fixture.receiver.Shutdown();
    server.Shutdown();
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

void NonEmptyExportsNotifyAfterPublication()
{
    silhouette::ActiveTraceManager active_traces;
    RecordingUpdateSink sink{active_traces};
    Receiver receiver{kEphemeralAddress, active_traces, &sink};
    sink.SetReceiver(receiver);
    receiver.Start();
    auto stub = Connect(receiver);

    ExportSuccessfully(*stub, {});
    Require(sink.notifications().empty(),
            "empty export emitted a dirty-trace notification");

    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 2, 1, "trace-two");
    AddValidSpan(request, 1, 1, "trace-one");
    AddValidSpan(request, 2, 2, "trace-two-again");
    ExportSuccessfully(*stub, request);

    const auto notifications = sink.notifications();
    Require(notifications.size() == 1,
            "one accepted export did not emit exactly one notification");
    Require(notifications.front()
                == std::vector<silhouette::TraceId>{
                    MakeTraceId(1),
                    MakeTraceId(2),
                },
            "notification did not contain sorted unique affected trace IDs");

    const auto retained_counts = sink.retained_counts();
    const auto accepted_counts = sink.accepted_counts();
    Require(retained_counts == std::vector<std::size_t>{3},
            "notification ran before the complete accepted batch was retained");
    Require(accepted_counts == std::vector<std::uint64_t>{3},
            "notification ran before accepted counters advanced");
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

void RealOtlpQuiescenceAndLateParent()
{
    using namespace std::chrono_literals;
    using silhouette::testing::FakeTraceClock;
    using silhouette::testing::LifecycleSweepSync;
    using silhouette::testing::AdvanceAndWakeLifecycle;
    FakeTraceClock clock;
    LifecycleSweepSync sweeps;
    silhouette::ActiveTraceManager active_traces{1h, silhouette::TraceRetentionLimits{.max_quiescent_age = 24h}, FakeTraceClock::Now, &clock};
    silhouette::rendering::GraphvizServiceGraphRenderer renderer;
    std::ostringstream diagnostics;
    silhouette::testing::QueryLiveMap live_map{active_traces, renderer, diagnostics};
    Receiver receiver{kEphemeralAddress, active_traces, &live_map};
    ActiveTraceManagerTestPeer::SetAfterSweepHook(
        active_traces, LifecycleSweepSync::Signal, &sweeps);
    active_traces.StartLifecycle();
    live_map.Start();
    Require(live_map.WaitForPublishedAfter(std::nullopt).has_value(),
            "initial Graphviz SVG was not published");
    receiver.Start();
    const auto stub = Connect(receiver);

    otlp::ExportTraceServiceRequest child;
    AddValidSpan(child, 1, 2, "checkout-child");
    auto* child_resource = child.mutable_resource_spans(0);
    auto* attribute = child_resource->mutable_resource()->add_attributes();
    attribute->set_key("service.name");
    attribute->mutable_value()->set_string_value("checkout");
    child_resource->mutable_scope_spans(0)->mutable_spans(0)->set_parent_span_id(
        MakeIdBytes(silhouette::SpanId::kSize, 1));
    ExportSuccessfully(*stub, child);
    const auto first = live_map.WaitForPublishedAfter(0);
    Require(first && first->has_telemetry && active_traces.active_trace_count() == 1
                && live_map.SnapshotGraph().gaps.size() == 1,
            "real child export did not create an active retained gap");

    sweeps.WaitFor(AdvanceAndWakeLifecycle(active_traces, clock, sweeps, 1h));
    Require(active_traces.quiescent_trace_count() == 1
                && RetainedSpanCount(active_traces) == 1
                && live_map.topology_version() == 1
                && live_map.SnapshotPublished()->svg == first->svg,
            "real exported trace lost evidence or topology on quiescence");
    ExportSuccessfully(*stub, {});
    Require(active_traces.quiescent_trace_count() == 1,
            "empty real export refreshed quiescent evidence");

    otlp::ExportTraceServiceRequest parent;
    AddValidSpan(parent, 1, 1, "frontend-parent");
    attribute = parent.mutable_resource_spans(0)->mutable_resource()
                    ->add_attributes();
    attribute->set_key("service.name");
    attribute->mutable_value()->set_string_value("frontend");
    ExportSuccessfully(*stub, parent);
    const auto reopened = live_map.WaitForPublishedAfter(1);
    receiver.Shutdown();
    active_traces.StopLifecycle();
    live_map.CloseAndDrain();
    const auto graph = live_map.SnapshotGraph();
    Require(reopened && reopened->has_telemetry && reopened->rendered_version == 2
                && active_traces.lifecycle_counts()
                    == silhouette::ActiveTraceManager::LifecycleCounts{1, 0, 1}
                && RetainedSpanCount(active_traces) == 2
                && receiver.accepted_request_count() == 3
                && receiver.accepted_span_count() == 2
                && graph.gaps.empty()
                && graph.edges == std::vector<silhouette::ServiceEdge>{
                    {"frontend", "checkout"}}
                && graph == silhouette::BuildServiceGraph(active_traces.Snapshot()),
            "real late-parent export failed reopening, topology, counters, or shutdown");
}


void CapacityPartialSuccessAndCumulativeCounters()
{
    using namespace std::chrono_literals;
    silhouette::testing::FakeTraceClock clock;
    silhouette::ActiveTraceManager traces{2s, silhouette::TraceRetentionLimits{
        .max_quiescent_age = 10s, .max_spans_per_trace = 3},
        silhouette::testing::FakeTraceClock::Now, &clock};
    RecordingUpdateSink sink{traces};
    Receiver receiver{kEphemeralAddress, traces, &sink};
    sink.SetReceiver(receiver);
    receiver.Start();
    const auto stub = Connect(receiver);
    otlp::ExportTraceServiceRequest seed;
    AddValidSpan(seed, 1, 1, "a"); AddValidSpan(seed, 1, 2, "b");
    Require(!ExportSuccessfully(*stub, seed).has_partial_success(), "seed export was rejected");
    clock.Advance(9s);
    otlp::ExportTraceServiceRequest mixed;
    for (std::uint8_t i : {3, 4, 5}) AddValidSpan(mixed, 1, i, "over-cap");
    AddValidSpan(mixed, 2, 1, "sibling"); AddValidSpan(mixed, 2, 2, "sibling");
    AddValidSpan(mixed, 3, 1, "invalid");
    mixed.mutable_resource_spans(5)->mutable_scope_spans(0)->mutable_spans(0)
        ->set_span_id(std::string(silhouette::SpanId::kSize, '\0'));
    const auto response = ExportSuccessfully(*stub, mixed);
    Require(response.partial_success().rejected_spans() == 4
                && response.partial_success().error_message()
                    == "Silhouette rejected 1 span(s) with invalid identifiers and 3 span(s) because a retained trace would exceed its span limit."
                && receiver.accepted_span_count() == 4 && RetainedSpanCount(traces) == 4
                && sink.notifications().back() == std::vector<silhouette::TraceId>{MakeTraceId(2)},
            "mixed validation/capacity rejection changed counts, siblings, or notification IDs");
    otlp::ExportTraceServiceRequest smaller;
    AddValidSpan(smaller, 1, 3, "fits");
    Require(!ExportSuccessfully(*stub, smaller).has_partial_success(), "fitting later batch rejected");
    const auto notifications = sink.notifications().size();
    const auto rejected = ExportSuccessfully(*stub, smaller);
    Require(rejected.partial_success().rejected_spans() == 1
                && rejected.partial_success().error_message()
                    == "Silhouette rejected 1 span(s) because a retained trace would exceed its span limit."
                && receiver.accepted_span_count() == 5 && receiver.accepted_request_count() == 4
                && sink.notifications().size() == notifications,
            "fully capacity-rejected export counted or notified unretained evidence");
    clock.Advance(10s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(traces));
    Require(traces.Snapshot().empty() && receiver.accepted_span_count() == 5,
            "eviction incorrectly decremented cumulative admission counters");
    receiver.Shutdown();
}

struct ResponsePreparationFailure final {
    std::atomic<bool> fail{true};
    static void Prepare(void* context)
    {
        if (static_cast<ResponsePreparationFailure*>(context)->fail.exchange(false)) {
            throw std::bad_alloc{};
        }
    }
};

void ResponsePreparationFailurePublishesNothingAndRetryAdmitsOnce()
{
    ResponsePreparationFailure failure;
    silhouette::ActiveTraceManager traces;
    RecordingUpdateSink sink{traces};
    Receiver receiver{kEphemeralAddress, traces, &sink};
    sink.SetReceiver(receiver);
    silhouette::testing::OtlpGrpcReceiverTestPeer::SetBeforeResponsePrepareHook(
        receiver, ResponsePreparationFailure::Prepare, &failure);
    receiver.Start();
    const auto stub = Connect(receiver);
    otlp::ExportTraceServiceRequest request;
    AddValidSpan(request, 1, 1, "retry-once");
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
    otlp::ExportTraceServiceResponse response;
    const auto status = stub->Export(&context, request, &response);
    Require(status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED
                && traces.Snapshot().empty() && sink.notifications().empty()
                && receiver.accepted_span_count() == 0 && receiver.accepted_request_count() == 0,
            "fallible response preparation ran after manager publication or counters");
    Require(!ExportSuccessfully(*stub, request).has_partial_success()
                && RetainedSpanCount(traces) == 1 && receiver.accepted_span_count() == 1
                && receiver.accepted_request_count() == 1 && sink.notifications().size() == 1,
            "retry after preparation failure duplicated or rejected retained evidence");
    receiver.Shutdown();
}

void ShutdownGuardStopsLifecycleAfterPartialStartup()
{
    using namespace std::chrono_literals;
    silhouette::testing::FakeTraceClock clock;
    silhouette::testing::LifecycleSweepSync sweeps;
    silhouette::ActiveTraceManager traces{1h, {.max_quiescent_age = 24h},
        silhouette::testing::FakeTraceClock::Now, &clock};
    silhouette::rendering::GraphvizServiceGraphRenderer renderer;
    std::ostringstream diagnostics;
    silhouette::testing::QueryLiveMap map{traces, renderer, diagnostics};
    silhouette::LocalMapServer server{"127.0.0.1", 0, map, map.queries()};
    Receiver receiver{"invalid-address", traces, &map};
    ActiveTraceManagerTestPeer::SetAfterSweepHook(traces,
        silhouette::testing::LifecycleSweepSync::Signal, &sweeps);
    bool unwound = false;
    try {
        silhouette::ApplicationShutdownGuard shutdown{receiver, traces, map.queries(), map, server};
        traces.StartLifecycle(&map);
        map.Start();
        sweeps.WaitFor(1);
        throw std::runtime_error{"injected failure before receiver/HTTP startup"};
    } catch (const std::runtime_error&) {
        unwound = true;
    }
    bool stopped = false;
    try { traces.StartLifecycle(&map); } catch (const std::logic_error&) { stopped = true; }
    Require(unwound && stopped && map.publisher_stopped(),
            "partial startup unwinding failed to stop the worker before its sink");
    // Pre-start shutdown of receiver/server must remain harmless.
    receiver.Shutdown(); server.Shutdown();
}

void RealTimeRetentionExpiresWithoutNewExports()
{
    using namespace std::chrono_literals;
    silhouette::ActiveTraceManager traces{100ms, {.max_quiescent_age = 2s}};
    silhouette::rendering::GraphvizServiceGraphRenderer renderer;
    std::ostringstream diagnostics;
    silhouette::testing::QueryLiveMap map{traces, renderer, diagnostics};
    silhouette::LocalMapServer server{"127.0.0.1", 0, map, map.queries()};
    Receiver receiver{kEphemeralAddress, traces, &map};
    silhouette::ApplicationShutdownGuard shutdown{receiver, traces, map.queries(), map, server};
    traces.StartLifecycle(&map); map.Start(); server.Start(); receiver.Start();
    const auto stub = Connect(receiver);
    Require(map.WaitForPublishedAfter(std::nullopt, 5s).has_value(), "initial SVG missing");
    const auto request_for = [](const std::uint8_t span_id, const std::string& service) {
        otlp::ExportTraceServiceRequest request;
        AddValidSpan(request, 1, span_id, service);
        auto* attribute = request.mutable_resource_spans(0)->mutable_resource()->add_attributes();
        attribute->set_key("service.name"); attribute->mutable_value()->set_string_value(service);
        return request;
    };
    ExportSuccessfully(*stub, request_for(1, "expiredservice"));
    Require(map.WaitForPublishedAfter(0, 5s).has_value(), "initial evidence did not render");
    const auto empty = map.WaitForPublishedAfter(1, 10s);
    Require(empty && !empty->has_telemetry && traces.Snapshot().empty(),
            "real age deadline required new traffic to evict retained history");
    httplib::Client http{"127.0.0.1", server.selected_port()};
    const auto empty_svg = http.Get("/graph.svg");
    Require(empty_svg && empty_svg->status == 200
                && empty_svg->body.find("expiredservice") == std::string::npos,
            "HTTP continued publishing the evicted service");
    ExportSuccessfully(*stub, request_for(2, "freshservice"));
    receiver.Shutdown(); traces.StopLifecycle();
    const auto fresh = map.WaitForPublishedAfter(2, 5s);
    const auto retained = traces.Snapshot();
    Require(fresh && fresh->has_telemetry && retained.size() == 1
                && retained.front().spans.size() == 1 && receiver.accepted_span_count() == 2,
            "reappearance recovered old evidence or lost cumulative admissions");
    const auto fresh_svg = http.Get("/graph.svg");
    Require(fresh_svg && fresh_svg->body.find("freshservice") != std::string::npos,
            "HTTP failed to serve the new retained incarnation");
    shutdown.Shutdown();
    const auto graph = silhouette::BuildServiceGraph(traces.Snapshot());
    Require(map.SnapshotGraph() == graph, "shutdown failed to converge retained topology");
    const auto artifacts = silhouette::rendering::WriteServiceGraphArtifacts(graph);
    Require(artifacts.svg_rendered, "final retained SVG failed to render");
    std::ifstream dot_file{"silhouette.dot"};
    std::ifstream svg_file{"silhouette.svg"};
    const std::string dot{std::istreambuf_iterator<char>{dot_file}, {}};
    const std::string svg{std::istreambuf_iterator<char>{svg_file}, {}};
    Require(dot.find("freshservice") != std::string::npos
                && svg.find("<svg") != std::string::npos
                && svg.find("freshservice") != std::string::npos
                && dot.find("expiredservice") == std::string::npos
                && svg.find("expiredservice") == std::string::npos,
            "final artifacts were not derived exclusively from retained evidence");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"capacity_partial_success_and_cumulative_counters",
             CapacityPartialSuccessAndCumulativeCounters},
    TestCase{"response_preparation_failure_is_atomic",
             ResponsePreparationFailurePublishesNothingAndRetryAdmitsOnce},
    TestCase{"shutdown_guard_handles_partial_startup",
             ShutdownGuardStopsLifecycleAfterPartialStartup},
    TestCase{"real_time_retention_expires_without_new_exports",
             RealTimeRetentionExpiresWithoutNewExports},

    TestCase{"real_otlp_quiescence_and_late_parent",
             RealOtlpQuiescenceAndLateParent},
    TestCase{"http_enqueue_failure_does_not_disable_ingestion",
             HttpEnqueueFailureDoesNotDisableIngestion},
    TestCase{"http_worker_startup_failure_does_not_disable_ingestion",
             HttpWorkerStartupFailureDoesNotDisableIngestion},
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
    TestCase{"non_empty_exports_notify_after_publication",
             NonEmptyExportsNotifyAfterPublication},
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
