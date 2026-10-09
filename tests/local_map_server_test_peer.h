#pragma once

#include "silhouette/local_map_server.h"

namespace silhouette::testing {

class LocalMapServerTestPeer final {
public:
    static void UseSingleWorker(LocalMapServer& server)
    {
        server.UseSingleWorkerForTesting();
    }
};

} // namespace silhouette::testing
