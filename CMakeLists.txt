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
  src/assets/card_save.cpp
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
  src/render/weather_renderer.cpp
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
list(FILTER NF_DRIVING_SOURCES EXCLUDE REGEX "particle_render\\.cpp$")
add_library(nf_driving_sim STATIC ${NF_DRIVING_SOURCES})
target_include_directories(nf_driving_sim PUBLIC src)
target_link_libraries(nf_driving_sim PUBLIC nf_assets)
target_compile_options(nf_driving_sim PRIVATE -Wall -Wextra)
add_library(nf_driving STATIC src/driving/scene_renderer.cpp src/driving/particle_render.cpp)
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
# Original UI art sheets (assets/ui/*.png + manifests, generated by tools/art/*.py) are compiled in.
file(GLOB NF_ART_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/ui/*.png" "${CMAKE_SOURCE_DIR}/assets/ui/*.txt")
set(NF_ART_SOURCE "${CMAKE_BINARY_DIR}/generated/art_sheet_data.cpp")
add_custom_command(OUTPUT "${NF_ART_SOURCE}"
  COMMAND ${CMAKE_COMMAND} -DART_DIR=${CMAKE_SOURCE_DIR}/assets/ui -DOUT=${NF_ART_SOURCE} -P ${CMAKE_SOURCE_DIR}/cmake/EmbedArt.cmake
  DEPENDS ${NF_ART_FILES} "${CMAKE_SOURCE_DIR}/cmake/EmbedArt.cmake"
  COMMENT "Embedding UI art sheets")
add_library(nf_ui STATIC ${NF_UI_SOURCES} "${NF_ART_SOURCE}")
target_link_libraries(nf_ui PUBLIC nf_render nf_game)   # nf_game: prompts ask map_inputs which button an action uses
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
  src/game/nfgame_weapon_view.cpp src/game/nfgame_effects.cpp src/game/nfgame_quads.cpp
  src/ui/input_devices.cpp)   # LocalPad's binding table
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
target_link_libraries(nfmips PRIVATE nf_ee nf_assets nf_game)   # nf_game: diff-input compares nf::map_inputs
target_compile_options(nfmips PRIVATE -Wall -Wextra)

# Online multiplayer wire protocol and UDP transport.
add_library(nf_net STATIC src/net/net.cpp)
target_include_directories(nf_net PUBLIC src)
target_compile_options(nf_net PRIVATE -Wall -Wextra)
add_executable(nfnet-test src/tools/nfnet_test.cpp)
target_link_libraries(nfnet-test PRIVATE nf_net)
target_compile_options(nfnet-test PRIVATE -Wall -Wextra)
add_executable(nfnet-predict-diff src/tools/nfnet_predict_diff.cpp)
target_link_libraries(nfnet-predict-diff PRIVATE nf_net nf_game)
target_compile_options(nfnet-predict-diff PRIVATE -Wall -Wextra)
# Tiny UDP server registry; the server list protocol is intentionally independent of gameplay protocol.
add_executable(nfmaster src/net/master.cpp)
target_link_libraries(nfmaster PRIVATE nf_net)
target_compile_options(nfmaster PRIVATE -Wall -Wextra)
add_executable(nfserver src/net/server.cpp src/app/config.cpp src/ui/input_devices.cpp)   # config.cpp persists the bindings
target_link_libraries(nfserver PRIVATE nf_net nf_game nf_audio)
target_compile_options(nfserver PRIVATE -Wall -Wextra)
file(GLOB NF_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp)
add_executable(nightfire ${NF_APP_SOURCES} src/net/server.cpp src/game/nfgame_weapon_hud.cpp
  src/game/nfgame_weapon_view.cpp src/game/nfgame_effects.cpp src/game/nfgame_quads.cpp)
target_link_libraries(nightfire PRIVATE nf_net nf_game nf_render nf_audio nf_ui nf_driving nf_media)
target_compile_definitions(nightfire PRIVATE NF_SERVER_EMBEDDED)
target_compile_options(nightfire PRIVATE -Wall -Wextra)

# Installed tools are useful alongside the game, but no copyrighted game data is
# part of a package. UI art is embedded at build time; keep the original
# distributable source sheets/manifests available for inspection and tooling.
if(APPLE)
  set_target_properties(nightfire PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Nightfire"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.nightfire.engine"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0")
endif()

install(TARGETS nightfire nfgame nfserver nfmaster nfui nfdump nfmips nfview nfdrive nfplay nfnet-test nfnet-predict-diff
  RUNTIME DESTINATION bin
  BUNDLE DESTINATION .)
install(DIRECTORY assets/ui/ DESTINATION share/nightfire/assets/ui
  FILES_MATCHING PATTERN "*.png" PATTERN "*.txt")
install(DIRECTORY tools/art/ DESTINATION share/nightfire/tools/art
  FILES_MATCHING PATTERN "*.py" PATTERN "*.txt")
install(DIRECTORY assets/shaders/ DESTINATION share/nightfire/shaders OPTIONAL)
install(FILES README.md DESTINATION share/doc/nightfire)

set(CPACK_PACKAGE_NAME "nightfire-engine")
set(CPACK_PACKAGE_VENDOR "nightfire-engine contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OpenMW-style reimplementation of 007: Nightfire")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_CONTACT "nightfire-engine contributors")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR "TGZ")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
if(WIN32)
  set(CPACK_GENERATOR "ZIP;NSIS")
  set(CPACK_NSIS_DISPLAY_NAME "Nightfire Engine")
  set(CPACK_NSIS_PACKAGE_NAME "Nightfire Engine")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_MODIFY_PATH OFF)
endif()
if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "Nightfire")
  set(CPACK_DMG_FORMAT "UDZO")
endif()
include(CPack)
