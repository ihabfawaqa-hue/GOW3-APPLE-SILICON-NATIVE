cmake_minimum_required(VERSION 3.24)
project(gow3gpu_abi_smoke LANGUAGES C CXX)
find_package(Threads REQUIRED)

add_executable(gow3gpu-runtime-adapter-smoke
    runtime_adapter_smoke.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/core/gow3gpu_runtime_adapter.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/core/gow3gpu_gnm_adapter.c")
target_compile_features(gow3gpu-runtime-adapter-smoke PRIVATE c_std_11)
target_include_directories(gow3gpu-runtime-adapter-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/modules/libkernel"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/kernel/vfs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu")
target_link_libraries(gow3gpu-runtime-adapter-smoke PRIVATE Threads::Threads)

add_executable(gow3gpu-host-page-smoke host_page_smoke.cpp)
target_compile_features(gow3gpu-host-page-smoke PRIVATE cxx_std_20)
target_include_directories(gow3gpu-host-page-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu/shadps4")

add_executable(gow3gpu-abi-smoke abi_smoke.cpp)
target_compile_features(gow3gpu-abi-smoke PRIVATE cxx_std_20)
target_include_directories(gow3gpu-abi-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu")

add_executable(gow3gpu-renderer-init-smoke renderer_init_smoke.cpp)
target_sources(gow3gpu-renderer-init-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/core/gow3gpu_gnm_adapter.c")
target_compile_features(gow3gpu-renderer-init-smoke PRIVATE cxx_std_20)
target_include_directories(gow3gpu-renderer-init-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../runtime/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu")
target_link_libraries(gow3gpu-renderer-init-smoke PRIVATE Threads::Threads)

add_executable(gow3gpu-host-runtime-smoke host_runtime_smoke.cpp)
target_compile_features(gow3gpu-host-runtime-smoke PRIVATE cxx_std_20)
target_include_directories(gow3gpu-host-runtime-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu")
