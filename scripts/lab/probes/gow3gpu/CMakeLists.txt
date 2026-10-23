cmake_minimum_required(VERSION 3.24)
project(gow3gpu_abi_smoke LANGUAGES CXX)

add_executable(gow3gpu-abi-smoke abi_smoke.cpp runtime_contract_stubs.cpp)
target_compile_features(gow3gpu-abi-smoke PRIVATE cxx_std_20)
target_link_options(gow3gpu-abi-smoke PRIVATE "LINKER:-export_dynamic")

add_executable(gow3gpu-renderer-init-smoke renderer_init_smoke.cpp runtime_contract_stubs.cpp)
target_compile_features(gow3gpu-renderer-init-smoke PRIVATE cxx_std_20)
target_include_directories(gow3gpu-renderer-init-smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/gow3gpu/gpu")
target_link_options(gow3gpu-renderer-init-smoke PRIVATE "LINKER:-export_dynamic")
