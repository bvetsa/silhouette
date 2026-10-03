#include "silhouette/active_trace_manager.h"

#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace silhouette {

static_assert(
    noexcept(std::declval<const TraceId&>() < std::declval<const TraceId&>()));
static_assert(std::is_nothrow_move_constructible_v<std::vector<Span>>);

void ActiveTraceManager::AppendBatch(std::vector<Span> spans)
{
    if (spans.empty()) {
        return;
    }

    std::map<TraceId, SpanBatches> incoming_by_trace;
    for (auto& span : spans) {
        const auto trace_id = span.trace_id;
        auto& batches = incoming_by_trace[trace_id];
        if (batches.empty()) {
            batches.emplace_back();
        }
        batches.front().push_back(std::move(span));
    }

    std::scoped_lock lock{mutex_};

    // Reserve every affected existing trace before changing logical state. A
    // failed allocation can change capacity but cannot publish part of a batch.
    for (const auto& [trace_id, incoming_batches] : incoming_by_trace) {
        const auto existing = batches_by_trace_.find(trace_id);
        if (existing == batches_by_trace_.end()) {
            continue;
        }
        if (incoming_batches.size()
            > existing->second.max_size() - existing->second.size()) {
            throw std::length_error{"active trace has too many span batches"};
        }
        existing->second.reserve(
            existing->second.size() + incoming_batches.size());
    }

    if (before_commit_hook_ != nullptr) {
        before_commit_hook_(before_commit_context_);
    }

    // Existing traces append prepared chunks without allocating. New map nodes
    // were already allocated; merge allocates nothing, and TraceId ordering
    // cannot throw. No historical span is recopied during ingestion.
    for (auto incoming = incoming_by_trace.begin();
         incoming != incoming_by_trace.end();) {
        const auto existing = batches_by_trace_.find(incoming->first);
        if (existing == batches_by_trace_.end()) {
            ++incoming;
            continue;
        }

        for (auto& batch : incoming->second) {
            existing->second.push_back(std::move(batch));
        }
        incoming = incoming_by_trace.erase(incoming);
    }
    batches_by_trace_.merge(incoming_by_trace);
}

std::vector<ReconstructedTrace> ActiveTraceManager::Snapshot() const
{
    return SnapshotWithHook(nullptr, nullptr);
}

std::vector<ReconstructedTrace> ActiveTraceManager::SnapshotWithHook(
    const TestHook before_lock, void* const context) const
{
    if (before_lock != nullptr) {
        before_lock(context);
    }

    std::vector<Span> spans;
    {
        std::scoped_lock lock{mutex_};

        std::size_t span_count = 0;
        for (const auto& [trace_id, trace_batches] : batches_by_trace_) {
            static_cast<void>(trace_id);
            for (const auto& batch : trace_batches) {
                span_count += batch.size();
            }
        }

        spans.reserve(span_count);
        for (const auto& [trace_id, trace_batches] : batches_by_trace_) {
            static_cast<void>(trace_id);
            for (const auto& batch : trace_batches) {
                spans.insert(spans.end(), batch.begin(), batch.end());
            }
        }
    }

    return ReconstructTraces(std::move(spans));
}

std::size_t ActiveTraceManager::active_trace_count() const
{
    std::scoped_lock lock{mutex_};
    return batches_by_trace_.size();
}

} // namespace silhouette
