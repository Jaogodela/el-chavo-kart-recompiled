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
