cmake_minimum_required(VERSION 3.21)
project(GodHandNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GODHAND_RAYLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/port/_deps/raylib-src"
    CACHE PATH "Path to the existing raylib 5.5 source checkout")
if(NOT EXISTS "${GODHAND_RAYLIB_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set GODHAND_RAYLIB_SOURCE_DIR to a raylib 5.5 source checkout.")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GODHAND_RAYLIB_SOURCE_DIR}" raylib EXCLUDE_FROM_ALL)

add_library(godhand_pc_media STATIC pc_media.cpp)
target_include_directories(godhand_pc_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(godhand_pc_media PUBLIC raylib)

add_executable(godhand-native main.cpp)
target_link_libraries(godhand-native PRIVATE godhand_pc_media)
set_target_properties(godhand-native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
    target_compile_options(godhand_pc_media PRIVATE /W4 /utf-8)
    target_compile_options(godhand-native PRIVATE /W4 /utf-8)
endif()
