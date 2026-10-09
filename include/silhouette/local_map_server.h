#pragma once

#include "silhouette/live_service_map.h"

#include <memory>
#include <string>

namespace silhouette {

namespace testing {
class LocalMapServerTestPeer;
}

class LocalMapServer final {
public:
    // live_map must outlive this server.
    LocalMapServer(
        std::string listen_address,
        int port,
        const LiveServiceMap& live_map);
    ~LocalMapServer();

    LocalMapServer(const LocalMapServer&) = delete;
    LocalMapServer& operator=(const LocalMapServer&) = delete;
    LocalMapServer(LocalMapServer&&) = delete;
    LocalMapServer& operator=(LocalMapServer&&) = delete;

    void Start();
    void Shutdown() noexcept;
    [[nodiscard]] int selected_port() const noexcept;

private:
    friend class testing::LocalMapServerTestPeer;

    void UseSingleWorkerForTesting();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace silhouette
