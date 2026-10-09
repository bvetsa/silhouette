#include "silhouette/live_service_map.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "live_service_map_test_peer.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using silhouette::ActiveTraceManager;
using silhouette::FormatServiceGraph;
using silhouette::LiveServiceMap;
using silhouette::ServiceGraph;
using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;
using silhouette::rendering::ServiceGraphRenderer;
using silhouette::rendering::SvgRenderResult;
using silhouette::testing::LiveServiceMapTestPeer;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

TraceId MakeTraceId(const std::uint8_t marker)
{
    TraceId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    return *TraceId::FromBytes(bytes);
}

SpanId MakeSpanId(const std::uint8_t marker)
{
    SpanId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    return *SpanId::FromBytes(bytes);
}

Span MakeSpan(
    const std::uint8_t trace_marker,
    const std::uint8_t span_marker,
    std::string service,
    const std::optional<std::uint8_t> parent_marker = std::nullopt)
{
    return Span{
        .trace_id = MakeTraceId(trace_marker),
        .span_id = MakeSpanId(span_marker),
        .parent_span_id = parent_marker
            ? std::optional<SpanId>{MakeSpanId(*parent_marker)}
            : std::nullopt,
        .service_name = std::move(service),
        .operation_name = "operation",
        .start_time_unix_nano = span_marker,
        .end_time_unix_nano = static_cast<std::uint64_t>(span_marker) + 1,
    };
}

class RecordingRenderer final : public ServiceGraphRenderer {
public:
    void BlockCall(
        const std::size_t call,
        std::latch& started,
        std::latch& release)
    {
        blocked_call_ = call;
        blocked_started_ = &started;
        blocked_release_ = &release;
    }

    void FailCall(const std::size_t call) { failed_calls_.insert(call); }

    SvgRenderResult RenderSvg(const ServiceGraph& graph) override
    {
        std::size_t call = 0;
        bool should_fail = false;
        std::latch* started = nullptr;
        std::latch* release = nullptr;
        const auto graph_text = FormatServiceGraph(graph);
        {
            std::scoped_lock lock{mutex_};
            call = ++started_calls_;
            rendered_graphs_.push_back(graph_text);
            should_fail = failed_calls_.contains(call);
            if (blocked_call_ == call) {
                started = blocked_started_;
                release = blocked_release_;
            }
        }

        if (started) {
            started->count_down();
            release->wait();
        }

        {
            std::scoped_lock lock{mutex_};
            completed_calls_ = std::max(completed_calls_, call);
        }
        completed_cv_.notify_all();

        if (should_fail) {
            return {.svg = {}, .error = "injected render failure"};
        }
        return {.svg = "<svg data-call=\"" + std::to_string(call)
                    + "\"><!--" + graph_text + "--></svg>",
                .error = {}};
    }

    void WaitForCompleted(const std::size_t call)
    {
        std::unique_lock lock{mutex_};
        completed_cv_.wait(lock, [&] { return completed_calls_ >= call; });
    }

private:
    std::mutex mutex_;
    std::condition_variable completed_cv_;
    std::size_t started_calls_{0};
    std::size_t completed_calls_{0};
    std::size_t blocked_call_{0};
    std::latch* blocked_started_{nullptr};
    std::latch* blocked_release_{nullptr};
    std::set<std::size_t> failed_calls_;
    std::vector<std::string> rendered_graphs_;
};

void InitialEmptyMapIsPublishedAndRestartIsRejected()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};

    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial empty live map was not published");
    Require(initial->rendered_version == 0 && !initial->has_telemetry,
            "initial live map reported the wrong version or telemetry state");
    Require(initial->svg.find("<svg") != std::string::npos,
            "initial live map did not own rendered SVG bytes");

    live_map.CloseAndDrain();
    live_map.CloseAndDrain();
    bool rejected = false;
    try {
        live_map.Start();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Require(rejected, "stopped live-map workers restarted");
}

void DirtyTraceUpdatesPublishLatestTopology()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);

    auto affected = active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
    });
    live_map.NotifyTracesDirty(std::move(affected));

    const auto updated = live_map.WaitForPublishedAfter(
        initial->rendered_version);
    Require(updated.has_value() && updated->has_telemetry,
            "accepted trace did not publish a telemetry-bearing map");
    Require(updated->rendered_version == live_map.topology_version(),
            "rendered and topology versions did not converge");
    Require(FormatServiceGraph(live_map.SnapshotGraph())
                == FormatServiceGraph(silhouette::BuildServiceGraph(
                    active_traces.Snapshot())),
            "targeted live topology differed from authoritative trace state");

    live_map.CloseAndDrain();
}

void StaleRenderIsDiscardedInFavorOfNewestVersion()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::latch stale_render_started{1};
    std::latch release_stale_render{1};
    renderer.BlockCall(2, stale_render_started, release_stale_render);
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
    }));
    stale_render_started.wait();

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(2, 1, "checkout"),
        MakeSpan(2, 2, "database", 1),
    }));
    LiveServiceMapTestPeer::WaitForTopologyVersion(live_map, 2);
    release_stale_render.count_down();

    const auto newest = live_map.WaitForPublishedAfter(
        initial->rendered_version);
    Require(newest.has_value() && newest->rendered_version == 2,
            "obsolete SVG version was published instead of being discarded");
    Require(newest->svg.find("frontend -> checkout") != std::string::npos
                && newest->svg.find("checkout -> database")
                    != std::string::npos,
            "newest SVG omitted a completed topology update");

    live_map.CloseAndDrain();
}

void RenderFailurePreservesLastPublishedSvg()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    renderer.FailCall(2);
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
    }));
    renderer.WaitForCompleted(2);

    const auto after_failure = live_map.SnapshotPublished();
    Require(after_failure.has_value()
                && after_failure->rendered_version
                    == initial->rendered_version
                && after_failure->svg == initial->svg,
            "failed render replaced the last valid published SVG");
    Require(diagnostics.str().find("injected render failure")
                != std::string::npos,
            "render failure was not reported diagnostically");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 3, "database", 2),
    }));
    const auto recovered = live_map.WaitForPublishedAfter(
        initial->rendered_version);
    Require(recovered.has_value() && recovered->rendered_version == 2,
            "renderer did not recover on the next topology version");

    live_map.CloseAndDrain();
}

void ThrowAllocationFailure(void*)
{
    throw std::bad_alloc{};
}

void DirtyMergeFailureFallsBackToFullResync()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    LiveServiceMapTestPeer::SetBeforeDirtyMergeHook(
        live_map, ThrowAllocationFailure, nullptr);

    auto affected = active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
    });
    live_map.NotifyTracesDirty(std::move(affected));

    const auto recovered = live_map.WaitForPublishedAfter(
        initial->rendered_version);
    Require(recovered.has_value() && recovered->has_telemetry,
            "notification allocation failure did not trigger full resync");
    Require(FormatServiceGraph(live_map.SnapshotGraph())
                == FormatServiceGraph(silhouette::BuildServiceGraph(
                    active_traces.Snapshot())),
            "full resync did not restore authoritative topology");

    live_map.CloseAndDrain();
}

void ShutdownDrainsFinalDirtyTopologyAndRender()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial map was not published before shutdown");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
    }));
    live_map.CloseAndDrain();

    const auto final = live_map.SnapshotPublished();
    Require(final.has_value() && final->rendered_version == 1
                && final->has_telemetry,
            "shutdown did not drain the final topology and render work");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"initial_empty_map_is_published_and_restart_is_rejected",
             InitialEmptyMapIsPublishedAndRestartIsRejected},
    TestCase{"dirty_trace_updates_publish_latest_topology",
             DirtyTraceUpdatesPublishLatestTopology},
    TestCase{"stale_render_is_discarded_in_favor_of_newest_version",
             StaleRenderIsDiscardedInFavorOfNewestVersion},
    TestCase{"render_failure_preserves_last_published_svg",
             RenderFailurePreservesLastPublishedSvg},
    TestCase{"dirty_merge_failure_falls_back_to_full_resync",
             DirtyMergeFailureFallsBackToFullResync},
    TestCase{"shutdown_drains_final_dirty_topology_and_render",
             ShutdownDrainsFinalDirtyTopologyAndRender},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one live-map test-case name.\n";
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

    std::cerr << "Unknown live-map test case: " << requested_case << '\n';
    return 1;
}
