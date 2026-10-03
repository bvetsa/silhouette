find_package(Threads REQUIRED)

add_executable(span_core_test span_core_test.cpp)
target_compile_features(span_core_test PRIVATE cxx_std_20)
set_target_properties(span_core_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(span_core_test PRIVATE silhouette_core)
silhouette_enable_warnings(span_core_test)

function(add_span_core_test test_case)
    add_test(NAME span_core_${test_case} COMMAND span_core_test ${test_case})
endfunction()

add_span_core_test(valid_ids_allow_individual_zero_bytes)
add_span_core_test(invalid_ids_are_rejected)
add_span_core_test(span_preserves_domain_fields)

add_executable(active_trace_manager_test active_trace_manager_test.cpp)
target_compile_features(active_trace_manager_test PRIVATE cxx_std_20)
set_target_properties(active_trace_manager_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(
    active_trace_manager_test
    PRIVATE silhouette_core Threads::Threads
)
silhouette_enable_warnings(active_trace_manager_test)

function(add_active_trace_manager_test test_case)
    add_test(
        NAME active_trace_manager_${test_case}
        COMMAND active_trace_manager_test ${test_case}
    )
endfunction()

add_active_trace_manager_test(one_trace_receives_one_batch)
add_active_trace_manager_test(one_trace_receives_multiple_batches)
add_active_trace_manager_test(multiple_interleaved_traces_remain_independent)
add_active_trace_manager_test(child_before_parent_reconstructs_after_later_batch)
add_active_trace_manager_test(duplicate_span_ids_are_preserved)
add_active_trace_manager_test(missing_parent_evidence_is_preserved)
add_active_trace_manager_test(active_trace_count_tracks_distinct_ids)
add_active_trace_manager_test(equivalent_arrival_orders_produce_stable_snapshots)
add_active_trace_manager_test(returned_snapshots_remain_owned_and_unchanged)
add_active_trace_manager_test(concurrent_append_batches_lose_no_spans)
add_active_trace_manager_test(concurrent_batches_publish_atomically)

add_executable(trace_reconstruction_test trace_reconstruction_test.cpp)
target_compile_features(trace_reconstruction_test PRIVATE cxx_std_20)
set_target_properties(trace_reconstruction_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(trace_reconstruction_test PRIVATE silhouette_core)
silhouette_enable_warnings(trace_reconstruction_test)

function(add_trace_reconstruction_test test_case)
    add_test(
        NAME trace_reconstruction_${test_case}
        COMMAND trace_reconstruction_test ${test_case}
    )
endfunction()

add_trace_reconstruction_test(id_ordering_and_hex_formatting_are_canonical)
add_trace_reconstruction_test(empty_and_single_root_formatting_is_readable)
add_trace_reconstruction_test(out_of_order_branching_reconstructs_by_identity)
add_trace_reconstruction_test(interleaved_traces_remain_independent_and_ordered)
add_trace_reconstruction_test(missing_parents_and_multiple_roots_remain_top_level)
add_trace_reconstruction_test(duplicate_ids_retain_spans_and_make_children_ambiguous)
add_trace_reconstruction_test(cycles_detach_only_cycle_edges_and_preserve_children)
add_trace_reconstruction_test(unresolved_edges_do_not_participate_in_cycle_detection)
add_trace_reconstruction_test(optional_tie_breakers_have_explicit_ordering)
add_trace_reconstruction_test(output_is_stable_across_input_permutations)

add_executable(service_graph_test service_graph_test.cpp)
target_compile_features(service_graph_test PRIVATE cxx_std_20)
set_target_properties(service_graph_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(service_graph_test PRIVATE silhouette_core)
silhouette_enable_warnings(service_graph_test)

function(add_service_graph_test test_case)
    add_test(
        NAME service_graph_${test_case}
        COMMAND service_graph_test ${test_case}
    )
endfunction()

add_service_graph_test(empty_graph_formats_all_sections)
add_service_graph_test(known_services_and_cross_service_edges_are_aggregated)
add_service_graph_test(repeated_and_same_service_relationships_are_collapsed)
add_service_graph_test(isolated_services_and_components_are_retained)
add_service_graph_test(duplicate_ids_follow_reconstruction_decisions)
add_service_graph_test(missing_service_gaps_preserve_known_endpoints)
add_service_graph_test(endpointless_missing_service_is_not_redundant)
add_service_graph_test(parent_resolution_gaps_preserve_reasons)
add_service_graph_test(cycle_gaps_keep_source_unknown_and_preserve_children)
add_service_graph_test(output_is_stable_across_input_permutations)

add_executable(service_graph_rendering_test service_graph_rendering_test.cpp)
target_compile_features(service_graph_rendering_test PRIVATE cxx_std_20)
set_target_properties(service_graph_rendering_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(service_graph_rendering_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(service_graph_rendering_test PRIVATE silhouette_rendering)
silhouette_enable_warnings(service_graph_rendering_test)

function(add_service_graph_rendering_test test_case)
    add_test(
        NAME service_graph_rendering_${test_case}
        COMMAND service_graph_rendering_test ${test_case}
    )
endfunction()

add_service_graph_rendering_test(empty_graph_has_exact_legend)
add_service_graph_rendering_test(known_topology_and_isolated_services)
add_service_graph_rendering_test(service_labels_are_escaped)
add_service_graph_rendering_test(missing_service_endpoint_combinations)
add_service_graph_rendering_test(parent_gaps_have_distinct_shapes_and_colors)
add_service_graph_rendering_test(gap_connections_use_only_stored_endpoints)
add_service_graph_rendering_test(output_is_stable_across_capture_permutations)
add_service_graph_rendering_test(real_graphviz_produces_svg)
add_service_graph_rendering_test(dot_write_failure_has_file_context)
if(UNIX)
    add_service_graph_rendering_test(unavailable_graphviz_retains_dot_and_removes_stale_svg)
    add_service_graph_rendering_test(failing_graphviz_removes_partial_svg)
endif()

add_executable(otlp_span_converter_test otlp_span_converter_test.cpp)
target_compile_features(otlp_span_converter_test PRIVATE cxx_std_20)
set_target_properties(otlp_span_converter_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(
    otlp_span_converter_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(
    otlp_span_converter_test
    SYSTEM PRIVATE
        "${SILHOUETTE_GENERATED_DIR}"
        "$<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>"
)
target_link_libraries(otlp_span_converter_test PRIVATE silhouette_otlp_conversion)
silhouette_enable_warnings(otlp_span_converter_test)

function(add_otlp_span_converter_test test_case)
    add_test(
        NAME otlp_span_converter_${test_case}
        COMMAND otlp_span_converter_test ${test_case}
    )
endfunction()

add_otlp_span_converter_test(converts_exact_fields_and_parent)
add_otlp_span_converter_test(preserves_nesting_order_and_selects_service_name)
add_otlp_span_converter_test(rejects_malformed_ids_and_keeps_valid_siblings)

add_executable(otlp_grpc_receiver_test otlp_grpc_receiver_test.cpp)
target_compile_features(otlp_grpc_receiver_test PRIVATE cxx_std_20)
set_target_properties(otlp_grpc_receiver_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(
    otlp_grpc_receiver_test
    PRIVATE silhouette_ingestion
)
target_include_directories(
    otlp_grpc_receiver_test
    SYSTEM PRIVATE
        "${SILHOUETTE_GENERATED_DIR}"
        "$<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>"
        "$<TARGET_PROPERTY:gRPC::grpc++,INTERFACE_INCLUDE_DIRECTORIES>"
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
add_otlp_grpc_receiver_test(empty_export_is_accepted_without_active_trace)
add_otlp_grpc_receiver_test(fully_valid_export_publishes_active_trace_state)
add_otlp_grpc_receiver_test(partially_valid_export_reports_rejection)
add_otlp_grpc_receiver_test(accepted_span_count_never_leads_published_state)
add_otlp_grpc_receiver_test(rejects_second_start)
add_otlp_grpc_receiver_test(allows_shutdown_before_start)
add_otlp_grpc_receiver_test(allows_repeated_shutdown)
add_otlp_grpc_receiver_test(rejects_restart_after_shutdown)
add_otlp_grpc_receiver_test(destructor_releases_port)
add_otlp_grpc_receiver_test(rejects_duplicate_bind)
