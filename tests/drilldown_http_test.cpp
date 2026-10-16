#include "silhouette/local_map_server.h"
#include "silhouette/rendering/topology_fact_key.h"
#include "observability_query_service_test_peer.h"
#include "trace_lifecycle_test_support.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <latch>
#include <limits>
#include <sstream>
#include <thread>

namespace {
using namespace silhouette;
using namespace std::chrono_literals;
using Json = nlohmann::json;
using testing::FakeTraceClock;
using testing::LifecycleSweepSync;
using testing::ActiveTraceManagerTestPeer;
using testing::ObservabilityQueryServiceTestPeer;
void Require(bool condition, std::string_view message)
{ if (!condition) throw std::runtime_error{std::string{message}}; }
Span Evidence(unsigned marker, std::string service)
{
    TraceId::Bytes id{}; id.back() = std::byte(marker);
    SpanId::Bytes sid{}; sid.back() = std::byte{1};
    return {*TraceId::FromBytes(id), *SpanId::FromBytes(sid), {}, std::move(service),
        "operation", std::numeric_limits<std::uint64_t>::max() - 1,
        std::numeric_limits<std::uint64_t>::max()};
}
class Renderer final : public rendering::ServiceGraphRenderer {
public:
    std::atomic<unsigned> calls{0};
    std::latch* started{nullptr};
    std::latch* release{nullptr};
    rendering::SvgRenderResult RenderSvg(const ServiceGraph&) override
    {
        if (++calls == 1 && started) { started->count_down(); release->wait(); }
        return {"<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>", {}};
    }
};
struct Fixture final {
    FakeTraceClock clock;
    ActiveTraceManager manager{1s, TraceRetentionLimits{1000, 10s, 10000}, FakeTraceClock::Now, &clock};
    std::ostringstream diagnostics;
    Renderer renderer;
    ObservabilityQueryService queries{manager, diagnostics};
    LiveServiceMap map{queries, renderer, diagnostics};
    LocalMapServer server{"127.0.0.1", 0, map, queries};
    LifecycleSweepSync changes, sweeps;
    std::size_t expected{0};
    Fixture()
    {
        ObservabilityQueryServiceTestPeer::AfterReconcile(queries, LifecycleSweepSync::Signal, &changes);
        ActiveTraceManagerTestPeer::SetAfterSweepHook(manager, LifecycleSweepSync::Signal, &sweeps);
    }
    ~Fixture() { manager.StopLifecycle(); queries.CloseAndDrain(); map.CloseAndDrain(); server.Shutdown(); }
    void Start()
    { queries.Start(); manager.StartLifecycle(&queries, &queries); map.Start(); server.Start(); }
    void Append(std::vector<Span> spans)
    {
        auto admitted = manager.AppendBatch(std::move(spans));
        queries.NotifyTracesDirty(std::move(admitted.affected_trace_ids)); changes.WaitFor(++expected);
    }
    void Advance(std::chrono::steady_clock::duration amount)
    {
        sweeps.WaitFor(testing::AdvanceAndWakeLifecycle(manager, clock, sweeps, amount));
        changes.WaitFor(++expected);
    }
    httplib::Client Client()
    {
        httplib::Client client{"127.0.0.1", server.selected_port()};
        client.set_read_timeout(5, 0); return client;
    }
    std::string Path(const TraceRecordKey& key)
    { return "/query/trace/" + key.trace_id.ToHex() + '/' + std::to_string(key.incarnation)
        + "?instance=" + queries.observation_instance_id().value; }
};

void FocusedQueriesAndTransportErrors()
{
    Fixture f; f.Start();
    const std::string service{"checkout \"<script>\\雪"};
    auto span = Evidence(1, service);
    f.Append({span, span});
    auto client = f.Client();
    auto response = client.Post("/query/support", Json{{"factKey", rendering::EncodeTopologyFact(service)}}.dump(), "application/json");
    Require(response && response->status == 200, "support query failed");
    const auto page = Json::parse(response->body);
    Require(page["traces"].size() == 1 && page["nextCursor"].is_null()
        && page["traces"][0]["incarnation"].is_string()
        && page["factLabel"] == service, "support JSON changed identity or string content");
    const auto key = f.manager.SnapshotRecords()[0].metadata.key;
    auto detail = client.Get(f.Path(key));
    Require(detail && detail->status == 200, "exact detail query failed");
    const auto data = Json::parse(detail->body);
    Require(data["spans"].size() == 2 && data["spans"][0]["service"] == service
        && data["spans"][0]["parentSpanId"].is_null()
        && data["spans"][0]["duplicateSpanId"] == true
        && data["spans"][0]["endTimeUnixNano"] == "18446744073709551615",
        "detail lost duplicates, scalars, or 64-bit timestamp precision");
    Require(client.Post("/query/support", "{}", "application/json")->status == 400
        && client.Post("/query/support", "not-json", "application/json")->status == 400,
        "malformed support query was not rejected");
    auto wrong = key; ++wrong.incarnation;
    Require(client.Get(f.Path(wrong))->status == 404, "detail resolved a different incarnation");
    Require(client.Get(f.Path(key) + "-wrong")->status == 404, "detail ignored observation instance");
    Require(client.Get("/query/trace/bad/1?instance=x")->status == 400, "malformed identity was not rejected");
    auto missing = client.Post("/query/support", Json{{"factKey", rendering::EncodeTopologyFact(std::string{"absent"})}}.dump(), "application/json");
    Require(Json::parse(missing->body)["traces"].empty(), "unknown fact invented supporting traces");
    f.Advance(10s);
    Require(client.Get(f.Path(key))->status == 404, "evicted incarnation remained queryable");
    f.Append({Evidence(1, "reappeared")});
    Require(client.Get(f.Path(key))->status == 404, "new incarnation replaced old detail");
    auto invalid = Evidence(2, "valid"); invalid.operation_name = std::string(1, static_cast<char>(0xff));
    f.Append({invalid});
    const auto invalid_key = f.manager.LookupRecords(std::span{&invalid.trace_id, std::size_t{1}})[0].metadata->key;
    Require(client.Get(f.Path(invalid_key))->status == 500, "serialization error was reported as missing evidence");
    Require(f.queries.QueryTrace(invalid_key).has_value(), "presentation error discarded engine evidence");
}

void ObservationEventsDoNotWaitForGraphviz()
{
    Fixture f;
    std::latch render_started{1}, release_render{1}, observed{1};
    f.renderer.started = &render_started; f.renderer.release = &release_render;
    f.Start(); render_started.wait();
    std::string events;
    std::thread reader{[&] {
        auto client = f.Client();
        client.Get("/events", [&](const char* data, std::size_t size) {
            events.append(data, size);
            if (events.find("\"observationVersion\":\"1\"") != std::string::npos) {
                observed.count_down(); return false;
            }
            return true;
        });
    }};
    f.Append({Evidence(1, "a")});
    observed.wait(); reader.join();
    Require(!f.map.SnapshotPublished(), "blocked renderer unexpectedly published SVG");
    f.Advance(1s);
    auto state = f.map.WaitForVersionsAfter({}, 1, 0ms);
    Require(state.observation_version == 2 && !state.rendered_version,
            "lifecycle invalidation waited for SVG publication");
    release_render.count_down();
    const auto published = f.map.WaitForPublishedAfter({}, 5s);
    Require(published && published->rendered_version == 1 && f.renderer.calls == 2,
            "quiescence caused an extra layout or lost latest graph");
}

void HttpRestartPreservesQueryIdentity()
{
    Fixture f; f.Start(); f.Append({Evidence(1, "a")});
    const auto instance = f.queries.observation_instance_id();
    const auto key = f.manager.SnapshotRecords()[0].metadata.key;
    f.server.Shutdown();
    LocalMapServer replacement{"127.0.0.1", 0, f.map, f.queries}; replacement.Start();
    httplib::Client client{"127.0.0.1", replacement.selected_port()};
    const auto detail = client.Get(f.Path(key));
    Require(detail && detail->status == 200 && Json::parse(detail->body)["instance"] == instance.value,
            "HTTP restart invalidated a retained engine record");
    replacement.Shutdown();
    ObservabilityQueryService new_queries{f.manager, f.diagnostics};
    Require(new_queries.observation_instance_id() != instance,
            "new query service reused observation identity");
}

void LargeFlatHttpDetail()
{
    Fixture f; f.Start();
    auto span = Evidence(1, "a"); std::vector<Span> spans;
    for (unsigned i = 1; i <= 10000; ++i) {
        SpanId::Bytes bytes{}; bytes[6] = std::byte(i >> 8); bytes[7] = std::byte(i & 255);
        span.span_id = *SpanId::FromBytes(bytes);
        spans.push_back(span); span.parent_span_id = span.span_id;
    }
    f.Append(std::move(spans));
    const auto key = f.manager.SnapshotRecords()[0].metadata.key;
    const auto result = f.Client().Get(f.Path(key));
    Require(result && result->status == 200, "large HTTP detail failed: "
        + (result ? std::to_string(result->status) + " " + result->body : httplib::to_string(result.error())));
    const auto data = Json::parse(result->body);
    Require(data["spans"].size() == 10000 && data["topLevelIndices"].size() == 1,
            "large trace was truncated or recursively misserialized");
}
}

int main(int argc, char** argv)
{
    const std::pair<std::string_view, void(*)()> cases[]{
        {"focused_queries_and_transport_errors", FocusedQueriesAndTransportErrors},
        {"observation_events_do_not_wait_for_graphviz", ObservationEventsDoNotWaitForGraphviz},
        {"http_restart_preserves_query_identity", HttpRestartPreservesQueryIdentity},
        {"large_flat_http_detail", LargeFlatHttpDetail}};
    try {
        for (const auto& [name, test] : cases) if (argc == 2 && name == argv[1]) { test(); return 0; }
        throw std::runtime_error{"unknown test case"};
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
