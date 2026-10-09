#pragma once

#include "silhouette/live_service_map.h"

namespace silhouette::testing {

class LiveServiceMapTestPeer final {
public:
    using TestHook = void (*)(void*);

    static void SetBeforeDirtyInsertHook(
        LiveServiceMap& live_map, TestHook hook, void* context)
    {
        live_map.before_dirty_insert_hook_ = hook;
        live_map.before_dirty_insert_context_ = context;
    }

    static void SetAfterDirtyInsertHook(
        LiveServiceMap& live_map, TestHook hook, void* context)
    {
        live_map.after_dirty_insert_hook_ = hook;
        live_map.after_dirty_insert_context_ = context;
    }

    static void SetBeforeTraceSnapshotHook(
        LiveServiceMap& live_map, TestHook hook, void* context)
    {
        live_map.before_trace_snapshot_hook_ = hook;
        live_map.before_trace_snapshot_context_ = context;
    }

    static void WaitForTopologyVersion(
        LiveServiceMap& live_map, const std::uint64_t version)
    {
        std::unique_lock lock{live_map.topology_mutex_};
        live_map.topology_cv_.wait(lock, [&] {
            return live_map.topology_version_ >= version;
        });
    }

    static void WaitForInvalidationInputClosed(LiveServiceMap& live_map)
    {
        std::unique_lock lock{live_map.dirty_mutex_};
        live_map.dirty_cv_.wait(lock, [&] {
            return live_map.invalidation_input_closed_;
        });
    }
};

} // namespace silhouette::testing
