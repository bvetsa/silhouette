cmake_minimum_required(VERSION 3.24)

project(Silhouette LANGUAGES CXX)

add_executable(silhouette
    src/main.cpp
)

target_compile_features(silhouette PRIVATE cxx_std_20)
set_target_properties(silhouette PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(silhouette PRIVATE /W4)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(silhouette PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
