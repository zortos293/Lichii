# SPDX-License-Identifier: GPL-3.0-or-later

# The shader extractor, the NUP reader and the shader cache v2 log reader are built from their
# sources, as nup-shader-export does, so the tool does not link the whole emulator.
add_executable(nx2-precompile main.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/nx2_precompile.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/nx2_precompile_build.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/nx2_shader_extract.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/nx2_lowering.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/shader_cache_v2/cache_file.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/shader_cache_v2/cache_format.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/nup_container.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/vfs/vfs.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/vfs/vfs_real.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/vfs/vfs_vector.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_sys/vfs/vfs_offset.cpp)
target_link_libraries(nx2-precompile PRIVATE common shader_recompiler OpenSSL::Crypto zstd::zstd xxhash nlohmann_json::nlohmann_json)
if (TARGET Boost::crc)
    target_link_libraries(nx2-precompile PRIVATE Boost::crc)
endif()
target_include_directories(nx2-precompile PRIVATE ${CMAKE_SOURCE_DIR}/src)
create_target_directory_groups(nx2-precompile)
