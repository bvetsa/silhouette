#pragma once

#include "silhouette/local_map_server.h"

#include <utility>

namespace silhouette::testing {

class LocalMapServerTestPeer final {
public:
    static void FailWorkerStartup(LocalMapServer& server)
    {
        server.FailWorkerStartupForTesting();
    }

    static void UseTwoWorkers(LocalMapServer& server)
    {
        server.UseTwoWorkersForTesting();
    }

    static void BeforeEnqueue(LocalMapServer& server, std::function<void()> hook)
    {
        server.SetBeforeEnqueueHookForTesting(std::move(hook));
    }

    static void AfterSseRelease(LocalMapServer& server, std::function<void()> hook)
    {
        server.SetAfterSseReleaseHookForTesting(std::move(hook));
    }
};

} // namespace silhouette::testing
