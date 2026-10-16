// Interactive verification fixture, not a product endpoint. All evidence enters
// through real OTLP/gRPC; stdin controls fake time and the next export.
#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/local_map_server.h"
#include "observability_query_service_test_peer.h"
#include "trace_lifecycle_test_support.h"

#include "opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

namespace {
using namespace silhouette;
using namespace std::chrono_literals;
using testing::FakeTraceClock;
using testing::LifecycleSweepSync;
std::string Bytes(unsigned value, std::size_t size)
{
    std::string bytes(size, '\0');
    bytes[size - 2] = static_cast<char>(value >> 8);
    bytes[size - 1] = static_cast<char>(value & 255); return bytes;
}
struct Evidence final { unsigned trace, span, parent; std::string service, operation; };
}

int main(int argc, char** argv)
{
    FakeTraceClock clock;
    ActiveTraceManager manager{30s, FakeTraceClock::Now, &clock};
    rendering::GraphvizServiceGraphRenderer renderer;
    ObservabilityQueryService queries{manager, std::cerr};
    LiveServiceMap map{queries, renderer, std::cerr};
    LocalMapServer server{"127.0.0.1", argc == 2 ? std::stoi(argv[1]) : 0, map, queries};
    ingestion::OtlpGrpcReceiver receiver{"127.0.0.1:0", manager, &queries};
    struct Shutdown final {
        ingestion::OtlpGrpcReceiver& receiver;
        ActiveTraceManager& manager;
        ObservabilityQueryService& queries;
        LiveServiceMap& map;
        LocalMapServer& server;
        ~Shutdown() {
            receiver.Shutdown(); manager.StopLifecycle(); queries.CloseAndDrain();
            map.CloseAndDrain(); server.Shutdown();
        }
    } shutdown{receiver, manager, queries, map, server};
    LifecycleSweepSync changes, sweeps;
    testing::ObservabilityQueryServiceTestPeer::AfterReconcile(queries, LifecycleSweepSync::Signal, &changes);
    testing::ActiveTraceManagerTestPeer::SetAfterSweepHook(manager, LifecycleSweepSync::Signal, &sweeps);
    queries.Start(); manager.StartLifecycle(&queries, &queries); map.Start(); server.Start(); receiver.Start();
    using Service = opentelemetry::proto::collector::trace::v1::TraceService;
    auto stub = Service::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(receiver.selected_port()), grpc::InsecureChannelCredentials()));
    std::size_t expected = 0;
    const auto publish = [&](const std::vector<Evidence>& evidence) {
        opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request;
        for (const auto& item : evidence) {
            auto* resource = request.add_resource_spans();
            auto* attribute = resource->mutable_resource()->add_attributes();
            attribute->set_key("service.name"); attribute->mutable_value()->set_string_value(item.service);
            auto* span = resource->add_scope_spans()->add_spans();
            span->set_trace_id(Bytes(item.trace, 16)); span->set_span_id(Bytes(item.span, 8));
            if (item.parent) span->set_parent_span_id(Bytes(item.parent, 8));
            span->set_name(item.operation); span->set_start_time_unix_nano(100);
            span->set_end_time_unix_nano(200);
        }
        grpc::ClientContext context;
        opentelemetry::proto::collector::trace::v1::ExportTraceServiceResponse response;
        const auto result = stub->Export(&context, request, &response);
        if (!result.ok() || response.has_partial_success()) throw std::runtime_error{"fixture OTLP export failed"};
        changes.WaitFor(++expected);
    };
    std::cout << "READY http://127.0.0.1:" << server.selected_port() << std::endl;
    try {
        std::string command;
        while (std::getline(std::cin, command) && command != "stop") {
            if (command == "seed") publish({
                {1, 2, 42, "checkout", "complete order"},
                {2, 1, 0, "frontend", "POST /orders"},
                {2, 2, 1, "checkout", "reserve stock"},
                {2, 3, 2, "inventory", "lookup item"},
                {3, 1, 0, "audit <script>alert(1)</script> & \"雪\"", "record event"}});
            else if (command == "parent") publish({{1, 42, 0, "frontend", "late parent"}});
            else if (command == "reappear") publish({{1, 2, 42, "checkout", "new incarnation"}});
            else if (command == "extra-a" || command == "extra-b" || command == "extra-c") {
                const auto marker = static_cast<unsigned>(100 + command.back() - 'a');
                publish({{marker, 1, 0, "shipping-" + command.substr(6), "dispatch"}});
            } else if (command == "pages") {
                std::vector<Evidence> records;
                for (unsigned i = 10; i < 61; ++i) records.push_back({i, 1, 0, "checkout", "paged request"});
                publish(records);
            } else if (command == "large") {
                std::vector<Evidence> records;
                for (unsigned i = 1; i <= 10000; ++i) records.push_back({4, i, 0, "batch-worker", "parallel item"});
                publish(records);
            } else if (command == "quiet" || command == "expire") {
                const auto next = testing::AdvanceAndWakeLifecycle(manager, clock, sweeps,
                    command == "quiet" ? 30s : 601s);
                sweeps.WaitFor(next); changes.WaitFor(++expected);
            } else throw std::runtime_error{"unknown fixture command"};
            std::cout << "DONE " << command << " observation=" << queries.observation_version()
                      << " topology=" << queries.topology_version() << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
