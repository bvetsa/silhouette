add_executable(otlp_grpc_receiver_test
    otlp_grpc_receiver_test.cpp
)
target_compile_features(otlp_grpc_receiver_test PRIVATE cxx_std_20)
set_target_properties(otlp_grpc_receiver_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(
    otlp_grpc_receiver_test
    PRIVATE
        silhouette_ingestion
        silhouette_otlp_proto
)
silhouette_enable_warnings(otlp_grpc_receiver_test)

add_test(
    NAME otlp_grpc_receiver
    COMMAND otlp_grpc_receiver_test
)
