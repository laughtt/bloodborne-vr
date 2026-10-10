# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: MIT
#
# bbport_xr_sandbox3d.exe: the 3D VR sandbox (hunter body, IK arms, fingers, weapons, hitboxes)
# rendered by the app as stereo through bbport_xr.dll (ABI 4), and bbport_xr_sandbox3d_test.exe
# (offline). Built as part of gpu/xr_bridge (add_subdirectory there).

find_program(BB_GLSLANG NAMES glslangValidator glslang REQUIRED)

set(SB3D_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SB3D_SHADER_HEADERS "")
foreach(SHADER mesh.vert mesh.frag line.vert line.frag)
    string(REPLACE "." "_" SYMBOL ${SHADER})
    set(HEADER ${SB3D_SHADER_DIR}/${SYMBOL}.h)
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SB3D_SHADER_DIR}
        COMMAND ${BB_GLSLANG} -V --target-env vulkan1.3 --vn ${SYMBOL}_spv -o ${HEADER}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}
        COMMENT "sandbox3d shader ${SHADER}"
        VERBATIM)
    list(APPEND SB3D_SHADER_HEADERS ${HEADER})
endforeach()
add_custom_target(bbport_xr_sandbox3d_shaders DEPENDS ${SB3D_SHADER_HEADERS})

set(SB3D_CORE config.cpp config.h json.h math3d.h model.cpp model.h rig.cpp rig.h
    consumables_config.cpp consumables_config.h)

add_executable(bbport_xr_sandbox3d main.cpp renderer.cpp renderer.h scene.cpp scene.h
    image_out.h ${SB3D_CORE})
add_dependencies(bbport_xr_sandbox3d bbport_xr_sandbox3d_shaders)
target_include_directories(bbport_xr_sandbox3d PRIVATE ${SB3D_SHADER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bbport_xr_sandbox3d PRIVATE Vulkan::Vulkan)
target_compile_definitions(bbport_xr_sandbox3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# Runs from a scratch folder next to bbport_xr.dll: no MinGW runtime DLLs needed.
target_link_options(bbport_xr_sandbox3d PRIVATE -static-libstdc++ -static-libgcc)

add_executable(bbport_xr_sandbox3d_test rig_test.cpp ${SB3D_CORE})
target_include_directories(bbport_xr_sandbox3d_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_options(bbport_xr_sandbox3d_test PRIVATE -static-libstdc++ -static-libgcc)

# Comfort dash (docs/vr/findings/19): locomotion and moves, the vignette pass (reuses the line
# shaders), the --offline dash_*.png previews; the filter itself is ../vr_dash.h; the scripted
# locomotion tests in bbport_xr_sandbox3d_dash_test.exe.
target_sources(bbport_xr_sandbox3d PRIVATE locomotion.cpp locomotion.h vignette.cpp vignette.h
    dash_preview.cpp dash_preview.h ../vr_dash.h)
add_executable(bbport_xr_sandbox3d_dash_test locomotion_test.cpp locomotion.cpp locomotion.h
    model.cpp model.h json.h math3d.h ../vr_dash.h)
target_include_directories(bbport_xr_sandbox3d_dash_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bbport_xr_sandbox3d_dash_test PRIVATE Vulkan::Headers)
target_link_options(bbport_xr_sandbox3d_dash_test PRIVATE -static-libstdc++ -static-libgcc)

# Solid weapons (physics_weapon.h): weapon collision capsules from the mesh, the training props and
# the enemy, the substepped push-out solver, haptics, melee hits; the --offline collision_*.png
# previews; the collision tests run in bbport_xr_sandbox3d_test.
set(SB3D_PHYSICS collision.cpp collision.h physics_config.cpp physics_config.h physics_weapon.cpp
    physics_weapon.h)
target_sources(bbport_xr_sandbox3d PRIVATE ${SB3D_PHYSICS} physics_view.cpp physics_view.h
    collision_offline.cpp collision_offline.h)
target_sources(bbport_xr_sandbox3d_test PRIVATE ${SB3D_PHYSICS} collision_test.cpp)

# VR menu prototypes (ui.h): radial, grid, wrist and body-slot menus from out/vr_assets/
# ui_layouts.json, drawn with the game's icons and font (scripts/vr_assets/ui_extract.py) through
# the renderer's draw hook; the --offline ui_*.png previews; bbport_xr_sandbox3d_ui_test.exe.
set(SB3D_UI_SHADER_HEADERS "")
foreach(SHADER ui.vert ui.frag)
    string(REPLACE "." "_" SYMBOL ${SHADER})
    set(HEADER ${SB3D_SHADER_DIR}/${SYMBOL}.h)
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SB3D_SHADER_DIR}
        COMMAND ${BB_GLSLANG} -V --target-env vulkan1.3 --vn ${SYMBOL}_spv -o ${HEADER}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}
        COMMENT "sandbox3d shader ${SHADER}"
        VERBATIM)
    list(APPEND SB3D_UI_SHADER_HEADERS ${HEADER})
endforeach()
add_custom_target(bbport_xr_sandbox3d_ui_shaders DEPENDS ${SB3D_UI_SHADER_HEADERS})
set(SB3D_UI ui.cpp ui.h ui_layout.cpp ui_layout.h ui_render.cpp ui_render.h png_read.h)
target_sources(bbport_xr_sandbox3d PRIVATE ${SB3D_UI} ui_scene.cpp)
add_dependencies(bbport_xr_sandbox3d bbport_xr_sandbox3d_ui_shaders)
add_executable(bbport_xr_sandbox3d_ui_test ui_test.cpp ${SB3D_UI} model.cpp model.h json.h
    math3d.h image_out.h)
add_dependencies(bbport_xr_sandbox3d_ui_test bbport_xr_sandbox3d_ui_shaders)
target_include_directories(bbport_xr_sandbox3d_ui_test PRIVATE ${SB3D_SHADER_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bbport_xr_sandbox3d_ui_test PRIVATE Vulkan::Vulkan)
target_compile_definitions(bbport_xr_sandbox3d_ui_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(bbport_xr_sandbox3d_ui_test PRIVATE -static-libstdc++ -static-libgcc)

# Swing classification and stamina (combat.h, docs/vr/findings/20-swing-and-stamina.md S1-S2): the
# game's shared pure logic (../swing_logic.h) on the real right hand, paid when the solid weapon
# connects; the HUD through the menus' pipeline; the --offline combat_*.png previews; the scripted
# tests (also through the weapon physics) in bbport_xr_sandbox3d_combat_test.exe.
target_sources(bbport_xr_sandbox3d PRIVATE combat.cpp combat.h combat_hud.cpp combat_hud.h
    combat_view.cpp combat_view.h ../swing_logic.h)
add_executable(bbport_xr_sandbox3d_combat_test combat_test.cpp combat.cpp combat.h ../swing_logic.h
    ${SB3D_PHYSICS} ${SB3D_CORE} locomotion.cpp locomotion.h ../vr_dash.h)
target_include_directories(bbport_xr_sandbox3d_combat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bbport_xr_sandbox3d_combat_test PRIVATE Vulkan::Headers)
target_compile_definitions(bbport_xr_sandbox3d_combat_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(bbport_xr_sandbox3d_combat_test PRIVATE -static-libstdc++ -static-libgcc)


# Hand-thrown goods (throwables.h, docs/vr/findings/35-throwables.md): the rack, grab, hand-velocity
# throws mapped onto the game's Bullet values, impact effects; the scripted headless tests in
# bbport_xr_sandbox3d_throw_test.exe (also through the weapon physics' world).
set(SB3D_THROW throw_config.cpp throw_config.h throwables.cpp throwables.h)
target_sources(bbport_xr_sandbox3d PRIVATE ${SB3D_THROW})
# (config.cpp reads the "throwables" object: every target built with it links throw_config.cpp)
target_sources(bbport_xr_sandbox3d_test PRIVATE throw_config.cpp throw_config.h)
target_sources(bbport_xr_sandbox3d_combat_test PRIVATE throw_config.cpp throw_config.h)
add_executable(bbport_xr_sandbox3d_throw_test throwables_test.cpp ${SB3D_THROW} ${SB3D_PHYSICS}
    ${SB3D_CORE})
target_include_directories(bbport_xr_sandbox3d_throw_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_options(bbport_xr_sandbox3d_throw_test PRIVATE -static-libstdc++ -static-libgcc)

# Consumables as real objects (consumables.h, docs/vr/findings/36-vr-consumables.md): the R3-hold item
# wheel of 3D objects, grab with the grip, the throw / rub-on-the-blade / bring-to-the-head / crush
# gestures, the inventory and the player state; the scripted headless tests in
# bbport_xr_sandbox3d_consume_test.exe. (consumables_config.cpp is part of SB3D_CORE: config.cpp
# reads the "consumables" object.)
set(SB3D_CONSUME consumables.cpp consumables.h)
target_sources(bbport_xr_sandbox3d PRIVATE ${SB3D_CONSUME})
add_executable(bbport_xr_sandbox3d_consume_test consumables_test.cpp ${SB3D_CONSUME} ${SB3D_THROW}
    ${SB3D_PHYSICS} ${SB3D_CORE})
target_include_directories(bbport_xr_sandbox3d_consume_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_options(bbport_xr_sandbox3d_consume_test PRIVATE -static-libstdc++ -static-libgcc)
