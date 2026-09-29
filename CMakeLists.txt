cmake_minimum_required(VERSION 3.24)
project(nightfire_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

add_library(nf_assets STATIC
  src/assets/elf.cpp
  src/assets/game_files.cpp
  src/assets/bin_archive.cpp
  src/assets/map_file.cpp
  src/assets/ps2_gfx.cpp
  src/assets/level.cpp
  src/assets/collision.cpp)
target_include_directories(nf_assets PUBLIC src)
target_compile_options(nf_assets PRIVATE -Wall -Wextra)

add_executable(nfdump src/tools/nfdump.cpp)
target_link_libraries(nfdump PRIVATE nf_assets)
target_compile_options(nfdump PRIVATE -Wall -Wextra)

find_package(OpenGL REQUIRED)
add_library(nf_render STATIC
  src/render/gl.cpp
  src/render/window.cpp
  src/render/level_renderer.cpp)
target_link_libraries(nf_render PUBLIC nf_assets SDL3::SDL3-static OpenGL::GL)
target_compile_options(nf_render PRIVATE -Wall -Wextra)

add_executable(nfview src/viewer/main.cpp)
target_link_libraries(nfview PRIVATE nf_render)
target_compile_options(nfview PRIVATE -Wall -Wextra)
