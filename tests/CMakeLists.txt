# Pure-logic tests: no game data, no ReXGlue SDK, no TRG Launcher. tools\test.ps1 configures and builds this
# tree by itself so these checks run without the prerequisites the main build needs.
cmake_minimum_required(VERSION 3.25)
project(el_chavo_kart_tests LANGUAGES CXX)

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
