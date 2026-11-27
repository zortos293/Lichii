# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-2.0-or-later

add_library(frontend_common STATIC
    config.cpp
    config.h
    profile_import.cpp
    profile_import.h
    recommended_settings.cpp
    recommended_settings.h
    content_manager.h
    firmware_manager.h
    firmware_manager.cpp
    data_manager.h data_manager.cpp
    play_time_manager.cpp
    play_time_manager.h
    settings_generator.h settings_generator.cpp
    update_installer.h update_installer.cpp
    mod_manager.h mod_manager.cpp
    nx_archive.h nx_archive.cpp
    cloud_sync/zip_archive.h cloud_sync/zip_archive.cpp
    cloud_sync/save_archive.h cloud_sync/save_archive.cpp
    cloud_sync/google_auth.h cloud_sync/google_auth.cpp
    cloud_sync/drive_client.h cloud_sync/drive_client.cpp
    cloud_sync/sync_state.h cloud_sync/sync_state.cpp
    cloud_sync/sync_layout.h cloud_sync/sync_layout.cpp
    cloud_sync/sync_engine.h cloud_sync/sync_engine.cpp
    cloud_sync/profile_sync.h cloud_sync/profile_sync.cpp
    cloud_sync/web_service_sync.h cloud_sync/web_service_sync.cpp)

target_link_libraries(frontend_common PRIVATE httplib::httplib nlohmann_json::nlohmann_json
    OpenSSL::SSL OpenSSL::Crypto minizip::minizip)

# The build-time Drive sync OAuth client, in a generated header so changing it rebuilds one file.
configure_file(cloud_sync/build_client.h.in frontend_common/cloud_sync/build_client.h @ONLY)
target_include_directories(frontend_common PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if (ENABLE_UPDATE_CHECKER)
    target_sources(frontend_common PRIVATE
        update_checker.cpp
        update_checker.h)
endif()

create_target_directory_groups(frontend_common)
target_link_libraries(frontend_common
    PUBLIC core SimpleIni::SimpleIni frozen::frozen-headers
    PRIVATE common Boost::headers)
