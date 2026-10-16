#include "silhouette/trace_provenance_index.h"

#include <stdexcept>

namespace silhouette {
namespace {
template <typename Function>
void ForEachFact(const ServiceGraph& graph, Function function)
{
    for (const auto& service : graph.services) function(TopologyFact{service});
    for (const auto& edge : graph.edges) function(TopologyFact{edge});
    for (const auto& gap : graph.gaps) function(TopologyFact{gap});
}
}

TraceProvenanceIndex::Prepared TraceProvenanceIndex::Prepare(
    std::vector<Contribution> contributions, const std::span<const TraceId> removed)
{
    Prepared prepared;
    prepared.removed.insert(removed.begin(), removed.end());
    for (auto& contribution : contributions) {
        const auto id = contribution.key.trace_id;
        if (prepared.removed.contains(id)) {
            throw std::invalid_argument{"provenance record cannot be present and absent"};
        }
        prepared.replacements.insert_or_assign(id, std::move(contribution));
    }
    const auto remove = [&](const TraceId& id) {
        const auto old = contributions_.find(id);
        if (old != contributions_.end()) {
            ForEachFact(old->second.graph, [&](const TopologyFact& fact) {
                prepared.removals[fact].insert(old->second.key);
            });
        }
    };
    for (const auto& id : prepared.removed) remove(id);
    for (const auto& [id, contribution] : prepared.replacements) {
        remove(id);
        ForEachFact(contribution.graph, [&](const TopologyFact& fact) {
            prepared.additions[fact].insert(contribution.key);
        });
    }
    // Empty placeholders are invisible. Allocate destination nodes now so
    // committing can transfer prepared set/map nodes without allocating.
    for (const auto& [fact, keys] : prepared.additions) {
        static_cast<void>(keys);
        support_.try_emplace(fact);
    }
    return prepared;
}

ServiceGraph TraceProvenanceIndex::Preview(const Prepared& prepared) const
{
    ServiceGraph graph;
    for (const auto& [fact, keys] : support_) {
        const auto removed = prepared.removals.find(fact);
        const auto added = prepared.additions.find(fact);
        const auto count = keys.size()
            - (removed == prepared.removals.end() ? 0 : removed->second.size())
            + (added == prepared.additions.end() ? 0 : added->second.size());
        if (count == 0) continue;
        if (const auto service = std::get_if<std::string>(&fact)) {
            graph.services.push_back(*service);
        } else if (const auto edge = std::get_if<ServiceEdge>(&fact)) {
            graph.edges.push_back(*edge);
        } else {
            graph.gaps.push_back(std::get<ServiceGraphGap>(fact));
        }
    }
    return graph;
}

void TraceProvenanceIndex::Commit(Prepared prepared) noexcept
{
    for (const auto& [fact, keys] : prepared.removals) {
        auto& existing = support_.find(fact)->second;
        for (const auto& key : keys) existing.erase(key);
    }
    for (const auto& id : prepared.removed) contributions_.erase(id);
    for (const auto& [id, contribution] : prepared.replacements) {
        static_cast<void>(contribution);
        contributions_.erase(id);
    }
    for (auto& [fact, keys] : prepared.additions) {
        support_.find(fact)->second.merge(keys);
    }
    contributions_.merge(prepared.replacements);
    for (auto it = support_.begin(); it != support_.end();) {
        if (it->second.empty()) it = support_.erase(it);
        else ++it;
    }
}

const std::set<TraceRecordKey>* TraceProvenanceIndex::Support(const TopologyFact& fact) const
{
    const auto found = support_.find(fact);
    return found == support_.end() ? nullptr : &found->second;
}

void TraceProvenanceIndex::Swap(TraceProvenanceIndex& other) noexcept
{
    contributions_.swap(other.contributions_);
    support_.swap(other.support_);
}

} // namespace silhouette
