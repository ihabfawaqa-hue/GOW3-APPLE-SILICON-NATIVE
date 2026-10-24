cmake_minimum_required(VERSION 3.24)
project(gow3_vulkan_probe LANGUAGES C CXX)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)

set(SPIRV_OUT "${CMAKE_CURRENT_BINARY_DIR}/push_descriptor.comp.spv")
add_custom_command(
    OUTPUT "${SPIRV_OUT}"
    COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/push_descriptor.comp" -o "${SPIRV_OUT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/push_descriptor.comp"
    VERBATIM)
add_custom_target(probe_shader DEPENDS "${SPIRV_OUT}")

set(BDA_SPIRV_OUT "${CMAKE_CURRENT_BINARY_DIR}/bda_indirection.comp.spv")
add_custom_command(
    OUTPUT "${BDA_SPIRV_OUT}"
    COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/bda_indirection.comp" -o "${BDA_SPIRV_OUT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bda_indirection.comp"
    VERBATIM)
add_custom_target(probe_bda_shader DEPENDS "${BDA_SPIRV_OUT}")

set(ALIAS_SPIRV_OUT "${CMAKE_CURRENT_BINARY_DIR}/direct_alias.comp.spv")
add_custom_command(
    OUTPUT "${ALIAS_SPIRV_OUT}"
    COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/direct_alias.comp" -o "${ALIAS_SPIRV_OUT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/direct_alias.comp"
    VERBATIM)
add_custom_target(probe_alias_shader DEPENDS "${ALIAS_SPIRV_OUT}")

add_executable(gow3-vulkan-probe vulkan_probe.cpp)
add_dependencies(gow3-vulkan-probe probe_shader probe_bda_shader probe_alias_shader)
target_compile_features(gow3-vulkan-probe PRIVATE cxx_std_20)
target_compile_definitions(gow3-vulkan-probe PRIVATE
    GOW3_PROBE_SHADER_PATH="${SPIRV_OUT}"
    GOW3_PROBE_BDA_SHADER_PATH="${BDA_SPIRV_OUT}"
    GOW3_PROBE_ALIAS_SHADER_PATH="${ALIAS_SPIRV_OUT}")
target_link_libraries(gow3-vulkan-probe PRIVATE Vulkan::Vulkan PkgConfig::SDL3)
