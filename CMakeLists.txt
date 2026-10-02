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
  src/assets/collision.cpp
  src/assets/anim.cpp
  src/assets/character.cpp
  src/assets/refpack.cpp
  src/assets/big_archive.cpp
  src/assets/carp_file.cpp
  src/assets/ssh_texture.cpp
  src/assets/spu_adpcm.cpp
  src/assets/sound_bank.cpp
  src/assets/music.cpp
  src/assets/sound_archive.cpp
  src/assets/map_sounds.cpp
  src/assets/level_music.cpp
  src/assets/driving_audio.cpp
  src/assets/ui_fonts.cpp
  src/assets/strings.cpp
  src/assets/sprites.cpp
  src/assets/ui_assets.cpp
  src/assets/menu_file.cpp
  src/assets/menu_messages.cpp
  src/assets/menu_validate.cpp
  src/assets/sp_menu.cpp
  src/assets/tweak_data.cpp
  src/assets/profile.cpp
  src/assets/credit_data.cpp
  src/assets/mission_data.cpp
  src/assets/cutscene.cpp
  src/assets/mp_data.cpp
  src/assets/hud_data.cpp
  src/assets/weapon_data.cpp
  src/assets/nav_data.cpp)
target_include_directories(nf_assets PUBLIC src)
target_compile_options(nf_assets PRIVATE -Wall -Wextra)

add_executable(nfdump src/tools/nfdump.cpp src/tools/nfdump_sounds.cpp src/tools/nfdump_chars.cpp src/tools/nfdump_ui.cpp
  src/tools/nfdump_ssh.cpp src/tools/nfdump_driving.cpp src/tools/nfdump_hud.cpp src/tools/nfdump_mp.cpp src/tools/nfdump_game.cpp src/tools/nfdump_weapons.cpp src/tools/nfdump_nav.cpp src/tools/nfdump_arena.cpp src/tools/nfdump_sp.cpp src/tools/nfdump_bots.cpp src/tools/nfdump_script.cpp src/tools/nfdump_diff.cpp
  src/ui/mp_setup.cpp)  # mp_setup is UI-independent (no GL), so nfdump compiles it directly instead of linking nf_ui
target_link_libraries(nfdump PRIVATE nf_assets nf_game nf_driving_sim)
target_compile_options(nfdump PRIVATE -Wall -Wextra)

find_package(OpenGL REQUIRED)
add_library(nf_render STATIC
  src/render/gl.cpp
  src/render/window.cpp
  src/render/level_renderer.cpp
  src/render/character_renderer.cpp)
target_link_libraries(nf_render PUBLIC nf_assets SDL3::SDL3-static OpenGL::GL)
target_compile_options(nf_render PRIVATE -Wall -Wextra)

add_executable(nfview src/viewer/main.cpp src/viewer/char_view.cpp)
target_link_libraries(nfview PRIVATE nf_render nf_game) # nf_game: CollisionWorld for the room-cel ray test
target_compile_options(nfview PRIVATE -Wall -Wextra)

# Driving missions: EA "Simulation"/CARP side of the game (DRIVING.ELF), see docs/driving.md.
# nf_driving_sim: everything without GL (formats, collision, physics, camera, session); nfdump links it.
file(GLOB NF_DRIVING_SOURCES CONFIGURE_DEPENDS src/driving/*.cpp)
list(FILTER NF_DRIVING_SOURCES EXCLUDE REGEX "scene_renderer\\.cpp$")
add_library(nf_driving_sim STATIC ${NF_DRIVING_SOURCES})
target_include_directories(nf_driving_sim PUBLIC src)
target_link_libraries(nf_driving_sim PUBLIC nf_assets)
target_compile_options(nf_driving_sim PRIVATE -Wall -Wextra)
add_library(nf_driving STATIC src/driving/scene_renderer.cpp)
target_link_libraries(nf_driving PUBLIC nf_driving_sim nf_render)
target_compile_options(nf_driving PRIVATE -Wall -Wextra)

add_executable(nfdrive src/nfdrive/main.cpp)
target_link_libraries(nfdrive PRIVATE nf_driving nf_audio)
target_compile_options(nfdrive PRIVATE -Wall -Wextra)

# Audio: software mixer over the SPU2 ADPCM data (SFX.IRX behaviour), SDL3 output, docs/audio.md.
file(GLOB NF_AUDIO_SOURCES CONFIGURE_DEPENDS src/audio/*.cpp)
add_library(nf_audio STATIC ${NF_AUDIO_SOURCES})
target_link_libraries(nf_audio PUBLIC nf_assets SDL3::SDL3-static)
target_compile_options(nf_audio PRIVATE -Wall -Wextra)

add_executable(nfplay src/tools/nfplay.cpp src/tools/nfplay_driving.cpp)
target_link_libraries(nfplay PRIVATE nf_audio)
target_compile_options(nfplay PRIVATE -Wall -Wextra)

# Movies: PSS (MPEG-2 program stream) playback via system FFmpeg (docs/formats.md "Movies").
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(MediaFFmpeg)
file(GLOB NF_MEDIA_SOURCES CONFIGURE_DEPENDS src/media/*.cpp)
add_library(nf_media STATIC ${NF_MEDIA_SOURCES})
target_link_libraries(nf_media PUBLIC nf_assets FFmpeg SDL3::SDL3-static)
target_compile_options(nf_media PRIVATE -Wall -Wextra)

# 2D UI: sprite/text renderer, HUD and front-end menus (docs/ui.md), preview tool nfui.
file(GLOB NF_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
add_library(nf_ui STATIC ${NF_UI_SOURCES})
target_link_libraries(nf_ui PUBLIC nf_render)
target_compile_options(nf_ui PRIVATE -Wall -Wextra)

file(GLOB NF_UI_TOOL_SOURCES CONFIGURE_DEPENDS src/tools/nfui*.cpp)
add_executable(nfui ${NF_UI_TOOL_SOURCES})
target_link_libraries(nfui PRIVATE nf_ui nf_media)
target_compile_options(nfui PRIVATE -Wall -Wextra)

# Gameplay: collision world, player movement, 30 Hz world tick (docs/gameplay.md), player executable nfgame.
# nf_game has no renderer dependency so nfdump can self-check the collision world.
file(GLOB NF_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
list(FILTER NF_GAME_SOURCES EXCLUDE REGEX "/(main|nfgame_[a-z_]+)\\.cpp$")   # the nfgame_* files draw, so they belong to the executable
add_library(nf_game STATIC ${NF_GAME_SOURCES})
target_link_libraries(nf_game PUBLIC nf_assets)
target_compile_options(nf_game PRIVATE -Wall -Wextra)

add_executable(nfgame src/game/main.cpp src/game/nfgame_mp.cpp src/game/nfgame_weapon_hud.cpp
  src/game/nfgame_weapon_view.cpp src/game/nfgame_effects.cpp src/game/nfgame_quads.cpp)
target_link_libraries(nfgame PRIVATE nf_game nf_render nf_audio)
target_compile_options(nfgame PRIVATE -Wall -Wextra)

# EE/R5900 headless interpreter + harness (docs/ee.md). nf_ee needs only
# zlib/zstd; nfmips additionally links nf_assets for the weapon_data check.
file(GLOB NF_EE_SOURCES CONFIGURE_DEPENDS src/ee/*.cpp)
add_library(nf_ee STATIC ${NF_EE_SOURCES})
target_include_directories(nf_ee PUBLIC src)
find_package(ZLIB REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
find_path(ZSTD_INCLUDE zstd.h REQUIRED)
target_include_directories(nf_ee PRIVATE ${ZSTD_INCLUDE})
target_link_libraries(nf_ee PUBLIC ZLIB::ZLIB ${ZSTD_LIB})
target_compile_options(nf_ee PRIVATE -Wall -Wextra)

add_executable(nfmips src/tools/nfmips.cpp)
target_link_libraries(nfmips PRIVATE nf_ee nf_assets)
target_compile_options(nfmips PRIVATE -Wall -Wextra)

# The playable game: frontend -> SP / MP / driving sessions with HUD, audio,
# pause and results (src/app/nightfire.cpp). Reuses nfgame's weapon view/effects
# translation units; main.cpp itself is untouched (Movement co-owns it).
file(GLOB NF_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp)
add_executable(nightfire ${NF_APP_SOURCES} src/game/nfgame_weapon_hud.cpp
  src/game/nfgame_weapon_view.cpp src/game/nfgame_effects.cpp src/game/nfgame_quads.cpp)
target_link_libraries(nightfire PRIVATE nf_game nf_render nf_audio nf_ui nf_driving nf_media)
target_compile_options(nightfire PRIVATE -Wall -Wextra)
