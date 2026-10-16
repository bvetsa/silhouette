#include "silhouette/observability_query_service.h"
#include "observability_query_service_test_peer.h"
#include "trace_lifecycle_test_support.h"

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace silhouette;
using namespace std::chrono_literals;
using silhouette::testing::ActiveTraceManagerTestPeer;
using silhouette::testing::ObservabilityQueryServiceTestPeer;
using silhouette::testing::FakeTraceClock;
using silhouette::testing::LifecycleSweepSync;

void Require(bool condition, std::string_view message)
{ if (!condition) throw std::runtime_error{std::string{message}}; }

TraceId Id(unsigned value)
{
    TraceId::Bytes bytes{};
    bytes[14] = std::byte(value >> 8); bytes[15] = std::byte(value & 255);
    return *TraceId::FromBytes(bytes);
}
SpanId Sid(unsigned value)
{
    SpanId::Bytes bytes{};
    bytes[6] = std::byte(value >> 8); bytes[7] = std::byte(value & 255);
    return *SpanId::FromBytes(bytes);
}
Span Evidence(unsigned trace, unsigned span, std::string service,
              std::optional<unsigned> parent = {})
{
    return {Id(trace), Sid(span), parent ? std::optional{Sid(*parent)} : std::nullopt,
            std::move(service), "operation", 1, 2};
}

struct Fixture final {
    FakeTraceClock clock;
    ActiveTraceManager manager{1s, FakeTraceClock::Now, &clock};
    std::ostringstream diagnostics;
    ObservabilityQueryService queries{manager, diagnostics};
    void Append(std::vector<Span> spans)
    {
        auto result = manager.AppendBatch(std::move(spans));
        ObservabilityQueryServiceTestPeer::Reconcile(queries, result.affected_trace_ids, false, true);
    }
};

void MetadataPublicationAndOwnedRecovery()
{
    FakeTraceClock clock;
    ActiveTraceManager manager{1s, TraceRetentionLimits{1000, 10min, 3}, FakeTraceClock::Now, &clock};
    manager.AppendBatch({Evidence(1, 1, "a"), Evidence(2, 1, "b")});
    const auto snapshot = manager.SnapshotRecords();
    Require(snapshot.size() == 2 && snapshot[0].metadata.publication_sequence == 1
        && snapshot[1].metadata.publication_sequence == 1, "one batch needs one sequence");
    Require(snapshot[0].metadata.key.incarnation != snapshot[1].metadata.key.incarnation,
            "new records reused an incarnation");
    clock.Advance(5s);
    manager.AppendBatch({Evidence(1, 1, "a")});
    const auto duplicate = manager.LookupRecord(snapshot[0].metadata.key);
    Require(duplicate && duplicate->metadata.publication_sequence == 2
        && duplicate->metadata.evidence_age == 0s && duplicate->evidence.size() == 2,
        "duplicate did not preserve identity and refresh revision");
    manager.AppendBatch({});
    manager.AppendBatch({Evidence(1, 3, "a"), Evidence(1, 4, "a")});
    manager.AppendBatch({Evidence(3, 1, "c")});
    Require(manager.SnapshotRecords().back().metadata.publication_sequence == 3,
            "empty or fully rejected publication consumed a sequence");
    Require(snapshot[0].evidence.size() == 1 && snapshot[0].metadata.evidence_age == 0s,
            "owned full snapshot changed after later publication");
    clock.Advance(4s);
    const auto samples = clock.samples();
    auto records = manager.SnapshotRecords();
    Require(clock.samples() == samples + 1 && records[0].metadata.evidence_age == 4s
        && records[1].metadata.evidence_age == 9s, "metadata used different or real clocks");
    const std::array revisions{TraceRecordRevision{records[0].metadata.key, 2}};
    const std::array ids{Id(1), Id(99)};
    const auto lookups = manager.LookupRecords(ids, revisions);
    Require(lookups[0].metadata && !lookups[0].evidence && !lookups[1].metadata,
            "targeted metadata lookup lost unchanged/absent distinction");
}

void ExactSupportAndLateParent()
{
    Fixture f;
    f.Append({Evidence(1, 2, "checkout", 1), Evidence(2, 1, "checkout")});
    auto initial = f.queries.SnapshotGraph();
    const auto gap = initial.gaps.front();
    Require(f.queries.QuerySupport(std::string{"checkout"}).traces.size() == 2,
            "service support omitted retained record");
    Require(f.queries.QuerySupport(gap).traces.size() == 1, "exact gap support missing");
    const auto version = f.queries.observation_version();
    f.Append({Evidence(1, 1, "frontend")});
    Require(f.queries.QuerySupport(gap).traces.empty(), "late parent retained stale gap support");
    Require(f.queries.QuerySupport(ServiceEdge{"frontend", "checkout"}).traces.size() == 1,
            "resolved edge support missing");
    Require(f.queries.observation_version() > version, "evidence failed to invalidate detail");
    const auto key = f.queries.QuerySupport(std::string{"frontend"}).traces[0].metadata.key;
    auto detail = f.queries.QueryTrace(key);
    Require(detail && detail->trace.spans.size() == 2 && detail->graph.edges.size() == 1,
            "detail failed to use authoritative reconstruction");
}

void ExhaustionDoesNotPartiallyPublish()
{
    ActiveTraceManager manager;
    manager.AppendBatch({Evidence(1, 1, "a")});
    const auto original = manager.SnapshotRecords().front();
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ActiveTraceManagerTestPeer::SetRecordCounters(manager, maximum, 1);
    bool rejected = false;
    try { manager.AppendBatch({Evidence(1, 2, "a"), Evidence(2, 1, "b")}); }
    catch (const std::overflow_error&) { rejected = true; }
    const auto after = manager.SnapshotRecords();
    Require(rejected && after.size() == 1 && after[0].evidence == original.evidence
        && after[0].metadata.publication_sequence == 1,
        "incarnation exhaustion partially published a fitting sibling");
    manager.AppendBatch({Evidence(1, 2, "a")});
    Require(manager.SnapshotRecords()[0].metadata.publication_sequence == 2,
            "failed creation consumed publication sequence");
    ActiveTraceManagerTestPeer::SetRecordCounters(manager, maximum, maximum);
    rejected = false;
    try { manager.AppendBatch({Evidence(1, 3, "a")}); }
    catch (const std::overflow_error&) { rejected = true; }
    Require(rejected && manager.SnapshotRecords()[0].evidence.size() == 2,
            "publication sequence wrapped or committed evidence on failure");
}

void DelayedLifecycleInvalidationRechecksCurrentState()
{
    Fixture f;
    f.Append({Evidence(1, 1, "a")});
    const auto key = f.manager.SnapshotRecords()[0].metadata.key;
    f.clock.Advance(1s);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(f.manager));
    f.Append({Evidence(1, 2, "a")});
    const auto version = f.queries.observation_version();
    const std::array ids{Id(1)};
    // Process the old quiescence notice after the reopening publication.
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries, ids, false, false);
    const auto detail = f.queries.QueryTrace(key);
    Require(detail && detail->metadata.lifecycle == TraceLifecycle::active
        && detail->trace.spans.size() == 2 && f.queries.observation_version() == version,
        "stale quiescence notice overwrote current lifecycle or invalidated a no-op");
}

void VersionsAndFakeAges()
{
    Fixture f;
    f.Append({Evidence(1, 1, "a")});
    const auto topology = f.queries.topology_version();
    const auto version = f.queries.observation_version();
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries);
    const std::array ids{Id(1)};
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries, ids, false);
    Require(f.queries.observation_version() == version, "no-op reconciliation incremented version");
    f.clock.Advance(2s);
    auto summary = f.queries.QuerySupport(std::string{"a"});
    Require(summary.traces.front().metadata.evidence_age == 2s
        && f.queries.observation_version() == version, "query age depended on publication or real time");
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(f.manager));
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries, ids, false);
    Require(f.queries.observation_version() == version + 1
        && f.queries.topology_version() == topology, "quiescence changed topology or not observation");
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries);
    Require(f.queries.observation_version() == version + 1, "identical recovery invalidated state");
    f.Append({Evidence(1, 1, "a")});
    Require(f.queries.observation_version() == version + 2
        && f.queries.topology_version() == topology, "duplicate evidence failed to refresh observation only");
}

void ReappearanceAndStaleSupport()
{
    Fixture f;
    f.Append({Evidence(1, 1, "old")});
    const auto old = f.manager.SnapshotRecords()[0].metadata.key;
    f.clock.Advance(10min);
    static_cast<void>(ActiveTraceManagerTestPeer::SweepLifecycle(f.manager));
    Require(!f.queries.QueryTrace(old) && f.queries.QuerySupport(std::string{"old"}).traces.empty(),
            "query served evicted identity before asynchronous reconciliation");
    f.manager.AppendBatch({Evidence(1, 2, "new")});
    Require(f.queries.QuerySupport(std::string{"old"}).traces.empty(), "index aliased new incarnation");
    const std::array ids{Id(1)};
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries, ids, false, true);
    const auto current = f.queries.QuerySupport(std::string{"new"}).traces[0].metadata.key;
    Require(current != old && !f.queries.QueryTrace(old), "eviction reused record identity");
    Require(f.queries.QueryTrace(current).has_value(), "delayed eviction deleted new incarnation");
}

void LivePagination()
{
    Fixture f;
    std::vector<Span> spans;
    for (unsigned i = 1; i <= 51; ++i) spans.push_back(Evidence(i, 1, "shared"));
    f.Append(std::move(spans));
    auto first = f.queries.QuerySupport(std::string{"shared"});
    Require(first.traces.size() == 50 && first.next_cursor, "first page boundary incorrect");
    auto last = f.queries.QuerySupport(std::string{"shared"}, first.next_cursor);
    Require(last.traces.size() == 1 && !last.next_cursor
        && last.traces[0].metadata.key.trace_id == Id(51), "tie-break or continuation incorrect");
    f.Append({Evidence(51, 2, "shared")});
    Require(f.queries.QuerySupport(std::string{"shared"}, first.next_cursor).traces.empty(),
            "cursor pinned a historical page");
    Require(f.queries.QuerySupport(std::string{"shared"}).traces[0].metadata.key.trace_id == Id(51),
            "refresh did not return newest accepted evidence first");
    auto wrong = *first.next_cursor;
    wrong.instance.value = "another-instance";
    bool rejected = false;
    try { static_cast<void>(f.queries.QuerySupport(std::string{"shared"}, wrong)); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "cursor survived observation instance change");
}

void Throw(void*) { throw std::bad_alloc{}; }
void AtomicPreparationAndFullRecovery()
{
    Fixture f;
    f.Append({Evidence(1, 2, "child", 1)});
    const auto graph = f.queries.SnapshotGraph();
    const auto version = f.queries.observation_version();
    f.manager.AppendBatch({Evidence(1, 1, "parent")});
    const std::array ids{Id(1)};
    for (bool graph_failure : {false, true}) {
        if (graph_failure) ObservabilityQueryServiceTestPeer::BeforeGraph(f.queries, Throw);
        else ObservabilityQueryServiceTestPeer::BeforeProvenance(f.queries, Throw);
        bool failed = false;
        try { ObservabilityQueryServiceTestPeer::Reconcile(f.queries, ids, false, true); }
        catch (const std::bad_alloc&) { failed = true; }
        Require(failed && f.queries.SnapshotGraph() == graph
            && f.queries.observation_version() == version
            && f.queries.QuerySupport(std::string{"parent"}).traces.empty(),
            "failed preparation published half of graph/provenance/metadata");
        ObservabilityQueryServiceTestPeer::BeforeGraph(f.queries, nullptr);
        ObservabilityQueryServiceTestPeer::BeforeProvenance(f.queries, nullptr);
    }
    ObservabilityQueryServiceTestPeer::Reconcile(f.queries);
    Require(f.queries.SnapshotGraph().gaps.empty()
        && f.queries.QuerySupport(ServiceEdge{"parent", "child"}).traces.size() == 1,
        "full metadata recovery failed to restore exact support");
}

class LifecycleRecorder final : public TraceLifecycleSink, public TraceEvictionSink {
public:
    void NotifyTracesQuiesced(std::vector<TraceId> ids) noexcept override { quiesced = std::move(ids); }
    void NotifyTracesEvicted(std::vector<TraceId> ids) noexcept override { evicted = std::move(ids); }
    std::vector<TraceId> quiesced, evicted;
};
void SweepNotifiesOnlyQuiescentSurvivors()
{
    for (const std::size_t capacity : {0, 1}) {
        FakeTraceClock clock;
        ActiveTraceManager manager{1s, TraceRetentionLimits{capacity, 10s, 100}, FakeTraceClock::Now, &clock};
        manager.AppendBatch({Evidence(1, 1, "a")});
        clock.Advance(1s);
        LifecycleRecorder notifications;
        LifecycleSweepSync sweeps;
        ActiveTraceManagerTestPeer::SetAfterSweepHook(manager, LifecycleSweepSync::Signal, &sweeps);
        manager.StartLifecycle(&notifications, &notifications);
        sweeps.WaitFor(1);
        manager.StopLifecycle();
        Require(notifications.quiesced.size() == capacity
            && notifications.evicted.size() == 1 - capacity,
            "same-sweep eviction also published a quiescence transition");
    }
}

void LargeDeepDetailPreservesEvidence()
{
    Fixture f;
    std::vector<Span> spans;
    for (unsigned i = 1; i <= 10000; ++i) spans.push_back(Evidence(1, i, "a", i == 1
        ? std::optional<unsigned>{} : i - 1));
    f.Append(std::move(spans));
    auto key = f.manager.SnapshotRecords()[0].metadata.key;
    const auto detail = f.queries.QueryTrace(key);
    Require(detail && detail->trace.spans.size() == 10000
        && detail->trace.top_level_indices.size() == 1, "large detail was truncated");
}
}

int main(int argc, char** argv)
{
    const std::pair<std::string_view, void(*)()> cases[]{
        {"metadata_publication_and_owned_recovery", MetadataPublicationAndOwnedRecovery},
        {"exhaustion_does_not_partially_publish", ExhaustionDoesNotPartiallyPublish},
        {"delayed_lifecycle_invalidation_rechecks_current_state", DelayedLifecycleInvalidationRechecksCurrentState},
        {"exact_support_and_late_parent", ExactSupportAndLateParent},
        {"versions_and_fake_ages", VersionsAndFakeAges},
        {"reappearance_and_stale_support", ReappearanceAndStaleSupport},
        {"live_pagination", LivePagination},
        {"atomic_preparation_and_full_recovery", AtomicPreparationAndFullRecovery},
        {"sweep_notifies_only_quiescent_survivors", SweepNotifiesOnlyQuiescentSurvivors},
        {"large_deep_detail_preserves_evidence", LargeDeepDetailPreservesEvidence},
    };
    try {
        for (const auto& [name, test] : cases) {
            if (argc == 2 && name == argv[1]) { test(); return 0; }
        }
        throw std::runtime_error{"unknown test case"};
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
