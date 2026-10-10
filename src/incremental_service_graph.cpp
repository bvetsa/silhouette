#include "silhouette/incremental_service_graph.h"

#include <cassert>
#include <map>
#include <utility>

namespace silhouette {
namespace {

ServiceGraph BuildContribution(const ReconstructedTrace& trace)
{
    return BuildServiceGraph(std::span{&trace, std::size_t{1}});
}

template <typename Key>
void Increment(std::map<Key, std::size_t>& counts, const Key& key)
{
    ++counts.find(key)->second;
}

template <typename Key>
void Decrement(std::map<Key, std::size_t>& counts, const Key& key)
{
    auto found = counts.find(key);
    assert(found != counts.end() && found->second > 0);
    --found->second;
}

template <typename Key>
void RemoveZeroCounts(std::map<Key, std::size_t>& counts)
{
    for (auto current = counts.begin(); current != counts.end();) {
        if (current->second == 0) {
            current = counts.erase(current);
        } else {
            ++current;
        }
    }
}

void AddContribution(
    std::map<std::string, std::size_t>& services,
    std::map<ServiceEdge, std::size_t>& edges,
    std::map<ServiceGraphGap, std::size_t>& gaps,
    const ServiceGraph& contribution)
{
    for (const auto& service : contribution.services) {
        ++services[service];
    }
    for (const auto& edge : contribution.edges) {
        ++edges[edge];
    }
    for (const auto& gap : contribution.gaps) {
        ++gaps[gap];
    }
}

} // namespace

void IncrementalServiceGraph::UpdateTraces(
    const std::span<const ReconstructedTrace> traces)
{
    std::map<TraceId, ServiceGraph> prepared;
    for (const auto& trace : traces) {
        prepared.insert_or_assign(trace.trace_id, BuildContribution(trace));
    }
    if (prepared.empty()) {
        return;
    }

    std::scoped_lock lock{mutex_};

    // Prepare every allocation before changing any observable reference count.
    // A failed allocation may leave empty contributions or zero-count entries,
    // but Snapshot() deliberately excludes that bookkeeping state.
    for (const auto& [trace_id, contribution] : prepared) {
        state_.contributions.try_emplace(trace_id);
        for (const auto& service : contribution.services) {
            state_.service_refcounts.try_emplace(service, 0);
        }
        for (const auto& edge : contribution.edges) {
            state_.edge_refcounts.try_emplace(edge, 0);
        }
        for (const auto& gap : contribution.gaps) {
            state_.gap_refcounts.try_emplace(gap, 0);
        }
    }

    if (before_commit_hook_) {
        before_commit_hook_(before_commit_context_);
    }

    for (const auto& [trace_id, contribution] : prepared) {
        const auto& previous = state_.contributions.find(trace_id)->second;
        for (const auto& service : previous.services) {
            Decrement(state_.service_refcounts, service);
        }
        for (const auto& edge : previous.edges) {
            Decrement(state_.edge_refcounts, edge);
        }
        for (const auto& gap : previous.gaps) {
            Decrement(state_.gap_refcounts, gap);
        }
    }

    for (auto& [trace_id, contribution] : prepared) {
        for (const auto& service : contribution.services) {
            Increment(state_.service_refcounts, service);
        }
        for (const auto& edge : contribution.edges) {
            Increment(state_.edge_refcounts, edge);
        }
        for (const auto& gap : contribution.gaps) {
            Increment(state_.gap_refcounts, gap);
        }
        state_.contributions.find(trace_id)->second = std::move(contribution);
    }

    RemoveZeroCounts(state_.service_refcounts);
    RemoveZeroCounts(state_.edge_refcounts);
    RemoveZeroCounts(state_.gap_refcounts);
}

void IncrementalServiceGraph::ReplaceAll(
    const std::span<const ReconstructedTrace> traces)
{
    State replacement;
    for (const auto& trace : traces) {
        replacement.contributions.insert_or_assign(
            trace.trace_id, BuildContribution(trace));
    }
    for (const auto& [trace_id, contribution] : replacement.contributions) {
        static_cast<void>(trace_id);
        AddContribution(
            replacement.service_refcounts,
            replacement.edge_refcounts,
            replacement.gap_refcounts,
            contribution);
    }

    std::scoped_lock lock{mutex_};
    state_.contributions.swap(replacement.contributions);
    state_.service_refcounts.swap(replacement.service_refcounts);
    state_.edge_refcounts.swap(replacement.edge_refcounts);
    state_.gap_refcounts.swap(replacement.gap_refcounts);
}

ServiceGraph IncrementalServiceGraph::Snapshot() const
{
    std::scoped_lock lock{mutex_};
    ServiceGraph graph;
    graph.services.reserve(state_.service_refcounts.size());
    graph.edges.reserve(state_.edge_refcounts.size());
    graph.gaps.reserve(state_.gap_refcounts.size());

    for (const auto& [service, count] : state_.service_refcounts) {
        if (count > 0) {
            graph.services.push_back(service);
        }
    }
    for (const auto& [edge, count] : state_.edge_refcounts) {
        if (count > 0) {
            graph.edges.push_back(edge);
        }
    }
    for (const auto& [gap, count] : state_.gap_refcounts) {
        if (count > 0) {
            graph.gaps.push_back(gap);
        }
    }
    return graph;
}

} // namespace silhouette
