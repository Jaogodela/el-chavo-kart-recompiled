# SDK boilerplate lives in cmake/rexglue_pregenerated.cmake and cmake/eck_guest.cmake, not generated/default/
# rexglue.cmake: that file (written by rexglue codegen) would rerun codegen from the Ninja build graph.

cmake_minimum_required(VERSION 3.25)
project(el_chavo_kart VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where tools\codegen.ps1 unpacks the pinned SDK.
if(WIN32)
    set(ECK_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue/win-amd64")
else()
    set(ECK_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue/linux-amd64")
endif()
list(APPEND CMAKE_PREFIX_PATH "${ECK_SDK_DIR}")

# Release binaries must not carry the build machine's folders (__FILE__ in log calls).
add_compile_options("$<$<CONFIG:Release>:-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.>")

include(cmake/rexglue_pregenerated.cmake)

# TRG Launcher: the third_party/trg-launcher submodule, or a local checkout
# named by ECK_TRG_LAUNCHER_DIR while the submodule is not checked out.
set(ECK_TRG_LAUNCHER_DIR "$ENV{ECK_TRG_LAUNCHER_DIR}" CACHE PATH
    "Local TRG Launcher checkout, used when third_party/trg-launcher is empty")

# Upstream TRG is immutable. Every build compiles a patched copy under the
# binary directory; the submodule checkout must stay at the pinned revision.
set(ECK_TRG_LAUNCHER_REVISION "b384a28c712c9320eea8f9d50639dec49981b6c1")
set(ECK_TRG_LAUNCHER_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches/trg-launcher")
set(ECK_TRG_LAUNCHER_PATCH_STAMP_VERSION 1)
file(GLOB _trg_patch_names CONFIGURE_DEPENDS RELATIVE "${ECK_TRG_LAUNCHER_PATCH_DIR}"
    "${ECK_TRG_LAUNCHER_PATCH_DIR}/*.patch")
list(SORT _trg_patch_names)
# The glob dependency only tracks added or removed files; list the patches as
# configure dependencies too so editing one triggers a reconfigure.
foreach(_trg_patch_name IN LISTS _trg_patch_names)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ECK_TRG_LAUNCHER_PATCH_DIR}/${_trg_patch_name}")
endforeach()

find_package(Git QUIET)
if(NOT Git_FOUND)
    message(FATAL_ERROR "TRG Launcher: git is required to apply patches/trg-launcher")
endif()

# Copies the source into output_dir and applies the patches there. The stamp
# covers the source state and every patch, so the copy is rebuilt only when one
# of them changes. GIT_CEILING_DIRECTORIES keeps git apply from discovering the
# parent repository when the generated copy has no .git of its own.
function(eck_prepare_patched_trg_launcher source_dir output_dir require_pinned)
    set(_source_state "")
    if(EXISTS "${source_dir}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
                        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _head_result ERROR_QUIET)
        if(NOT _head_result EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: ${source_dir} is not a git checkout; run `git submodule update --init third_party/trg-launcher`")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" status --porcelain --untracked-files=normal
                        OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" diff --binary HEAD
                        OUTPUT_VARIABLE _diff)
        string(SHA256 _source_state "${_head}\n${_status}\n${_diff}")
        if(require_pinned)
            if(NOT _head STREQUAL ECK_TRG_LAUNCHER_REVISION)
                message(FATAL_ERROR "TRG Launcher: ${source_dir} is at ${_head}, expected the pinned ${ECK_TRG_LAUNCHER_REVISION}. Run `git submodule update --init third_party/trg-launcher`.")
            endif()
            if(NOT _status STREQUAL "")
                message(FATAL_ERROR "TRG Launcher: ${source_dir} has local changes; run `git -C third_party/trg-launcher checkout .` and configure again.")
            endif()
        endif()
    else()
        file(GLOB_RECURSE _source_files LIST_DIRECTORIES false RELATIVE "${source_dir}" "${source_dir}/*")
        list(SORT _source_files)
        foreach(_file IN LISTS _source_files)
            if(_file MATCHES "(^|/)\\.git(/|$)")
                continue()
            endif()
            file(SHA256 "${source_dir}/${_file}" _file_hash)
            string(APPEND _source_state "${_file}\n${_file_hash}\n")
        endforeach()
        string(SHA256 _source_state "${_source_state}")
    endif()

    set(_fingerprint "${ECK_TRG_LAUNCHER_PATCH_STAMP_VERSION}\n${source_dir}\n${_source_state}\n")
    foreach(_patch_name IN LISTS _trg_patch_names)
        file(SHA256 "${ECK_TRG_LAUNCHER_PATCH_DIR}/${_patch_name}" _patch_hash)
        string(APPEND _fingerprint "${_patch_name}\n${_patch_hash}\n")
    endforeach()
    string(SHA256 _fingerprint "${_fingerprint}")

    set(_stamp "${output_dir}/.eck-trg-patch-stamp")
    if(EXISTS "${_stamp}" AND EXISTS "${output_dir}/CMakeLists.txt")
        file(READ "${_stamp}" _stamp_value)
        if(_stamp_value STREQUAL _fingerprint)
            return()
        endif()
    endif()

    message(STATUS "TRG Launcher: preparing patched copy in ${output_dir}")
    file(REMOVE_RECURSE "${output_dir}")
    file(MAKE_DIRECTORY "${output_dir}")
    file(COPY "${source_dir}/" DESTINATION "${output_dir}" PATTERN ".git" EXCLUDE)

    get_filename_component(_ceiling "${output_dir}" DIRECTORY)
    foreach(_patch_name IN LISTS _trg_patch_names)
        set(_patch "${ECK_TRG_LAUNCHER_PATCH_DIR}/${_patch_name}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                        "${GIT_EXECUTABLE}" -C "${output_dir}" apply --check "${_patch}"
                        RESULT_VARIABLE _can_apply ERROR_VARIABLE _apply_error)
        if(NOT _can_apply EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: ${_patch} does not apply to ${source_dir} (${_apply_error}); the patch and the source revision are out of sync.")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                        "${GIT_EXECUTABLE}" -C "${output_dir}" apply "${_patch}"
                        RESULT_VARIABLE _apply_result)
        if(NOT _apply_result EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: failed to apply ${_patch}")
        endif()
    endforeach()
    file(WRITE "${_stamp}" "${_fingerprint}")
endfunction()

set(_trg_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/trg-launcher")
if(EXISTS "${_trg_source_dir}/CMakeLists.txt")
    set(_trg_require_pinned TRUE)
else()
    set(_trg_source_dir "${ECK_TRG_LAUNCHER_DIR}")
    set(_trg_require_pinned FALSE)
endif()
if(NOT EXISTS "${_trg_source_dir}/CMakeLists.txt")
    message(FATAL_ERROR "TRG Launcher not found: run `git submodule update --init third_party/trg-launcher`")
endif()

set(_trg_patched_dir "${CMAKE_BINARY_DIR}/trg-launcher-patched")
eck_prepare_patched_trg_launcher("${_trg_source_dir}" "${_trg_patched_dir}" ${_trg_require_pinned})

file(READ "${_trg_patched_dir}/src/launcher.cpp" _trg_launcher_source)
string(FIND "${_trg_launcher_source}" "void Launcher::HandleHotkeys() {" _hotkeys_pos)
string(SUBSTRING "${_trg_launcher_source}" ${_hotkeys_pos} -1 _hotkeys_source)
string(FIND "${_hotkeys_source}" "ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)" _ctrl_s_pos)
string(FIND "${_hotkeys_source}" "io.WantTextInput) return;" _text_guard_pos)
if(_hotkeys_pos EQUAL -1 OR _ctrl_s_pos EQUAL -1 OR _text_guard_pos EQUAL -1 OR _ctrl_s_pos GREATER _text_guard_pos)
    message(FATAL_ERROR "TRG Launcher: the staged HandleHotkeys does not handle Ctrl+S before the text-input guard (${_trg_patched_dir}/src/launcher.cpp)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_trg_patched_dir}/src/launcher.cpp")

# Reconfigure when the patches, the upstream checkout or its HEAD change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_trg_source_dir}/CMakeLists.txt")
if(EXISTS "${_trg_source_dir}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_trg_source_dir}" rev-parse --git-path HEAD
                    OUTPUT_VARIABLE _trg_head_path OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_trg_head_path)
        get_filename_component(_trg_head_path "${_trg_head_path}" ABSOLUTE BASE_DIR "${_trg_source_dir}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_trg_head_path}")
    endif()
endif()


# The launcher draws inside the runtime's own Dear ImGui frame, so it must use
# the ImGui compiled into rexruntime (one context), never a downloaded copy.
add_library(eck_imgui INTERFACE)
target_link_libraries(eck_imgui INTERFACE rex::runtime)
set(TRG_LAUNCHER_IMGUI_TARGET eck_imgui CACHE STRING "" FORCE)
set(TRG_LAUNCHER_STANDALONE OFF CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_FETCH_DEPS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_trg_patched_dir}" trg-launcher)
# Written for C++17: in C++20 and later u8string() is a different type.
set_target_properties(trg_launcher PROPERTIES CXX_STANDARD 17)
# TRG defines this only for cl.exe; clang++ needs it too for the MSVC CRT.
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
get_directory_property(ECK_TRG_VERSION DIRECTORY "${_trg_patched_dir}" DEFINITION PROJECT_VERSION)

set(ECK_SOURCES
    src/main.cpp
    src/achievements_overlay.cpp
    src/active_log.cpp
    src/crash_report.cpp
    src/diagnostics.cpp
    src/frame_counter.cpp
    src/frame_pacer.cpp
    src/game_files.cpp
    src/graphics_adapters.cpp
    src/guest_functions.cpp
    src/imgui_gamepad.cpp
    src/input_summary.cpp
    src/launcher.cpp
    src/launcher_achievements.cpp
    src/log_filter.cpp
    src/log_path.cpp
    src/modal_dialog.cpp
    src/notice_dialog.cpp
    src/platform.cpp
    src/port_version.cpp
    src/pt_br_ui_text.cpp
    src/session_log.cpp
    src/settings.cpp
    src/sha256.cpp
    src/system_info.cpp
    src/ui_text.cpp
    src/user_data.cpp
    src/window_host.cpp
    src/xiso.cpp
)
if(WIN32)
    # Resource 1 is the first icon in the executable: Explorer, and SDL's window class, use it.
    enable_language(RC)
    set(ECK_APP_RC "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/el_chavo_kart.rc")
    set_source_files_properties("${ECK_APP_RC}" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/app-icon.ico")
    list(APPEND ECK_SOURCES "${ECK_APP_RC}")
    add_executable(el_chavo_kart WIN32 ${ECK_SOURCES})
    set(ECK_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/el_chavo_kart.manifest")
    target_link_options(el_chavo_kart PRIVATE "LINKER:/MANIFESTINPUT:${ECK_APP_MANIFEST}")
    set_property(TARGET el_chavo_kart APPEND PROPERTY LINK_DEPENDS "${ECK_APP_MANIFEST}")
else()
    add_executable(el_chavo_kart ${ECK_SOURCES})
endif()
target_include_directories(el_chavo_kart PRIVATE src)

rexglue_setup_host_target(el_chavo_kart GPU_PLUGINS xenos)
target_link_libraries(el_chavo_kart PRIVATE trg::launcher)
if(WIN32)
    target_link_libraries(el_chavo_kart PRIVATE shcore synchronization dxgi shell32)
    # The SDK stages its DLLs only when the program links, so a rebuilt runtime or GPU plugin has to
    # make it link again; tools\build.ps1 also checks the staged copies.
    foreach(_sdk_dll rexruntime rexruntimed rexruntimerd rexgpu-xenos rexgpu-xenosd rexgpu-xenosrd)
        if(EXISTS "${ECK_SDK_DIR}/bin/${_sdk_dll}.dll")
            set_property(TARGET el_chavo_kart APPEND PROPERTY LINK_DEPENDS
                "${ECK_SDK_DIR}/bin/${_sdk_dll}.dll")
        endif()
    endforeach()
else()
    # The SDK stages the runtime beside the program only on Windows and macOS; the RPATH is $ORIGIN.
    add_custom_command(TARGET el_chavo_kart POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:rex::runtime>"
            "$<TARGET_FILE_DIR:el_chavo_kart>"
        VERBATIM
    )
endif()

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo|Release)$")
    message(FATAL_ERROR "Set CMAKE_BUILD_TYPE to Debug, RelWithDebInfo or Release (the presets and tools\\build.ps1 "
        "do): the ReXGlue runtime and GPU plugin are checked per configuration")
endif()
# Every build checks that tools/rexglue holds the runtime and GPU plugin built from patches/rexglue-sdk, and that
# the copies next to the program are those; tools\build.ps1 rebuilds them when they are not.
if(WIN32)
    # Windows PowerShell has a fixed path, unlike a Store pwsh; the module path a parent pwsh sets breaks it.
    find_program(ECK_WINDOWS_POWERSHELL NAMES powershell REQUIRED)
    set(ECK_REXGLUE_CHECK "${CMAKE_COMMAND}" -E env --unset=PSModulePath
        "${ECK_WINDOWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-patched-rexruntime.ps1" -Check -Config $<CONFIG>)
    add_custom_target(eck_rexglue_check COMMAND ${ECK_REXGLUE_CHECK} VERBATIM)
    add_dependencies(el_chavo_kart eck_rexglue_check)
    add_custom_target(eck_rexglue_staging ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE:rex::gpu-xenos>"
            "$<TARGET_FILE_DIR:el_chavo_kart>"
        COMMAND ${ECK_REXGLUE_CHECK} -Staged "$<TARGET_FILE_DIR:el_chavo_kart>"
        VERBATIM
    )
    add_dependencies(eck_rexglue_staging el_chavo_kart)
endif()

# ThinLTO in Release and RelWithDebInfo (the default of tools\build.ps1); the runtime and GPU plugin are prebuilt.
# Debug compiles at -O0, which marks every function optnone, so LTO would optimize nothing there.
function(eck_enable_thinlto target)
    set(_lto "$<CONFIG:Release,RelWithDebInfo>")
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        # lld-link runs the LTO backend at -O2 unless told otherwise; Release compiles at -O3.
        set(_link -flto=thin "$<$<CONFIG:Release>:-Wl,/opt:lldlto=3>"
            "-Wl,/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
    elseif(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_link -flto=thin -fuse-ld=lld "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
    else()
        return()
    endif()
    target_compile_options(${target} PRIVATE "$<$<AND:${_lto},$<COMPILE_LANGUAGE:CXX>>:-flto=thin>")
    get_target_property(_type ${target} TYPE)
    if(NOT _type STREQUAL "OBJECT_LIBRARY")
        target_link_options(${target} PRIVATE "$<${_lto}:${_link}>")
    endif()
endfunction()
eck_enable_thinlto(el_chavo_kart)
if(TARGET el_chavo_kart_recomp)
    eck_enable_thinlto(el_chavo_kart_recomp)
endif()

target_compile_definitions(el_chavo_kart PRIVATE
    ECK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ECK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ECK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ECK_TRG_VERSION="${ECK_TRG_VERSION}"
    ECK_BUILD_CONFIG="$<CONFIG>")

# The runtime reads its cvars from <exe dir>/el_chavo_kart.toml.
add_custom_command(TARGET el_chavo_kart POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/el_chavo_kart.toml"
        "$<TARGET_FILE_DIR:el_chavo_kart>"
    VERBATIM
)

include(cmake/eck_guest.cmake)

# The pure-logic tests (tests\CMakeLists.txt) are a separate CMake project with no ReXGlue SDK or TRG Launcher
# dependency; tools\test.ps1 configures and builds it on its own.
