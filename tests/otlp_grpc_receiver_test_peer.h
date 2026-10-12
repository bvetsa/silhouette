#pragma once

#include "silhouette/ingestion/otlp_grpc_receiver.h"

namespace silhouette::testing {

class OtlpGrpcReceiverTestPeer final {
public:
    // Configure before receiver startup; context outlives its handlers.
    static void SetBeforeResponsePrepareHook(
        ingestion::OtlpGrpcReceiver& receiver, void (*hook)(void*), void* context)
    {
        receiver.before_response_prepare_hook_ = hook;
        receiver.before_response_prepare_context_ = context;
    }
};

} // namespace silhouette::testing
