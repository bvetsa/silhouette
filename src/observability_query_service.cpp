#include "silhouette/observability_query_service.h"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>
#include "diagnostics_mutex.h"
#include <utility>

namespace silhouette {
namespace {

ObservationInstanceId NewInstance()
{
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const auto byte = random() & 0xff;
        value.push_back(hex[byte >> 4]);
        value.push_back(hex[byte & 15]);
    }
    return {std::move(value)};
}

bool SameSummary(const TraceSummary& left, const TraceSummary& right)
{
    const auto& a = left.metadata;
    const auto& b = right.metadata;
    // Age changes as time passes; it is calculated afresh by the manager and
    // deliberately is not a read-model publication or invalidation.
    return a.key == b.key && a.lifecycle == b.lifecycle
        && a.publication_sequence == b.publication_sequence
        && a.last_evidence_time == b.last_evidence_time
        && a.span_count == b.span_count && left.service_count == right.service_count;
}

bool Newer(const TraceSummary& a, const TraceSummary& b)
{
    if (a.metadata.publication_sequence != b.metadata.publication_sequence) {
        return a.metadata.publication_sequence > b.metadata.publication_sequence;
    }
    return a.metadata.key < b.metadata.key;
}

} // namespace

ObservabilityQueryService::ObservabilityQueryService(
    ActiveTraceManager& traces, std::ostream& diagnostics)
    : traces_{traces}, diagnostics_{diagnostics}, instance_{NewInstance()}
{
}

ObservabilityQueryService::~ObservabilityQueryService() { CloseAndDrain(); }

void ObservabilityQueryService::SetChangeSink(QueryChangeSink* sink)
{
    if (started_) throw std::logic_error{"query sink must be set before start"};
    sink_ = sink;
}

void ObservabilityQueryService::Start()
{
    if (started_) throw std::logic_error{"query service can only be started once"};
    if (traces_.retained_trace_count() != 0) resync_requests_.fetch_add(1);
    worker_ = std::thread{[this] { Worker(); }};
    started_ = true;
}

void ObservabilityQueryService::CloseAndDrain() noexcept
{
    if (!started_ || closed_) return;
    {
        std::scoped_lock lock{dirty_mutex_};
        invalidation_input_closed_ = true;
    }
    dirty_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    closed_ = true;
}

void ObservabilityQueryService::Notify(std::vector<TraceId> ids, const bool evidence) noexcept
{
    std::scoped_lock lock{dirty_mutex_};
    if (invalidation_input_closed_) return;
    evidence_pending_ |= evidence;
    try {
        for (const auto& id : ids) {
            if (before_dirty_insert_hook_) before_dirty_insert_hook_(before_dirty_insert_context_);
            dirty_trace_ids_.insert(id);
            if (after_dirty_insert_hook_) after_dirty_insert_hook_(after_dirty_insert_context_);
        }
    } catch (...) {
        resync_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    dirty_cv_.notify_all();
}

void ObservabilityQueryService::NotifyTracesDirty(std::vector<TraceId> ids) noexcept
{ Notify(std::move(ids), true); }
void ObservabilityQueryService::NotifyTracesEvicted(std::vector<TraceId> ids) noexcept
{ Notify(std::move(ids), true); }
void ObservabilityQueryService::NotifyTracesQuiesced(std::vector<TraceId> ids) noexcept
{ Notify(std::move(ids), false); }

void ObservabilityQueryService::Worker()
{
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    for (;;) {
        std::set<TraceId> dirty;
        bool full;
        bool evidence;
        std::uint64_t target;
        {
            std::unique_lock lock{dirty_mutex_};
            dirty_cv_.wait(lock, [&] {
                return invalidation_input_closed_ || !dirty_trace_ids_.empty()
                    || resync_requests_.load() > std::max(completed, failed);
            });
            target = resync_requests_.load();
            full = target > completed;
            if (invalidation_input_closed_ && dirty_trace_ids_.empty() && !full) break;
            dirty.swap(dirty_trace_ids_);
            evidence = std::exchange(evidence_pending_, false);
        }
        try {
            const std::vector<TraceId> ids{dirty.begin(), dirty.end()};
            Reconcile(ids, full, evidence);
            if (full) { completed = target; failed = 0; }
            if (after_topology_update_hook_) after_topology_update_hook_(after_topology_update_context_);
        } catch (const std::exception& error) {
            if (full) failed = target;
            else resync_requests_.fetch_add(1);
            Report("read-model reconciliation failed", error.what());
        } catch (...) {
            if (full) failed = target;
            else resync_requests_.fetch_add(1);
            Report("read-model reconciliation failed", "unknown error");
        }
        if (full && failed == target) {
            std::scoped_lock lock{dirty_mutex_};
            if (invalidation_input_closed_) break;
        }
    }
}

void ObservabilityQueryService::Reconcile(
    const std::span<const TraceId> ids, const bool full, bool evidence)
{
    std::vector<TraceRecordLookup> lookups;
    if (before_trace_snapshot_hook_) before_trace_snapshot_hook_(before_trace_snapshot_context_);
    if (full) {
        auto snapshots = traces_.SnapshotRecords();
        lookups.reserve(snapshots.size());
        for (auto& record : snapshots) {
            lookups.push_back({record.metadata.key.trace_id, record.metadata, std::move(record.evidence)});
        }
    } else {
        std::vector<TraceRecordRevision> known;
        {
            std::scoped_lock lock{publication_mutex_};
            known.reserve(ids.size());
            for (const auto& id : ids) {
                const auto found = records_.find(id);
                if (found != records_.end()) {
                    known.push_back({found->second.metadata.key,
                                     found->second.metadata.publication_sequence});
                }
            }
        }
        lookups = traces_.LookupRecords(ids, known);
    }
    if (after_trace_lookup_hook_) after_trace_lookup_hook_(after_trace_lookup_context_);

    std::vector<ReconstructedTrace> traces;
    std::vector<TraceProvenanceIndex::Contribution> contributions;
    std::map<TraceId, TraceSummary> replacements;
    std::vector<TraceId> absent;
    for (auto& lookup : lookups) {
        if (!lookup.metadata) { absent.push_back(lookup.trace_id); continue; }
        if (lookup.evidence) {
            auto reconstructed = ReconstructTraces(std::move(*lookup.evidence));
            auto graph = BuildServiceGraph(reconstructed);
            replacements.emplace(lookup.trace_id, TraceSummary{*lookup.metadata, graph.services.size()});
            contributions.push_back({lookup.metadata->key, std::move(graph)});
            traces.push_back(std::move(reconstructed.front()));
        } else {
            std::scoped_lock lock{publication_mutex_};
            replacements.emplace(lookup.trace_id,
                TraceSummary{*lookup.metadata, records_.at(lookup.trace_id).service_count});
        }
    }

    TraceProvenanceIndex replacement_index;
    // Full recovery is independently prepared before touching published state.
    if (full) {
        auto prepared = replacement_index.Prepare(std::move(contributions), {});
        replacement_index.Commit(std::move(prepared));
    }

    QueryChange change{};
    {
        std::scoped_lock lock{publication_mutex_};
        bool changed = full && replacements.size() != records_.size();
        for (const auto& [id, record] : replacements) {
            const auto old = records_.find(id);
            changed |= old == records_.end() || !SameSummary(old->second, record);
            evidence |= old == records_.end()
                || old->second.metadata.key != record.metadata.key
                || old->second.metadata.publication_sequence != record.metadata.publication_sequence;
        }
        for (const auto& id : absent) changed |= records_.contains(id);
        if (full && replacements.size() != records_.size()) evidence = true;
        if (before_provenance_prepare_hook_) before_provenance_prepare_hook_(before_provenance_prepare_context_);
        auto prepared = full ? TraceProvenanceIndex::Prepared{}
                             : provenance_.Prepare(std::move(contributions), absent);
        auto graph = full ? replacement_index.Preview({}) : provenance_.Preview(prepared);
        std::size_t retained = full ? replacements.size() : records_.size();
        if (!full) {
            for (const auto& [id, record] : replacements) {
                static_cast<void>(record);
                retained += !records_.contains(id);
            }
            for (const auto& id : absent) retained -= records_.contains(id);
        }
        const bool has_telemetry = retained != 0;
        const bool topology_changed = graph != graph_.Snapshot()
            || has_telemetry != !records_.empty();
        changed |= topology_changed;
        if ((changed && observation_version_ == std::numeric_limits<std::uint64_t>::max())
            || (topology_changed && topology_version_ == std::numeric_limits<std::uint64_t>::max())) {
            throw std::overflow_error{"query version exhausted"};
        }
        change = {topology_version_ + topology_changed,
                  observation_version_ + changed, has_telemetry, std::nullopt};
        if (evidence || topology_changed) change.topology = std::move(graph);
        if (before_graph_commit_hook_) before_graph_commit_hook_(before_graph_commit_context_);
        // This is the last fallible operation. Its own preparation/commit
        // guarantee protects graph state; all remaining publications transfer
        // already allocated nodes under this outer publication mutex.
        if (full) graph_.ReplaceAll(traces);
        else if (!traces.empty() || !absent.empty()) graph_.ReconcileTraces(traces, absent);
        if (full) {
            provenance_.Swap(replacement_index);
            records_.swap(replacements);
        } else {
            provenance_.Commit(std::move(prepared));
            for (const auto& id : absent) records_.erase(id);
            for (const auto& [id, record] : replacements) {
                static_cast<void>(record);
                records_.erase(id);
            }
            records_.merge(replacements);
        }
        topology_version_ = change.topology_version;
        observation_version_ = change.observation_version;
    }
    if (sink_) sink_->NotifyQueryChanged(std::move(change));
}

SupportPage ObservabilityQueryService::QuerySupport(
    const TopologyFact& fact, const std::optional<SupportCursor>& cursor) const
{
    if (cursor && cursor->instance != instance_) {
        throw std::invalid_argument{"support cursor belongs to another observation instance"};
    }
    std::vector<TraceSummary> candidates;
    std::vector<TraceId> ids;
    std::uint64_t version;
    {
        std::scoped_lock lock{publication_mutex_};
        version = observation_version_;
        if (const auto keys = provenance_.Support(fact)) {
            for (const auto& key : *keys) {
                candidates.push_back(records_.at(key.trace_id));
                ids.push_back(key.trace_id);
            }
        }
    }
    const auto current = traces_.LookupRecords(ids, {}, false);
    std::map<TraceId, TraceRecordMetadata> metadata;
    for (const auto& lookup : current) {
        if (lookup.metadata) metadata.emplace(lookup.trace_id, *lookup.metadata);
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](TraceSummary& record) {
        const auto found = metadata.find(record.metadata.key.trace_id);
        if (found == metadata.end() || found->second.key != record.metadata.key
            || found->second.publication_sequence != record.metadata.publication_sequence) return true;
        record.metadata = found->second;
        if (!cursor) return false;
        return record.metadata.publication_sequence > cursor->publication_sequence
            || (record.metadata.publication_sequence == cursor->publication_sequence
                && record.metadata.key <= cursor->key);
    }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), Newer);
    SupportPage page{instance_, version, {}, std::nullopt};
    if (candidates.size() > 50) {
        const auto& last = candidates[49].metadata;
        page.next_cursor = SupportCursor{instance_, last.publication_sequence, last.key};
        candidates.erase(candidates.begin() + 50, candidates.end());
    }
    page.traces = std::move(candidates);
    return page;
}

std::optional<TraceDetail> ObservabilityQueryService::QueryTrace(const TraceRecordKey& key) const
{
    auto record = traces_.LookupRecord(key);
    if (!record) return std::nullopt;
    auto reconstructed = ReconstructTraces(std::move(record->evidence));
    auto graph = BuildServiceGraph(reconstructed);
    return TraceDetail{instance_, observation_version(), record->metadata,
                       std::move(reconstructed.front()), std::move(graph)};
}

ServiceGraph ObservabilityQueryService::SnapshotGraph() const
{ std::scoped_lock lock{publication_mutex_}; return graph_.Snapshot(); }
std::uint64_t ObservabilityQueryService::topology_version() const
{ std::scoped_lock lock{publication_mutex_}; return topology_version_; }
std::uint64_t ObservabilityQueryService::observation_version() const
{ std::scoped_lock lock{publication_mutex_}; return observation_version_; }
const ObservationInstanceId& ObservabilityQueryService::observation_instance_id() const noexcept
{ return instance_; }

void ObservabilityQueryService::Report(std::string_view message, std::string_view detail) noexcept
{
    try { std::scoped_lock lock{detail::DiagnosticsMutex()}; diagnostics_ << "Silhouette query service: " << message << ": " << detail << '\n'; }
    catch (...) {}
}

} // namespace silhouette
