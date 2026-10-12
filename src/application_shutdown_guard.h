#pragma once

#include "silhouette/active_trace_manager.h"
#include "silhouette/ingestion/otlp_grpc_receiver.h"
#include "silhouette/live_service_map.h"
#include "silhouette/local_map_server.h"

namespace silhouette {

// Construct after all referenced components and before starting any worker.
// Every cleanup operation is harmless before start and idempotent after stop.
class ApplicationShutdownGuard final {
public:
    ApplicationShutdownGuard(ingestion::OtlpGrpcReceiver& receiver,
                             ActiveTraceManager& traces, LiveServiceMap& map,
                             LocalMapServer& server) noexcept
        : receiver_{receiver}, traces_{traces}, map_{map}, server_{server} {}
    ~ApplicationShutdownGuard() { Shutdown(); }

    ApplicationShutdownGuard(const ApplicationShutdownGuard&) = delete;
    ApplicationShutdownGuard& operator=(const ApplicationShutdownGuard&) = delete;

    void Shutdown() noexcept
    {
        receiver_.Shutdown();
        traces_.StopLifecycle();
        map_.CloseAndDrain();
        server_.Shutdown();
    }

private:
    ingestion::OtlpGrpcReceiver& receiver_;
    ActiveTraceManager& traces_;
    LiveServiceMap& map_;
    LocalMapServer& server_;
};

} // namespace silhouette
