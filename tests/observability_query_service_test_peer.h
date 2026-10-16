#pragma once

#include "silhouette/observability_query_service.h"

namespace silhouette::testing {
class ObservabilityQueryServiceTestPeer final {
public:
    static void AfterReconcile(ObservabilityQueryService& queries,
                               void (*hook)(void*), void* context)
    { queries.after_topology_update_hook_ = hook; queries.after_topology_update_context_ = context; }
    static void Reconcile(ObservabilityQueryService& queries,
                          std::span<const TraceId> ids = {}, bool full = true,
                          bool evidence = false)
    { queries.Reconcile(ids, full, evidence); }
    static void BeforeProvenance(ObservabilityQueryService& queries,
                                 void (*hook)(void*), void* context = nullptr)
    { queries.before_provenance_prepare_hook_ = hook; queries.before_provenance_prepare_context_ = context; }
    static void BeforeGraph(ObservabilityQueryService& queries,
                            void (*hook)(void*), void* context = nullptr)
    { queries.before_graph_commit_hook_ = hook; queries.before_graph_commit_context_ = context; }
};
}
