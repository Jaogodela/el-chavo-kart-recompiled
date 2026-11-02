# generated/default/rexglue.cmake (written by rexglue codegen) adds a codegen rule that reruns during
# cmake --build, so it is not included. tools\codegen.ps1 is the only codegen entry point; the guest itself is
# wired in cmake\eck_guest.cmake.

find_package(rexglue 0.10.0 REQUIRED CONFIG)

# Guest SEH scopes emit __try/__except, which traps hardware faults only under asynchronous EH. clang++ spells
# that flag the GNU way.
set(REXGLUE_RECOMP_OPTIONS "")
if(WIN32)
    if(MSVC)
        list(APPEND REXGLUE_RECOMP_OPTIONS /EHa)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND REXGLUE_RECOMP_OPTIONS -fasync-exceptions)
    endif()
endif()
list(APPEND REXGLUE_RECOMP_OPTIONS
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)

# clang rejects a PCH whose EH mode disagrees with the TU including it, so the options go on the target too.
function(rexglue_apply_recomp_settings target_name pch_header)
    target_precompile_headers(${target_name} PRIVATE "${pch_header}")
    target_compile_options(${target_name} PRIVATE ${REXGLUE_RECOMP_OPTIONS})
endfunction()

# Call after add_executable(); the guest object library links in separately from cmake\eck_guest.cmake.
macro(rexglue_setup_host_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(${target_name} PRIVATE rex::runtime)
    rexglue_configure_target(${target_name} ${ARGN})
endmacro()
