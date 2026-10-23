cmake_minimum_required(VERSION 3.24)
project(gow3gpu_abi_smoke LANGUAGES CXX)

add_executable(gow3gpu-abi-smoke abi_smoke.cpp runtime_contract_stubs.cpp)
target_compile_features(gow3gpu-abi-smoke PRIVATE cxx_std_20)
target_link_options(gow3gpu-abi-smoke PRIVATE "LINKER:-export_dynamic")
