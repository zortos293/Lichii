# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# NVN direct renderer: Banana Native's NVN front end and Vulkan replay (MIT, bananative/) as a
# C library, built the way their x86-64 Linux port builds them (__ANDROID__
# selects their Vulkan replay; the Android headers they include come from bananative/shim).
# Linux x86-64, and Windows x86-64 with clang-cl (the code is GNU C: cl.exe does not take it), where
# plat_windows.c and the headers in bananative/shim/win32 stand in for the POSIX calls.

set(BN ${CMAKE_CURRENT_SOURCE_DIR}/bananative/dm)

add_library(nvn_direct STATIC
    gen/nvn_dispatch.inc
    gen_dispatch.py
    nvn_direct.h
    nvn_dispatch.c
    nvn_direct_front.h
    nvn_direct_host.c
    nvn_direct_log.cpp
    nvn_present.c
    nvn_recompile.c
    nvn_shutdown.c
    nvn_tracking.c
    nvn_window.c
    ${BN}/gfx/nvn/android_stub.c
    ${BN}/gfx/nvn/hle_nvn.c
    ${BN}/gfx/nvn/nvn_capture.c
    ${BN}/gfx/nvn/nvn_tex.c
    ${BN}/gfx/tex/nvn_deswizzle.c
    ${BN}/gfx/tex/nvn_fmt_mtl.c
    ${BN}/gfx/vk_android/vka_bind.c
    ${BN}/gfx/vk_android/vka_capture.c
    ${BN}/gfx/vk_android/vka_ctl.c
    ${BN}/gfx/vk_android/vka_dev.c
    ${BN}/gfx/vk_android/vka_dlss.c
    ${BN}/gfx/vk_android/vka_dlss_spv.h
    ${BN}/gfx/vk_android/vka_draw.c
    ${BN}/gfx/vk_android/vka_enc.c
    ${BN}/gfx/vk_android/vka_exec.c
    ${BN}/gfx/vk_android/vka_fps.c
    ${BN}/gfx/vk_android/vka_gprof.c
    ${BN}/gfx/vk_android/vka_imgmem.c
    ${BN}/gfx/vk_android/vka_mem.c
    ${BN}/gfx/vk_android/vka_nanscan.c
    ${BN}/gfx/vk_android/vka_perf.c
    ${BN}/gfx/vk_android/vka_pso.c
    ${BN}/gfx/vk_android/vka_readiness.c
    ${BN}/gfx/vk_android/vka_selftest.c
    ${BN}/gfx/vk_android/vka_shader.c
    ${BN}/gfx/vk_android/vka_shlib.c
    ${BN}/gfx/vk_android/vka_sparse.c
    ${BN}/gfx/vk_android/vka_spvpatch.c
    ${BN}/gfx/vk_android/vka_tex.c
    ${BN}/gfx/vk_android/vka_worker.c
    ${BN}/host/plat/plat_portable.c
)
if (WIN32)
    target_sources(nvn_direct PRIVATE ${BN}/host/plat/plat_windows.c)
else()
    target_sources(nvn_direct PRIVATE ${BN}/host/plat/plat_android.c)
endif()

# The recompiler translations' stamp (nvn_recompile.c, recompile_stamp.cmake): the shader
# recompiler, its NVN2 entry and the code that feeds it
set(RC_ROOT ${CMAKE_SOURCE_DIR}/src)
file(GLOB_RECURSE RC_STAMP_SOURCES CONFIGURE_DEPENDS RELATIVE ${RC_ROOT}
    ${RC_ROOT}/shader_recompiler/*.cpp ${RC_ROOT}/shader_recompiler/*.h ${RC_ROOT}/shader_recompiler/*.inc)
list(SORT RC_STAMP_SOURCES)
list(APPEND RC_STAMP_SOURCES video_core/renderer_vulkan/vk_nvn_direct_shaders.cpp video_core/renderer_vulkan/vk_nvn_direct_shaders.h
    video_core/nvn_direct/nvn_recompile.c video_core/nvn_direct/nvn_direct.h)
list(JOIN RC_STAMP_SOURCES "\n" RC_STAMP_LIST)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/recompile_stamp_sources.txt CONTENT "${RC_STAMP_LIST}\n")
list(TRANSFORM RC_STAMP_SOURCES PREPEND ${RC_ROOT}/ OUTPUT_VARIABLE RC_STAMP_DEPENDS)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nvn_recompile_stamp.h
    COMMAND ${CMAKE_COMMAND} -DROOT=${RC_ROOT} -DLIST=${CMAKE_CURRENT_BINARY_DIR}/recompile_stamp_sources.txt
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/nvn_recompile_stamp.h -P ${CMAKE_CURRENT_SOURCE_DIR}/recompile_stamp.cmake
    DEPENDS ${RC_STAMP_DEPENDS} ${CMAKE_CURRENT_SOURCE_DIR}/recompile_stamp.cmake ${CMAKE_CURRENT_BINARY_DIR}/recompile_stamp_sources.txt
    COMMENT "NVN direct: recompiler translations' stamp")
target_sources(nvn_direct PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nvn_recompile_stamp.h)

target_include_directories(nvn_direct PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    $<$<AND:$<BOOL:${WIN32}>,$<COMPILE_LANGUAGE:C>>:${CMAKE_CURRENT_SOURCE_DIR}/bananative/shim/win32>
    ${HOST_SHADERS_INCLUDE}
    bananative/shim
    gen
    .
    bananative/gen
    ${BN}/gfx/nvn/include
    ${BN}/gfx/nvn
    ${BN}/host
)
target_compile_definitions(nvn_direct PRIVATE __ANDROID__ _GNU_SOURCE)
# Upstream's code is kept as it is: its warnings are not Lichii's to fix.
target_compile_options(nvn_direct PRIVATE -w)
target_compile_definitions(nvn_direct INTERFACE LICHII_NVN_DIRECT)
# The shading-rate attachment's shader (vka_draw.c)
add_dependencies(nvn_direct host_shaders)
target_link_libraries(nvn_direct PUBLIC Vulkan::Headers PRIVATE common zstd::zstd ZLIB::ZLIB)
if (MSVC)
    # clang-cl knows GNU C only as gnu17 (its /std:c17 has no typeof), and the POSIX additions to the C
    # library's headers (shim/win32/posix.h) come ahead of every file
    target_compile_options(nvn_direct PRIVATE $<$<COMPILE_LANGUAGE:C>:/clang:-std=gnu17>
        $<$<COMPILE_LANGUAGE:C>:/FI${CMAKE_CURRENT_SOURCE_DIR}/bananative/shim/win32/posix.h>)
    target_compile_definitions(nvn_direct PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_link_libraries(nvn_direct PRIVATE synchronization)
elseif (MINGW)
    set_target_properties(nvn_direct PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
    target_compile_options(nvn_direct PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/bananative/shim/win32/posix.h>")
    target_compile_definitions(nvn_direct PRIVATE _USE_MATH_DEFINES)
    target_link_libraries(nvn_direct PRIVATE synchronization)
else()
    set_target_properties(nvn_direct PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
    target_link_libraries(nvn_direct PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
