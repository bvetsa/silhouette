#pragma once

#include "silhouette/live_service_map.h"

namespace silhouette::testing {

class LiveServiceMapTestPeer final {
public:
    using TestHook = void (*)(void*);

    static void SetBeforeDirtyMergeHook(
        LiveServiceMap& live_map, TestHook hook, void* context)
    {
        live_map.before_dirty_merge_hook_ = hook;
        live_map.before_dirty_merge_context_ = context;
    }

    static void WaitForTopologyVersion(
        LiveServiceMap& live_map, const std::uint64_t version)
    {
        std::unique_lock lock{live_map.topology_mutex_};
        live_map.topology_cv_.wait(lock, [&] {
            return live_map.topology_version_ >= version;
        });
    }
};

} // namespace silhouette::testing
