# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: MIT
#
# bbport_xr.dll (OpenXR bridge) and bbport_xr_smoke.exe, built separately from the port in the
# MSYS2 CLANG64 shell (pacman -S mingw-w64-clang-x86_64-openxr-sdk):
#   cmake -S gpu/xr_bridge -B out/xr-bridge -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   ninja -C out/xr-bridge

cmake_minimum_required(VERSION 3.20)
project(bbport_xr LANGUAGES CXX)

# BBPORT_ONLINE=OFF (public alpha): no co-op pose sync (vr_pose_net.h, sandbox3d/coop_*).
# BBPORT_PUBLIC (public release bridge): no builder folder names in __FILE__, assert strings or debug info.
option(BBPORT_PUBLIC "Public build: strip the builder's paths" OFF)
if (BBPORT_PUBLIC)
    get_filename_component(BB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    add_compile_options("-ffile-prefix-map=${BB_REPO_ROOT}=/src" "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/build"
                        "-fmacro-prefix-map=${BB_REPO_ROOT}=/src" "-fdebug-prefix-map=${BB_REPO_ROOT}=/src")
endif()
    add_compile_definitions(BB_PUBLIC_ALPHA=1)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenXR REQUIRED)
find_package(Vulkan REQUIRED)

add_library(bbport_xr SHARED bbport_xr_bridge.cpp bbport_xr_bridge.h xr_input.cpp xr_input.h
    xr_input_logic.h xr_screen_math.h)
set_target_properties(bbport_xr PROPERTIES PREFIX "")
target_link_libraries(bbport_xr PRIVATE OpenXR::openxr_loader Vulkan::Headers)
target_compile_definitions(bbport_xr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# No MinGW runtime DLLs next to the game: link libc++/unwind statically.
target_link_options(bbport_xr PRIVATE -static-libstdc++ -static-libgcc)

add_executable(bbport_xr_smoke xr_smoke.cpp)
target_link_libraries(bbport_xr_smoke PRIVATE Vulkan::Vulkan)

# Fast-iteration VR sandbox: synthetic menu, live input printout, laser (drawn by the bridge).
add_executable(bbport_xr_sandbox xr_sandbox.cpp)
target_link_libraries(bbport_xr_sandbox PRIVATE Vulkan::Vulkan)

# The game's VR UI (BB_VR_UI=1, ABI 8; docs/vr/findings/26-game-ui.md): the sandbox's menus
# (sandbox3d/ui.*), HUD (sandbox3d/combat_hud.*) and UI pipeline (sandbox3d/ui_render.*) drawn by
# the bridge into a transparent stereo layer over the game; the input filter (vr_ui_filter.h).
# The UI shaders come from the sandbox's build (bbport_xr_sandbox3d_ui_shaders). The UI calls
# Vulkan through the loader (vulkan-1.dll, already loaded by the game) on the game's device.
set(BB_GAME_UI_SOURCES vr_game_ui.cpp vr_game_ui.h vr_game_hitbox.cpp vr_game_hitbox.h
    sandbox3d/wire_shapes.h vr_ui_filter.h sandbox3d/ui.cpp sandbox3d/ui.h
    sandbox3d/ui_layout.cpp sandbox3d/ui_layout.h sandbox3d/ui_render.cpp sandbox3d/ui_render.h
    sandbox3d/combat_hud.cpp sandbox3d/combat_hud.h sandbox3d/model.cpp sandbox3d/model.h
    sandbox3d/png_read.h sandbox3d/json.h sandbox3d/math3d.h swing_logic.h
    vr_held_mesh.h vr_hand_model.h vr_skin_model.h sandbox3d/throw_config.cpp sandbox3d/throw_config.h
    sandbox3d/consumables_config.cpp sandbox3d/consumables_config.h)
target_sources(bbport_xr PRIVATE ${BB_GAME_UI_SOURCES} vr_ui_layer.cpp vr_ui_layer.h)
target_include_directories(bbport_xr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox3d ${CMAKE_CURRENT_BINARY_DIR}/sandbox3d/shaders)
target_link_libraries(bbport_xr PRIVATE Vulkan::Vulkan)
# Offline tests of the game UI (logic only, scripted controllers and game state):
# out/xr-bridge/bbport_xr_game_ui_test.exe out/vr_assets
add_executable(bbport_xr_game_ui_test vr_game_ui_test.cpp ${BB_GAME_UI_SOURCES})
target_include_directories(bbport_xr_game_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox3d ${CMAKE_CURRENT_BINARY_DIR}/sandbox3d/shaders)
target_link_libraries(bbport_xr_game_ui_test PRIVATE Vulkan::Vulkan)
target_compile_definitions(bbport_xr_game_ui_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(bbport_xr_game_ui_test PRIVATE -static-libstdc++ -static-libgcc)

# Offline unit tests of the pure logic (screen placement with junk poses, the L3 layer); no
# headset needed: out/xr-bridge/bbport_xr_math_test.exe
add_executable(bbport_xr_math_test xr_math_test.cpp xr_screen_math.h xr_input_logic.h)
target_link_libraries(bbport_xr_math_test PRIVATE Vulkan::Headers)
# Swing classifier, attack scheduler and stamina model (swing_logic.h): offline tests,
# out/xr-bridge/bbport_xr_swing_test.exe (docs/vr/findings/20-swing-and-stamina.md).
add_executable(bbport_xr_swing_test swing_logic_test.cpp swing_logic.h trick_transform.h)


# The 3D VR sandbox (app-rendered stereo, ABI 4): gpu/xr_bridge/sandbox3d/README.md.
add_subdirectory(sandbox3d)
add_dependencies(bbport_xr bbport_xr_sandbox3d_ui_shaders)
add_dependencies(bbport_xr_game_ui_test bbport_xr_sandbox3d_ui_shaders)

# The comfort dash filter the sandbox and the game share (vr_dash.h): offline tests,
# out/xr-bridge/bbport_xr_dash_test.exe (docs/vr/findings/19-dash-locomotion.md).
add_executable(bbport_xr_dash_test vr_dash_test.cpp vr_dash.h)

# Alternate-eye stereo (BB_XR_MODE=alt, xr_stereo.h): which eye a finished frame belongs to, the
# alt projection layer, the pacing ages. Offline tests, out/xr-bridge/bbport_xr_stereo_test.exe
# (docs/vr/findings/22-phase3-alt-eye-stereo.md).
add_executable(bbport_xr_stereo_test xr_stereo_test.cpp xr_stereo.h)
