# Pure-logic tests: no game data, no ReXGlue SDK, no TRG Launcher. tools\test.ps1 configures and builds this
# tree by itself so these checks run without the prerequisites the main build needs.
cmake_minimum_required(VERSION 3.25)
project(el_chavo_kart_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ECK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

find_package(Threads REQUIRED)

add_executable(eck_installer_tests
    installer_tests.cpp
    "${ECK_SRC_DIR}/game_files.cpp"
    "${ECK_SRC_DIR}/sha256.cpp"
    "${ECK_SRC_DIR}/xiso.cpp"
)
target_include_directories(eck_installer_tests PRIVATE "${ECK_SRC_DIR}")
target_link_libraries(eck_installer_tests PRIVATE Threads::Threads)

add_executable(eck_ui_text_tests
    ui_text_tests.cpp
    "${ECK_SRC_DIR}/ui_text.cpp"
    "${ECK_SRC_DIR}/pt_br_ui_text.cpp"
)
target_include_directories(eck_ui_text_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_ui_text_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_achievements_tests achievements_tests.cpp)
target_include_directories(eck_achievements_tests PRIVATE "${ECK_SRC_DIR}" stubs)


add_executable(eck_launcher_logic_tests launcher_logic_tests.cpp)
target_include_directories(eck_launcher_logic_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_launcher_logic_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

# Crash report text, a real unhandled exception in a child process, and what Report a bug resolves to.
add_executable(eck_crash_report_tests
    crash_report_tests.cpp
    "${ECK_SRC_DIR}/crash_report.cpp"
    "${ECK_SRC_DIR}/diagnostics.cpp"
    "${ECK_SRC_DIR}/log_path.cpp"
)
target_include_directories(eck_crash_report_tests PRIVATE "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(eck_crash_report_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")
target_link_libraries(eck_crash_report_tests PRIVATE ${CMAKE_DL_LIBS})

add_executable(eck_cursor_input_tests cursor_input_tests.cpp)
target_include_directories(eck_cursor_input_tests PRIVATE "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" stubs)
target_compile_definitions(eck_cursor_input_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_hotkey_tests hotkey_tests.cpp)
target_include_directories(eck_hotkey_tests PRIVATE "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(eck_hotkey_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_graphics_adapter_tests graphics_adapter_tests.cpp)
target_include_directories(eck_graphics_adapter_tests PRIVATE "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(eck_graphics_adapter_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_game_window_tests game_window_tests.cpp)
target_include_directories(eck_game_window_tests PRIVATE "${ECK_SRC_DIR}")


add_executable(eck_frame_limiter_tests frame_limiter_tests.cpp "${ECK_SRC_DIR}/frame_wait.cpp")
target_include_directories(eck_frame_limiter_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_frame_limiter_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")


add_executable(eck_debug_telemetry_tests debug_telemetry_tests.cpp)
target_include_directories(eck_debug_telemetry_tests PRIVATE "${ECK_SRC_DIR}")

# Window focus decides which game process consumes local input.
add_executable(eck_input_focus_tests input_focus_tests.cpp)
target_include_directories(eck_input_focus_tests PRIVATE "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(eck_input_focus_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_main_menu_tests
    main_menu_tests.cpp
    "${ECK_SRC_DIR}/main_menu_swf.cpp"
    "${ECK_SRC_DIR}/sha256.cpp"
)
target_include_directories(eck_main_menu_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_main_menu_tests PRIVATE ECK_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

# The client side of online play (src/online) without the launcher page or the game: protocol, identity, peer table,
# relay frames, rendezvous and the service against a scripted connection.
set(ECK_ONLINE_SOURCES
    "${ECK_SRC_DIR}/online/connection_test.cpp"
    "${ECK_SRC_DIR}/online/friend_view.cpp"
    "${ECK_SRC_DIR}/online/game_socket.cpp"
    "${ECK_SRC_DIR}/online/identity.cpp"
    "${ECK_SRC_DIR}/online/instance.cpp"
    "${ECK_SRC_DIR}/online/json.cpp"
    "${ECK_SRC_DIR}/online/lobby_link.cpp"
    "${ECK_SRC_DIR}/online/local_addresses.cpp"
    "${ECK_SRC_DIR}/online/net_addr.cpp"
    "${ECK_SRC_DIR}/online/online_service.cpp"
    "${ECK_SRC_DIR}/online/peer_table.cpp"
    "${ECK_SRC_DIR}/online/protocol.cpp"
    "${ECK_SRC_DIR}/online/random.cpp"
    "${ECK_SRC_DIR}/online/relay_frame.cpp"
    "${ECK_SRC_DIR}/online/rendezvous.cpp"
    "${ECK_SRC_DIR}/online/stun.cpp"
    "${ECK_SRC_DIR}/online/udp_transport.cpp"
    "${ECK_SRC_DIR}/online/upnp.cpp"
    "${ECK_SRC_DIR}/online/websocket.cpp"
    "${ECK_SRC_DIR}/online/websocket_frame.cpp"
    "${ECK_SRC_DIR}/pt_br_ui_text.cpp"
    "${ECK_SRC_DIR}/sha256.cpp"
    "${ECK_SRC_DIR}/ui_text.cpp"
)
if(WIN32)
    list(APPEND ECK_ONLINE_SOURCES "${ECK_SRC_DIR}/online/websocket_win.cpp")
else()
    list(APPEND ECK_ONLINE_SOURCES "${ECK_SRC_DIR}/online/websocket_posix.cpp")
endif()
add_library(eck_online_lib STATIC ${ECK_ONLINE_SOURCES})
target_include_directories(eck_online_lib PUBLIC "${ECK_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(eck_online_lib PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(eck_online_lib PUBLIC ws2_32 winhttp bcrypt)
    target_compile_definitions(eck_online_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(eck_online_lib PUBLIC ${CMAKE_DL_LIBS})
endif()

# The race model and the guest fingerprint check are pure: no service, no sockets.
add_executable(eck_online_race_tests online_race_tests.cpp "${ECK_SRC_DIR}/online_race.cpp")
target_include_directories(eck_online_race_tests PRIVATE "${ECK_SRC_DIR}")

# Race URL parser, probe registry, dry-run mapping and the guest fingerprint tables: pure, no ReXGlue.
add_executable(eck_online_probe_tests online_probe_tests.cpp "${ECK_SRC_DIR}/online_probe.cpp"
    "${ECK_SRC_DIR}/online_race_url.cpp" "${ECK_SRC_DIR}/online_race.cpp")
target_include_directories(eck_online_probe_tests PRIVATE "${ECK_SRC_DIR}")

# Race telemetry: from the race URL and the game state to one start and one complete, against the service wire.
add_executable(eck_online_race_telemetry_tests online_race_telemetry_tests.cpp "${ECK_SRC_DIR}/online_race_telemetry.cpp"
    "${ECK_SRC_DIR}/online_race_url.cpp")
target_link_libraries(eck_online_race_telemetry_tests PRIVATE eck_online_lib)
target_include_directories(eck_online_race_telemetry_tests PRIVATE "${ECK_SRC_DIR}")

# The character screen as the lobby, the race start and the open-file hook, headless.
add_executable(eck_online_lobby_tests online_lobby_tests.cpp "${ECK_SRC_DIR}/online_race.cpp"
               "${ECK_SRC_DIR}/online_race_url.cpp" "${ECK_SRC_DIR}/online_race_telemetry.cpp")
target_link_libraries(eck_online_lobby_tests PRIVATE eck_online_lib)

add_executable(eck_character_unlock_tests character_unlock_tests.cpp "${ECK_SRC_DIR}/online_race.cpp"
               "${ECK_SRC_DIR}/online_race_url.cpp" "${ECK_SRC_DIR}/online_race_telemetry.cpp")
target_link_libraries(eck_character_unlock_tests PRIVATE eck_online_lib)
target_compile_definitions(eck_character_unlock_tests PRIVATE ECK_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(eck_track_unlock_tests track_unlock_tests.cpp "${ECK_SRC_DIR}/online_race.cpp"
               "${ECK_SRC_DIR}/online_race_url.cpp" "${ECK_SRC_DIR}/online_race_telemetry.cpp")
target_link_libraries(eck_track_unlock_tests PRIVATE eck_online_lib)
target_compile_definitions(eck_track_unlock_tests PRIVATE ECK_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(eck_online_race_sync_tests online_race_sync_tests.cpp "${ECK_SRC_DIR}/online_race.cpp")
target_link_libraries(eck_online_race_sync_tests PRIVATE eck_online_lib)
target_compile_definitions(eck_online_race_sync_tests PRIVATE ECK_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(eck_online_menu_tests online_menu_tests.cpp)
target_link_libraries(eck_online_menu_tests PRIVATE eck_online_lib)
target_compile_definitions(eck_online_menu_tests PRIVATE ECK_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

foreach(_online_test identity protocol peer service session launcher)
    add_executable(eck_online_${_online_test}_tests online_${_online_test}_tests.cpp)
    target_link_libraries(eck_online_${_online_test}_tests PRIVATE eck_online_lib)
endforeach()
target_compile_definitions(eck_online_launcher_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")
target_sources(eck_online_session_tests PRIVATE "${ECK_SRC_DIR}/online_race_url.cpp" "${ECK_SRC_DIR}/online_race_telemetry.cpp"
               "${ECK_SRC_DIR}/online_race.cpp")

# Updater logic without the runtime: versions, release JSON, SHA-256, ZIP safety, staging, jobs and rollback.
set(ECK_UPDATER_DIR "${ECK_SRC_DIR}/updater")
set_source_files_properties("${ECK_UPDATER_DIR}/third_party/miniz/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")
add_executable(eck_update_tests
    update_tests.cpp
    "${ECK_UPDATER_DIR}/github_release.cpp"
    "${ECK_UPDATER_DIR}/install_transaction.cpp"
    "${ECK_UPDATER_DIR}/json.cpp"
    "${ECK_UPDATER_DIR}/sha256.cpp"
    "${ECK_UPDATER_DIR}/update_job.cpp"
    "${ECK_UPDATER_DIR}/update_paths.cpp"
    "${ECK_UPDATER_DIR}/update_platform.cpp"
    "${ECK_UPDATER_DIR}/update_zip.cpp"
    "${ECK_UPDATER_DIR}/version.cpp"
    "${ECK_UPDATER_DIR}/third_party/miniz/miniz.c"
)
target_include_directories(eck_update_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_update_tests PRIVATE ECK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(eck_update_tests PRIVATE -Wall -Wextra)
if(WIN32)
    target_link_libraries(eck_update_tests PRIVATE shell32)

    # The same sources as the trg-updater target of the main build.
    add_executable(eck_trg_updater WIN32
        "${ECK_UPDATER_DIR}/helper_main.cpp"
        "${ECK_UPDATER_DIR}/install_transaction.cpp"
        "${ECK_UPDATER_DIR}/sha256.cpp"
        "${ECK_UPDATER_DIR}/update_job.cpp"
        "${ECK_UPDATER_DIR}/update_paths.cpp"
        "${ECK_UPDATER_DIR}/update_platform.cpp"
    )
    set_target_properties(eck_trg_updater PROPERTIES OUTPUT_NAME trg-updater)
    target_include_directories(eck_trg_updater PRIVATE "${ECK_SRC_DIR}")
    target_link_libraries(eck_trg_updater PRIVATE shell32)
    target_link_options(eck_trg_updater PRIVATE -static)
    set_property(TARGET eck_trg_updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(eck_trg_updater PRIVATE -Wall -Wextra)

    add_executable(eck_update_restart_marker WIN32 update_restart_marker.cpp)
    target_link_libraries(eck_update_restart_marker PRIVATE shell32)
    target_link_options(eck_update_restart_marker PRIVATE -static)
    target_compile_options(eck_update_restart_marker PRIVATE -Wall -Wextra)

    add_executable(eck_update_helper_tests update_helper_tests.cpp)
    target_compile_options(eck_update_helper_tests PRIVATE -Wall -Wextra)
    target_compile_definitions(eck_update_helper_tests PRIVATE
        ECK_UPDATER_EXE="$<TARGET_FILE:eck_trg_updater>"
        ECK_RESTART_MARKER_EXE="$<TARGET_FILE:eck_update_restart_marker>")
    add_dependencies(eck_update_helper_tests eck_trg_updater eck_update_restart_marker)
endif()

# The self-relaunch command, the user data folder, the logs folder and window placement, on every platform.
add_executable(eck_relaunch_tests relaunch_tests.cpp)
target_include_directories(eck_relaunch_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_relaunch_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_user_data_tests user_data_tests.cpp "${ECK_SRC_DIR}/user_data.cpp")
target_include_directories(eck_user_data_tests PRIVATE "${ECK_SRC_DIR}")
target_compile_definitions(eck_user_data_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")

add_executable(eck_log_path_tests log_path_tests.cpp "${ECK_SRC_DIR}/log_path.cpp")
target_include_directories(eck_log_path_tests PRIVATE "${ECK_SRC_DIR}")

add_executable(eck_window_placement_tests window_placement_tests.cpp)
target_include_directories(eck_window_placement_tests PRIVATE "${ECK_SRC_DIR}")

add_executable(eck_dpi_tests dpi_tests.cpp)
target_include_directories(eck_dpi_tests PRIVATE "${ECK_SRC_DIR}")

# The bytes on the wire against tests/wire-vectors.txt, which the online service's tests read too.
add_executable(eck_wire_parity_tests wire_parity_tests.cpp "${ECK_SRC_DIR}/online_race.cpp")
target_link_libraries(eck_wire_parity_tests PRIVATE eck_online_lib)
target_compile_definitions(eck_wire_parity_tests PRIVATE ECK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(eck_wire_parity_tests PRIVATE -Wall -Wextra)

# Real sockets on loopback, the Connection test's verdicts, and UPnP against a scripted router.
foreach(_network_test network connection upnp)
    add_executable(eck_online_${_network_test}_tests online_${_network_test}_tests.cpp)
    target_link_libraries(eck_online_${_network_test}_tests PRIVATE eck_online_lib)
    target_compile_options(eck_online_${_network_test}_tests PRIVATE -Wall -Wextra)
endforeach()

# The production client against the Worker's code running locally; tools/test.sh and tools/test.ps1 start the Worker.
add_executable(eck_online_integration_tests online_integration_tests.cpp "${ECK_SRC_DIR}/online_race.cpp")
target_link_libraries(eck_online_integration_tests PRIVATE eck_online_lib)
target_compile_options(eck_online_integration_tests PRIVATE -Wall -Wextra)

# Tests that need the ReXGlue SDK: tools/test.ps1 and tools/test.sh name the extracted SDK (and, on Linux, the patched TRG
# Launcher copy of the main build), so an ordinary tests\ build still needs neither.
set(ECK_REXGLUE_SDK "" CACHE PATH "Extracted ReXGlue SDK for the tests that link the runtime")
set(ECK_TRG_PATCHED_DIR "" CACHE PATH "Patched TRG Launcher copy of the main build")
set(ECK_TRG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/trg-launcher" CACHE PATH "TRG Launcher checkout")
if(NOT ECK_TRG_PATCHED_DIR)
    # The pinned TRG Launcher with patches/trg-launcher applied, as the main build compiles it.
    if(NOT EXISTS "${ECK_TRG_SOURCE_DIR}/src/platform.cpp")
        message(FATAL_ERROR "third_party/trg-launcher is not checked out (ECK_TRG_SOURCE_DIR=${ECK_TRG_SOURCE_DIR})")
    endif()
    find_package(Git REQUIRED)
    set(_eck_trg_patched "${CMAKE_CURRENT_BINARY_DIR}/trg-launcher-patched")
    file(GLOB _eck_trg_patches "${CMAKE_CURRENT_SOURCE_DIR}/../patches/trg-launcher/*.patch")
    list(SORT _eck_trg_patches)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ECK_TRG_SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE _eck_trg_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(_eck_trg_state "${_eck_trg_head}\n")
    foreach(_patch IN LISTS _eck_trg_patches)
        file(SHA256 "${_patch}" _hash)
        string(APPEND _eck_trg_state "${_patch}\n${_hash}\n")
    endforeach()
    string(SHA256 _eck_trg_state "${_eck_trg_state}")
    set(_eck_trg_stamp "${_eck_trg_patched}/.eck-tests-patch-stamp")
    set(_eck_trg_current FALSE)
    if(EXISTS "${_eck_trg_stamp}")
        file(READ "${_eck_trg_stamp}" _eck_trg_old)
        if(_eck_trg_old STREQUAL _eck_trg_state)
            set(_eck_trg_current TRUE)
        endif()
    endif()
    if(NOT _eck_trg_current)
        file(REMOVE_RECURSE "${_eck_trg_patched}")
        file(MAKE_DIRECTORY "${_eck_trg_patched}")
        file(COPY "${ECK_TRG_SOURCE_DIR}/" DESTINATION "${_eck_trg_patched}" PATTERN ".git" EXCLUDE)
        foreach(_patch IN LISTS _eck_trg_patches)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
                            "${GIT_EXECUTABLE}" -C "${_eck_trg_patched}" apply "${_patch}"
                            RESULT_VARIABLE _applied ERROR_VARIABLE _apply_error)
            if(NOT _applied EQUAL 0)
                message(FATAL_ERROR "TRG Launcher: ${_patch} does not apply (${_apply_error})")
            endif()
        endforeach()
        file(WRITE "${_eck_trg_stamp}" "${_eck_trg_state}")
    endif()
    set(ECK_TRG_PATCHED_DIR "${_eck_trg_patched}")
endif()
set(_eck_trg_dir "${ECK_TRG_PATCHED_DIR}")
# Open logs folder, Report a bug and links: the commands the real TRG functions build, captured instead of run.
add_executable(eck_open_commands_tests
    open_commands_tests.cpp
    "${ECK_SRC_DIR}/log_path.cpp"
    "${_eck_trg_dir}/src/platform.cpp"
)
target_include_directories(eck_open_commands_tests PRIVATE "${ECK_SRC_DIR}" "${_eck_trg_dir}/include")
if(EXISTS "${_eck_trg_dir}/src/file_dialog.cpp")
    target_sources(eck_open_commands_tests PRIVATE "${_eck_trg_dir}/src/file_dialog.cpp")
    set_source_files_properties("${_eck_trg_dir}/src/file_dialog.cpp" PROPERTIES COMPILE_OPTIONS "-std=c++17")
endif()
set_source_files_properties("${_eck_trg_dir}/src/platform.cpp"
    PROPERTIES COMPILE_OPTIONS "-std=c++17" COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")
target_link_libraries(eck_open_commands_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(eck_open_commands_tests PRIVATE shell32 ole32 comdlg32 advapi32 user32)
endif()

if(ECK_REXGLUE_SDK)
    list(APPEND CMAKE_PREFIX_PATH "${ECK_REXGLUE_SDK}")
    find_package(rexglue 0.10.0 REQUIRED CONFIG)

    function(eck_use_runtime target)
        target_include_directories(${target} SYSTEM PRIVATE "${ECK_REXGLUE_SDK}/include")
        target_link_libraries(${target} PRIVATE rex::runtime Threads::Threads ${CMAKE_DL_LIBS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
        if(WIN32)
            file(GLOB _eck_tracy_dlls "${ECK_REXGLUE_SDK}/bin/TracyClient*.dll")
            foreach(_dll IN LISTS _eck_tracy_dlls)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>"
                    VERBATIM
                )
            endforeach()
        endif()
    endfunction()

    # The program's own manifest: its UTF-8 active code page is what lets the SDK read a TOML file in a folder with
    # accents.
    function(eck_use_app_manifest target)
        if(WIN32)
            set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/el_chavo_kart.manifest")
            target_link_options(${target} PRIVATE "LINKER:/MANIFESTINPUT:${_manifest}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_manifest}")
        endif()
    endfunction()

    # settings.cpp, the launcher's settings file editor and the SDK's cvar loader on real files, on both platforms. The
    # window and graphics-device queries are fixed answers.
    add_executable(eck_settings_tests
        settings_tests.cpp
        settings_platform_stubs.cpp
        "${ECK_SRC_DIR}/settings.cpp"
        "${ECK_SRC_DIR}/port_folder.cpp"
        "${ECK_SRC_DIR}/user_data.cpp"
        "${ECK_SRC_DIR}/game_files.cpp"
        "${ECK_SRC_DIR}/sha256.cpp"
        "${ECK_SRC_DIR}/xiso.cpp"
        "${ECK_SRC_DIR}/ui_text.cpp"
        "${ECK_SRC_DIR}/pt_br_ui_text.cpp"
        "${ECK_SRC_DIR}/diagnostics.cpp"
        "${ECK_SRC_DIR}/system_info.cpp"
        "${ECK_SRC_DIR}/crash_report.cpp"
        "${ECK_SRC_DIR}/log_path.cpp"
        "${ECK_SRC_DIR}/frame_rate_setting.cpp"
        "${ECK_SRC_DIR}/frame_wait.cpp"
        "${ECK_SRC_DIR}/log_filter.cpp"
        "${_eck_trg_dir}/src/settings.cpp"
        "${_eck_trg_dir}/src/platform.cpp"
    )
    target_include_directories(eck_settings_tests PRIVATE "${ECK_SRC_DIR}" "${_eck_trg_dir}/include")
    if(EXISTS "${_eck_trg_dir}/src/file_dialog.cpp")
        target_sources(eck_settings_tests PRIVATE "${_eck_trg_dir}/src/file_dialog.cpp")
        set_source_files_properties("${_eck_trg_dir}/src/file_dialog.cpp" PROPERTIES COMPILE_OPTIONS "-std=c++17")
    endif()
    set_source_files_properties("${_eck_trg_dir}/src/settings.cpp" "${_eck_trg_dir}/src/platform.cpp"
        PROPERTIES COMPILE_OPTIONS "-std=c++17" COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")
    eck_use_runtime(eck_settings_tests)
    eck_use_app_manifest(eck_settings_tests)

    # The achievements as ReXGlue's manager keeps them, with the port's list and summary.
    add_executable(eck_achievement_store_tests achievement_store_tests.cpp
        "${ECK_SRC_DIR}/port_folder.cpp" "${ECK_SRC_DIR}/user_data.cpp")
    target_include_directories(eck_achievement_store_tests PRIVATE "${ECK_SRC_DIR}")
    eck_use_runtime(eck_achievement_store_tests)
    eck_use_app_manifest(eck_achievement_store_tests)

    # The save files the game writes, through the runtime's virtual file system and host path device.
    add_executable(eck_save_files_tests save_files_tests.cpp)
    eck_use_runtime(eck_save_files_tests)

    if(WIN32)
        target_link_libraries(eck_settings_tests PRIVATE shell32 ole32 comdlg32 advapi32 user32)
    endif()
endif()

# Linux tests that need the ReXGlue SDK.
if(UNIX AND NOT APPLE AND ECK_REXGLUE_SDK)
    # The Linux Graphics card list through a stand-in Vulkan driver.
    add_library(eck_fake_vulkan SHARED fake_vulkan_driver.cpp)
    target_include_directories(eck_fake_vulkan SYSTEM PRIVATE "${ECK_REXGLUE_SDK}/include")
    target_sources(eck_graphics_adapter_tests PRIVATE "${ECK_SRC_DIR}/graphics_adapters.cpp")
    target_include_directories(eck_graphics_adapter_tests SYSTEM PRIVATE "${ECK_REXGLUE_SDK}/include")
    target_link_libraries(eck_graphics_adapter_tests PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(eck_graphics_adapter_tests eck_fake_vulkan)
    target_compile_definitions(eck_graphics_adapter_tests PRIVATE
        ECK_FAKE_VULKAN_LIBRARY="$<TARGET_FILE:eck_fake_vulkan>")

    # The AppImage's file-system behaviour and the Linux self-updater, in real temporary folders.
    add_executable(eck_linux_appimage_tests linux_appimage_tests.cpp
        "${ECK_SRC_DIR}/log_path.cpp" "${ECK_SRC_DIR}/port_folder.cpp" "${ECK_SRC_DIR}/user_data.cpp"
        "${ECK_UPDATER_DIR}/sha256.cpp" "${ECK_UPDATER_DIR}/update_paths.cpp" "${ECK_UPDATER_DIR}/update_platform.cpp")
    target_include_directories(eck_linux_appimage_tests PRIVATE "${ECK_SRC_DIR}")
    eck_use_runtime(eck_linux_appimage_tests)

    # Friend code Copy against SDL's offscreen video driver.
    add_executable(eck_clipboard_sdl_tests clipboard_sdl_tests.cpp "${ECK_SRC_DIR}/clipboard_sdl.cpp")
    target_include_directories(eck_clipboard_sdl_tests PRIVATE "${ECK_SRC_DIR}")
    target_compile_definitions(eck_clipboard_sdl_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")
    eck_use_runtime(eck_clipboard_sdl_tests)

    # The Vulkan Internal render resolution decision, from the header the patched ReXGlue source compiles it from.
    set(ECK_REXGLUE_SRC "" CACHE PATH "The patched ReXGlue source the Linux runtime was built from")
    if(NOT EXISTS "${ECK_REXGLUE_SRC}/include/rex/graphics/vulkan/internal_render_scale.h")
        message(FATAL_ERROR "ECK_REXGLUE_SRC must name the patched ReXGlue source (patch 0051 adds internal_render_scale.h)")
    endif()
    add_executable(eck_vulkan_scale_tests vulkan_internal_scale_tests.cpp)
    target_include_directories(eck_vulkan_scale_tests PRIVATE "${ECK_REXGLUE_SRC}/include")

    add_executable(eck_active_log_tests active_log_tests.cpp "${ECK_SRC_DIR}/active_log.cpp" "${ECK_SRC_DIR}/log_filter.cpp")
    target_include_directories(eck_active_log_tests PRIVATE "${ECK_SRC_DIR}")
    eck_use_runtime(eck_active_log_tests)

    # The sockets WebSocket client with the system's OpenSSL, against a local TLS front.
    add_executable(eck_online_websocket_tls_tests online_websocket_tls_tests.cpp)
    target_compile_definitions(eck_online_websocket_tls_tests PRIVATE ECK_SRC_DIR="${ECK_SRC_DIR}")
    target_compile_options(eck_online_websocket_tls_tests PRIVATE -Wall -Wextra)
    target_link_libraries(eck_online_websocket_tls_tests PRIVATE eck_online_lib)

    if(ECK_TRG_PATCHED_DIR)
        # The XDG portal replies and the portal, zenity, kdialog fallback order of the launcher's file picker.
        add_executable(eck_file_dialog_tests file_dialog_tests.cpp "${ECK_TRG_PATCHED_DIR}/src/file_dialog.cpp")
        target_include_directories(eck_file_dialog_tests PRIVATE "${ECK_TRG_PATCHED_DIR}/include")
    endif()
endif()
