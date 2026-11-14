# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# This external directory is entered from video_core. Do not inherit the emulator's
# warning-as-error policy into third-party inference code or the shader compiler.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(LICHII_NR_GLSLANG NAMES glslang glslangValidator)
set(NR_SHADER_COMPILER "${LICHII_NR_GLSLANG}")
if (LICHII_NR_GLSLANG)
    execute_process(COMMAND "${LICHII_NR_GLSLANG}" --version
                    OUTPUT_VARIABLE NR_GLSLANG_VERSION RESULT_VARIABLE NR_GLSLANG_RESULT)
    string(REGEX MATCH "Glslang Version: [0-9]+:([0-9]+\\.[0-9]+\\.[0-9]+)"
           NR_GLSLANG_MATCH "${NR_GLSLANG_VERSION}")
endif()
if (NOT LICHII_NR_GLSLANG OR NOT NR_GLSLANG_RESULT EQUAL 0 OR
    NOT NR_GLSLANG_MATCH OR CMAKE_MATCH_1 VERSION_LESS "16.6.0")
    include(FetchContent)
    set(BUILD_EXTERNAL OFF)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)
    set(ENABLE_GLSLANG_BINARIES ON)
    set(ENABLE_SPIRV ON)
    set(ENABLE_OPT OFF)
    set(ENABLE_HLSL OFF)
    set(ENABLE_PCH OFF)
    FetchContent_Declare(lichii_nr_glslang
        URL "https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/16.6.0"
        URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
    FetchContent_MakeAvailable(lichii_nr_glslang)
    set(NR_SHADER_COMPILER "$<TARGET_FILE:glslang-standalone>")
    set(NR_SHADER_COMPILER_DEPENDENCY glslang-standalone)
endif()
file(GLOB NR_SHADER_SOURCES CONFIGURE_DEPENDS shaders/*.comp shaders/*.glsl)
file(GLOB NR_PTX_SOURCES CONFIGURE_DEPENDS scripts/ptx/*.py)
set(NR_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/assets.cpp")
add_custom_command(OUTPUT "${NR_ASSETS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_assets.py"
            --glslang "${NR_SHADER_COMPILER}" --output "${NR_ASSETS}"
    DEPENDS scripts/build_assets.py ${NR_SHADER_SOURCES} ${NR_PTX_SOURCES}
            ${NR_SHADER_COMPILER_DEPENDENCY}
    COMMENT "Building native neural rendering kernels"
    VERBATIM)

add_library(lichii_nr_kernels STATIC
    src/vk_context.cpp src/kernels.cpp src/nr_model.cpp src/nr_graph.cpp
    src/reference.cpp
    ${PROJECT_SOURCE_DIR}/src/video_core/neural_rendering/backend.cpp
    volk/volk.c "${NR_ASSETS}")
set_source_files_properties(volk/volk.c PROPERTIES LANGUAGE CXX)
target_compile_features(lichii_nr_kernels PRIVATE cxx_std_20)
target_compile_definitions(lichii_nr_kernels PRIVATE VOLK_NAMESPACE VK_ENABLE_BETA_EXTENSIONS VK_NO_PROTOTYPES)
target_include_directories(lichii_nr_kernels PRIVATE src volk)
target_link_libraries(lichii_nr_kernels PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
set_target_properties(lichii_nr_kernels PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (LICHII_TESTS)
    add_executable(neural_rendering_smoke EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/src/tests/video_core/neural_rendering_smoke.cpp
        ${PROJECT_SOURCE_DIR}/src/video_core/neural_rendering/model.cpp)
    target_include_directories(neural_rendering_smoke PRIVATE src volk ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(neural_rendering_smoke PRIVATE VOLK_NAMESPACE VK_ENABLE_BETA_EXTENSIONS VK_NO_PROTOTYPES)
    target_link_libraries(neural_rendering_smoke PRIVATE lichii_nr_kernels Vulkan::Headers
                          nlohmann_json::nlohmann_json OpenSSL::Crypto)
endif()
