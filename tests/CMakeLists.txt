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

function(add_otlp_grpc_receiver_test test_case)
    add_test(
        NAME otlp_grpc_receiver_${test_case}
        COMMAND otlp_grpc_receiver_test ${test_case}
    )
endfunction()

add_otlp_grpc_receiver_test(rejects_empty_address)
add_otlp_grpc_receiver_test(starts_on_ephemeral_port)
add_otlp_grpc_receiver_test(acknowledges_empty_export)
add_otlp_grpc_receiver_test(successful_export_omits_partial_success)
add_otlp_grpc_receiver_test(counts_export_request)
add_otlp_grpc_receiver_test(empty_export_counts_zero_spans)
add_otlp_grpc_receiver_test(counts_nested_spans)
add_otlp_grpc_receiver_test(rejects_second_start)
add_otlp_grpc_receiver_test(allows_shutdown_before_start)
add_otlp_grpc_receiver_test(allows_repeated_shutdown)
add_otlp_grpc_receiver_test(rejects_restart_after_shutdown)
add_otlp_grpc_receiver_test(destructor_releases_port)
add_otlp_grpc_receiver_test(rejects_duplicate_bind)
