#include "silhouette/live_service_map.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "live_service_map_test_peer.h"

#include <algorithm>
#include <array>
#include <chrono>
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
#include <thread>
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

    [[nodiscard]] std::vector<std::string> RenderedGraphs() const
    {
        std::scoped_lock lock{mutex_};
        return rendered_graphs_;
    }

private:
    mutable std::mutex mutex_;
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

struct TraceSnapshotSync final {
    explicit TraceSnapshotSync(const std::size_t target)
        : target_call{target}
    {
    }

    std::size_t call_count{0};
    std::size_t target_call;
    std::latch target_started{1};
    std::latch release_target{1};
};

void PauseTargetTraceSnapshot(void* context)
{
    auto& sync = *static_cast<TraceSnapshotSync*>(context);
    ++sync.call_count;
    if (sync.call_count == sync.target_call) {
        sync.target_started.count_down();
        sync.release_target.wait();
    }
}

void DirtyTraceReconstructsNewestAuthoritativeState()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    TraceSnapshotSync sync{1};
    LiveServiceMapTestPeer::SetBeforeTraceSnapshotHook(
        live_map, PauseTargetTraceSnapshot, &sync);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial map was not published");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 2, "checkout", 1),
    }));
    sync.target_started.wait();

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
    }));
    sync.release_target.count_down();
    live_map.CloseAndDrain();

    const auto authoritative = silhouette::BuildServiceGraph(
        active_traces.Snapshot());
    const auto observed = live_map.SnapshotGraph();
    Require(FormatServiceGraph(observed) == FormatServiceGraph(authoritative),
            "dirty trace did not reconstruct its newest retained evidence");
    Require(observed.edges == std::vector<silhouette::ServiceEdge>{
                                  {"frontend", "checkout"}}
                && observed.gaps.empty(),
            "newest contribution retained stale E1-only topology");

    const auto expected = FormatServiceGraph(authoritative);
    const auto rendered = renderer.RenderedGraphs();
    Require(std::all_of(
                rendered.begin() + 1, rendered.end(), [&](const auto& graph) {
                    return graph == expected;
                }),
            "renderer observed the non-authoritative E1-only state");
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
    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 3, "database", 2),
    }));
    const auto recovered = live_map.WaitForPublishedAfter(
        initial->rendered_version);
    Require(recovered.has_value() && recovered->rendered_version == 2,
            "renderer did not recover on the next topology version");

    live_map.CloseAndDrain();
    // Joining the worker guarantees Report() has finished before reading its
    // stream; renderer completion alone precedes diagnostic publication.
    Require(diagnostics.str().find("injected render failure")
                != std::string::npos,
            "render failure was not reported diagnostically");
}

void ThrowAfterDirtyInsert(void*)
{
    throw std::bad_alloc{};
}

void DirtyInsertionFailureFallsBackToFullResync(const bool before_insertion)
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    if (before_insertion) {
        LiveServiceMapTestPeer::SetBeforeDirtyInsertHook(
            live_map, ThrowAfterDirtyInsert, nullptr);
    } else {
        LiveServiceMapTestPeer::SetAfterDirtyInsertHook(
            live_map, ThrowAfterDirtyInsert, nullptr);
    }

    auto affected = active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
        MakeSpan(1, 2, "checkout", 1),
        MakeSpan(2, 1, "checkout"),
        MakeSpan(2, 2, "database", 1),
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

void FirstDirtyInsertionFailureFallsBackToFullResync()
{
    DirtyInsertionFailureFallsBackToFullResync(true);
}

void PartialDirtyMergeFailureFallsBackToFullResync()
{
    DirtyInsertionFailureFallsBackToFullResync(false);
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

void ShutdownDrainsPendingCoalescedWork()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::latch stale_render_started{1};
    std::latch release_stale_render{1};
    renderer.BlockCall(2, stale_render_started, release_stale_render);
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    TraceSnapshotSync graph_sync{3};
    LiveServiceMapTestPeer::SetBeforeTraceSnapshotHook(
        live_map, PauseTargetTraceSnapshot, &graph_sync);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial map was not published before shutdown");

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

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(3, 1, "database"),
        MakeSpan(3, 2, "cache", 1),
    }));
    graph_sync.target_started.wait();

    std::thread shutdown{[&] { live_map.CloseAndDrain(); }};
    LiveServiceMapTestPeer::WaitForInvalidationInputClosed(live_map);
    graph_sync.release_target.count_down();
    LiveServiceMapTestPeer::WaitForTopologyVersion(live_map, 3);
    release_stale_render.count_down();
    shutdown.join();

    const auto final = live_map.SnapshotPublished();
    Require(final.has_value() && final->rendered_version == 3,
            "shutdown published a stale SVG instead of the newest topology");
    Require(final->svg.find("frontend -> checkout") != std::string::npos
                && final->svg.find("checkout -> database")
                    != std::string::npos
                && final->svg.find("database -> cache") != std::string::npos,
            "shutdown lost pending coalesced topology work");
    Require(FormatServiceGraph(live_map.SnapshotGraph())
                == FormatServiceGraph(silhouette::BuildServiceGraph(
                    active_traces.Snapshot())),
            "shutdown final topology differed from authoritative trace state");
}

struct TopologyUpdateSync final {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t completed{0};

    static void Signal(void* context)
    {
        auto& sync = *static_cast<TopologyUpdateSync*>(context);
        {
            std::scoped_lock lock{sync.mutex};
            ++sync.completed;
        }
        sync.cv.notify_one();
    }

    void WaitFor(const std::size_t count)
    {
        std::unique_lock lock{mutex};
        cv.wait(lock, [&] { return completed >= count; });
    }
};

void EquivalentTelemetryRetriesFailedCurrentRender()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    renderer.FailCall(2);
    std::ostringstream diagnostics;
    TopologyUpdateSync idle_sync;
    TopologyUpdateSync graph_sync;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LiveServiceMapTestPeer::SetBeforeRenderWaitHook(
        live_map, TopologyUpdateSync::Signal, &idle_sync);
    LiveServiceMapTestPeer::SetAfterTopologyUpdateHook(
        live_map, TopologyUpdateSync::Signal, &graph_sync);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial && initial->rendered_version == 0,
            "initial SVG was not published before the injected failure");
    idle_sync.WaitFor(1);

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"), MakeSpan(1, 2, "checkout", 1),
    }));
    // This hook fires only once the failed attempt is consumed and the worker
    // has no pending render request. No elapsed-time guess establishes idleness.
    idle_sync.WaitFor(2);
    const auto failed = live_map.SnapshotPublished();
    Require(live_map.topology_version() == 1 && failed
                && failed->rendered_version == 0 && failed->svg == initial->svg
                && renderer.RenderedGraphs().size() == 2,
            "failed render replaced the last SVG or retried without an event");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(2, 1, "frontend"), MakeSpan(2, 2, "checkout", 1),
    }));
    graph_sync.WaitFor(2);
    // The same publication wait used by SSE must observe the retried version.
    // The timeout is only a failure bound, not a concurrency-ordering mechanism.
    const auto recovered = live_map.WaitForPublishedAfter(
        0, std::chrono::seconds{5});
    live_map.CloseAndDrain();
    Require(recovered && recovered->rendered_version == 1
                && recovered->has_telemetry && live_map.topology_version() == 1,
            "equivalent telemetry did not publish a retry of version one");
    Require(renderer.RenderedGraphs().size() == 3,
            "failed current render was not retried exactly once");
    const auto retained = active_traces.Snapshot();
    Require(retained.size() == 2
                && retained[0].spans.size() + retained[1].spans.size() == 4
                && live_map.SnapshotGraph()
                    == silhouette::BuildServiceGraph(retained),
            "render retry lost equivalent evidence or changed topology");
    Require(diagnostics.str().find("injected render failure")
                != std::string::npos,
            "render failure was not reported before recovery");
}

void EquivalentTelemetryDuringSuccessfulRenderDoesNotRenderAgain()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::latch render_started{1};
    std::latch release_render{1};
    renderer.BlockCall(2, render_started, release_render);
    std::ostringstream diagnostics;
    TopologyUpdateSync graph_sync;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LiveServiceMapTestPeer::SetAfterTopologyUpdateHook(
        live_map, TopologyUpdateSync::Signal, &graph_sync);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial map was not published");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"), MakeSpan(1, 2, "checkout", 1),
    }));
    render_started.wait();
    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(2, 1, "frontend"), MakeSpan(2, 2, "checkout", 1),
    }));
    graph_sync.WaitFor(2);
    release_render.count_down();
    const auto published = live_map.WaitForPublishedAfter(0);
    live_map.CloseAndDrain();
    Require(published && published->rendered_version == 1
                && live_map.topology_version() == 1
                && renderer.RenderedGraphs().size() == 2,
            "equivalent event caused a duplicate successfully rendered frame");
}

void FailedInitialRenderRetriesVersionZero()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    renderer.FailCall(1);
    std::ostringstream diagnostics;
    TopologyUpdateSync idle_sync;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LiveServiceMapTestPeer::SetBeforeRenderWaitHook(
        live_map, TopologyUpdateSync::Signal, &idle_sync);
    live_map.Start();
    idle_sync.WaitFor(1);
    Require(!live_map.SnapshotPublished()
                && renderer.RenderedGraphs().size() == 1,
            "initial failure published an SVG or retried without an event");

    // Exercise the empty presentation boundary without changing topology.
    LiveServiceMapTestPeer::PublishTopology(live_map, {}, false);
    const auto recovered = live_map.WaitForPublishedAfter(
        std::nullopt, std::chrono::seconds{5});
    live_map.CloseAndDrain();
    Require(recovered && recovered->rendered_version == 0
                && !recovered->has_telemetry && live_map.topology_version() == 0
                && renderer.RenderedGraphs().size() == 2,
            "version zero was mistaken for an already successful render");
}

void EquivalentEvidenceSuppressesRendersAndPreservesContributions()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    TopologyUpdateSync sync;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    LiveServiceMapTestPeer::SetAfterTopologyUpdateHook(
        live_map, TopologyUpdateSync::Signal, &sync);
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial.has_value(), "initial map was not published");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"), MakeSpan(1, 2, "checkout", 1),
    }));
    sync.WaitFor(1);
    const auto first = live_map.WaitForPublishedAfter(0);
    Require(first && first->rendered_version == 1,
            "first telemetry did not publish version one");

    // A second contributor to the same edge must be retained without rendering.
    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(2, 1, "frontend"), MakeSpan(2, 2, "checkout", 1),
    }));
    sync.WaitFor(2);
    Require(live_map.topology_version() == 1
                && renderer.RenderedGraphs().size() == 2,
            "equivalent trace advanced the version or rendered again");

    // Same-service evidence updates the trace but leaves topology unchanged.
    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 3, "checkout", 2),
    }));
    sync.WaitFor(3);
    Require(live_map.topology_version() == 1
                && renderer.RenderedGraphs().size() == 2,
            "same-service evidence advanced the version or rendered again");
    const auto retained = active_traces.Snapshot();
    Require(retained.size() == 2
                && retained[0].spans.size() + retained[1].spans.size() == 5,
            "suppression dropped accepted evidence");
    Require(live_map.SnapshotGraph() == silhouette::BuildServiceGraph(retained),
            "suppressed topology differed from authoritative evidence");

    // Ambiguity removes trace one's edge contribution; trace two still owns it.
    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(1, 1, "frontend"),
    }));
    sync.WaitFor(4);
    const auto revised = live_map.WaitForPublishedAfter(1);
    Require(revised && revised->rendered_version == 2,
            "new gap after suppression did not render version two");
    Require(live_map.SnapshotGraph().edges
                == std::vector<silhouette::ServiceEdge>{{"frontend", "checkout"}},
            "suppressed contributor did not preserve its edge reference");

    live_map.NotifyTracesDirty(active_traces.AppendBatch({
        MakeSpan(2, 1, "frontend"),
    }));
    sync.WaitFor(5);
    const auto final = live_map.WaitForPublishedAfter(2);
    Require(final && final->rendered_version == 3,
            "last edge removal did not render version three");
    live_map.CloseAndDrain();
    Require(live_map.SnapshotGraph().edges.empty()
                && live_map.SnapshotGraph()
                    == silhouette::BuildServiceGraph(active_traces.Snapshot()),
            "final incremental graph retained stale contribution counts");
    Require(renderer.RenderedGraphs().size() == 4,
            "suppressed updates caused extra renders");
}

void TelemetryFlagChangesEmptyPresentationState()
{
    ActiveTraceManager active_traces;
    RecordingRenderer renderer;
    std::ostringstream diagnostics;
    LiveServiceMap live_map{active_traces, renderer, diagnostics};
    live_map.Start();
    const auto initial = live_map.WaitForPublishedAfter(std::nullopt);
    Require(initial && !initial->has_telemetry,
            "initial map incorrectly claimed telemetry");

    // Exercise the presentation boundary directly: a retained real span normally
    // contributes either a service or a gap, even without service identity.
    LiveServiceMapTestPeer::PublishTopology(live_map, {}, true);
    const auto updated = live_map.WaitForPublishedAfter(0);
    Require(updated && updated->rendered_version == 1 && updated->has_telemetry,
            "telemetry flag transition with empty graph was suppressed");
    LiveServiceMapTestPeer::PublishTopology(live_map, {}, true);
    live_map.CloseAndDrain();
    Require(live_map.topology_version() == 1
                && renderer.RenderedGraphs().size() == 2,
            "unchanged telemetry flag and empty graph rendered again");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"equivalent_telemetry_retries_failed_current_render",
             EquivalentTelemetryRetriesFailedCurrentRender},
    TestCase{"equivalent_telemetry_during_successful_render_does_not_render_again",
             EquivalentTelemetryDuringSuccessfulRenderDoesNotRenderAgain},
    TestCase{"failed_initial_render_retries_version_zero",
             FailedInitialRenderRetriesVersionZero},
    TestCase{"equivalent_evidence_suppresses_renders_and_preserves_contributions",
             EquivalentEvidenceSuppressesRendersAndPreservesContributions},
    TestCase{"telemetry_flag_changes_empty_presentation_state",
             TelemetryFlagChangesEmptyPresentationState},
    TestCase{"initial_empty_map_is_published_and_restart_is_rejected",
             InitialEmptyMapIsPublishedAndRestartIsRejected},
    TestCase{"dirty_trace_updates_publish_latest_topology",
             DirtyTraceUpdatesPublishLatestTopology},
    TestCase{"dirty_trace_reconstructs_newest_authoritative_state",
             DirtyTraceReconstructsNewestAuthoritativeState},
    TestCase{"stale_render_is_discarded_in_favor_of_newest_version",
             StaleRenderIsDiscardedInFavorOfNewestVersion},
    TestCase{"render_failure_preserves_last_published_svg",
             RenderFailurePreservesLastPublishedSvg},
    TestCase{"partial_dirty_merge_failure_falls_back_to_full_resync",
             PartialDirtyMergeFailureFallsBackToFullResync},
    TestCase{"first_dirty_insertion_failure_falls_back_to_full_resync",
             FirstDirtyInsertionFailureFallsBackToFullResync},
    TestCase{"shutdown_drains_final_dirty_topology_and_render",
             ShutdownDrainsFinalDirtyTopologyAndRender},
    TestCase{"shutdown_drains_pending_coalesced_work",
             ShutdownDrainsPendingCoalescedWork},
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
