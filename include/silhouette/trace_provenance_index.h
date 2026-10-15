#pragma once

#include "silhouette/service_graph.h"
#include "silhouette/trace_record.h"

#include <map>
#include <set>
#include <variant>

namespace silhouette {

using TopologyFact = std::variant<std::string, ServiceEdge, ServiceGraphGap>;

// Serialized by the query service's publication mutex. Separate from the
// canonical service graph: changing support need not change topology.
class TraceProvenanceIndex final {
public:
    struct Contribution final {
        TraceRecordKey key;
        ServiceGraph graph;
    };
    struct Prepared final {
        std::map<TraceId, Contribution> replacements;
        std::set<TraceId> removed;
        std::map<TopologyFact, std::set<TraceRecordKey>> additions;
        std::map<TopologyFact, std::set<TraceRecordKey>> removals;
    };

    [[nodiscard]] Prepared Prepare(std::vector<Contribution> contributions,
                                   std::span<const TraceId> removed);
    [[nodiscard]] ServiceGraph Preview(const Prepared& prepared) const;
    void Commit(Prepared prepared) noexcept;
    [[nodiscard]] const std::set<TraceRecordKey>* Support(const TopologyFact& fact) const;
    void Swap(TraceProvenanceIndex& other) noexcept;

private:
    std::map<TraceId, Contribution> contributions_;
    std::map<TopologyFact, std::set<TraceRecordKey>> support_;
};

} // namespace silhouette
