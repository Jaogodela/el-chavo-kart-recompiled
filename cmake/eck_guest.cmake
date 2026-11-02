# generated\default, wired directly rather than through rexglue_setup_target(): that macro's codegen rule would
# rerun ReXGlue during cmake --build. tools\codegen.ps1 writes generated\default and its freshness stamp; nothing
# here regenerates them, so a configured build never changes its own inputs while Ninja is running.
#
# Call after add_executable(el_chavo_kart ...) and after cmake\rexglue_pregenerated.cmake.

set(ECK_GUEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
set(ECK_GUEST_STAMP "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen-inputs.stamp")
if(NOT EXISTS "${ECK_GUEST_DIR}/sources.cmake" OR NOT EXISTS "${ECK_GUEST_STAMP}")
    message(FATAL_ERROR "generated\\default is missing: run tools\\codegen.ps1 (it needs game\\extracted\\default.xex)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ECK_GUEST_STAMP}")

include("${ECK_GUEST_DIR}/sources.cmake")
add_library(el_chavo_kart_recomp OBJECT ${GENERATED_SOURCES})
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}"
    OBJECT_DEPENDS "${ECK_GUEST_STAMP}")
target_include_directories(el_chavo_kart_recomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src "${ECK_GUEST_DIR}")
target_link_libraries(el_chavo_kart_recomp PRIVATE rex::runtime)
rexglue_apply_target_settings(el_chavo_kart_recomp)
rexglue_apply_recomp_settings(el_chavo_kart_recomp "${ECK_GUEST_DIR}/el_chavo_kart_pch.h")

target_link_libraries(el_chavo_kart PRIVATE el_chavo_kart_recomp)

# Fails the build, before anything links, when generated\default no longer matches its inputs: tools\build.ps1
# checks this too, but a direct `cmake --build` (an IDE, for example) must not silently compile a stale image.
find_program(ECK_WINDOWS_POWERSHELL NAMES powershell REQUIRED)
set(ECK_GUEST_CHECK "${CMAKE_COMMAND}" -E env --unset=PSModulePath
    "${ECK_WINDOWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen.ps1" -Check)
add_custom_target(eck_guest_check COMMAND ${ECK_GUEST_CHECK} VERBATIM)
add_dependencies(el_chavo_kart eck_guest_check)
add_dependencies(el_chavo_kart_recomp eck_guest_check)
