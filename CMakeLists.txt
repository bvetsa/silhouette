cmake_minimum_required(VERSION 3.24)

project(Silhouette LANGUAGES CXX)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

include(FetchContent)

FetchContent_Declare(
    opentelemetry_proto
    URL https://github.com/open-telemetry/opentelemetry-proto/archive/refs/tags/v1.11.0.tar.gz
    URL_HASH SHA256=cefb4cf0dee432bdd0eb25af73ed4c996b16e80baea7f98285c413184c1b92ad
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cmake/unused
)
FetchContent_MakeAvailable(opentelemetry_proto)

function(silhouette_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(SILHOUETTE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SILHOUETTE_OTEL_PROTO_ROOT "${opentelemetry_proto_SOURCE_DIR}")
file(MAKE_DIRECTORY "${SILHOUETTE_GENERATED_DIR}")
set(SILHOUETTE_OTEL_PROTO_FILES
    opentelemetry/proto/common/v1/common.proto
    opentelemetry/proto/resource/v1/resource.proto
    opentelemetry/proto/trace/v1/trace.proto
    opentelemetry/proto/collector/trace/v1/trace_service.proto
)

set(SILHOUETTE_OTLP_PROTO_SOURCES)
set(SILHOUETTE_OTLP_PROTO_HEADERS)

foreach(proto_file IN LISTS SILHOUETTE_OTEL_PROTO_FILES)
    string(REPLACE ".proto" ".pb.cc" generated_source "${proto_file}")
    string(REPLACE ".proto" ".pb.h" generated_header "${proto_file}")

    list(APPEND SILHOUETTE_OTLP_PROTO_SOURCES
        "${SILHOUETTE_GENERATED_DIR}/${generated_source}"
    )
    list(APPEND SILHOUETTE_OTLP_PROTO_HEADERS
        "${SILHOUETTE_GENERATED_DIR}/${generated_header}"
    )

    add_custom_command(
        OUTPUT
            "${SILHOUETTE_GENERATED_DIR}/${generated_source}"
            "${SILHOUETTE_GENERATED_DIR}/${generated_header}"
        COMMAND protobuf::protoc
        ARGS
            "--cpp_out=${SILHOUETTE_GENERATED_DIR}"
            "--proto_path=${SILHOUETTE_OTEL_PROTO_ROOT}"
            "${SILHOUETTE_OTEL_PROTO_ROOT}/${proto_file}"
        DEPENDS
            protobuf::protoc
            "${SILHOUETTE_OTEL_PROTO_ROOT}/${proto_file}"
        COMMENT "Generating C++ protobuf bindings for ${proto_file}"
        VERBATIM
    )
endforeach()

set(SILHOUETTE_TRACE_SERVICE_PROTO
    opentelemetry/proto/collector/trace/v1/trace_service.proto
)
string(REPLACE ".proto" ".grpc.pb.cc"
    SILHOUETTE_TRACE_SERVICE_GRPC_SOURCE
    "${SILHOUETTE_TRACE_SERVICE_PROTO}"
)
string(REPLACE ".proto" ".grpc.pb.h"
    SILHOUETTE_TRACE_SERVICE_GRPC_HEADER
    "${SILHOUETTE_TRACE_SERVICE_PROTO}"
)
set(SILHOUETTE_TRACE_SERVICE_GRPC_SOURCE
    "${SILHOUETTE_GENERATED_DIR}/${SILHOUETTE_TRACE_SERVICE_GRPC_SOURCE}"
)
set(SILHOUETTE_TRACE_SERVICE_GRPC_HEADER
    "${SILHOUETTE_GENERATED_DIR}/${SILHOUETTE_TRACE_SERVICE_GRPC_HEADER}"
)

add_custom_command(
    OUTPUT
        "${SILHOUETTE_TRACE_SERVICE_GRPC_SOURCE}"
        "${SILHOUETTE_TRACE_SERVICE_GRPC_HEADER}"
    COMMAND protobuf::protoc
    ARGS
        "--grpc_out=${SILHOUETTE_GENERATED_DIR}"
        "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
        "--proto_path=${SILHOUETTE_OTEL_PROTO_ROOT}"
        "${SILHOUETTE_OTEL_PROTO_ROOT}/${SILHOUETTE_TRACE_SERVICE_PROTO}"
    DEPENDS
        protobuf::protoc
        gRPC::grpc_cpp_plugin
        "${SILHOUETTE_OTEL_PROTO_ROOT}/${SILHOUETTE_TRACE_SERVICE_PROTO}"
    COMMENT "Generating gRPC bindings for the OTLP trace service"
    VERBATIM
)

add_library(silhouette_otlp_proto
    ${SILHOUETTE_OTLP_PROTO_SOURCES}
    ${SILHOUETTE_OTLP_PROTO_HEADERS}
    "${SILHOUETTE_TRACE_SERVICE_GRPC_SOURCE}"
    "${SILHOUETTE_TRACE_SERVICE_GRPC_HEADER}"
)
target_compile_features(silhouette_otlp_proto PUBLIC cxx_std_20)
set_target_properties(silhouette_otlp_proto PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(
    silhouette_otlp_proto
    SYSTEM PUBLIC "${SILHOUETTE_GENERATED_DIR}"
)
target_link_libraries(silhouette_otlp_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(silhouette_ingestion
    src/ingestion/otlp_grpc_receiver.cpp
)
target_compile_features(silhouette_ingestion PUBLIC cxx_std_20)
set_target_properties(silhouette_ingestion PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(silhouette_ingestion PUBLIC include)
target_link_libraries(silhouette_ingestion PRIVATE silhouette_otlp_proto)
silhouette_enable_warnings(silhouette_ingestion)

add_executable(silhouette
    src/main.cpp
)
target_compile_features(silhouette PRIVATE cxx_std_20)
set_target_properties(silhouette PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(silhouette PRIVATE silhouette_ingestion)
silhouette_enable_warnings(silhouette)

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
