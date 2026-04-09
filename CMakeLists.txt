cmake_minimum_required(VERSION 3.21)

project(QuazzDoom LANGUAGES C)

set(DOOM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/QuazzDoom")

set(DOOM_COMMON_SOURCES
    ${DOOM_SRC_DIR}/am_map.c
    ${DOOM_SRC_DIR}/d_items.c
    ${DOOM_SRC_DIR}/d_main.c
    ${DOOM_SRC_DIR}/d_net.c
    ${DOOM_SRC_DIR}/doomdef.c
    ${DOOM_SRC_DIR}/doomstat.c
    ${DOOM_SRC_DIR}/dstrings.c
    ${DOOM_SRC_DIR}/f_finale.c
    ${DOOM_SRC_DIR}/f_wipe.c
    ${DOOM_SRC_DIR}/g_game.c
    ${DOOM_SRC_DIR}/hu_lib.c
    ${DOOM_SRC_DIR}/hu_stuff.c
    ${DOOM_SRC_DIR}/info.c
    ${DOOM_SRC_DIR}/m_argv.c
    ${DOOM_SRC_DIR}/m_bbox.c
    ${DOOM_SRC_DIR}/m_cheat.c
    ${DOOM_SRC_DIR}/m_fixed.c
    ${DOOM_SRC_DIR}/m_menu.c
    ${DOOM_SRC_DIR}/m_misc.c
    ${DOOM_SRC_DIR}/m_random.c
    ${DOOM_SRC_DIR}/m_swap.c
    ${DOOM_SRC_DIR}/p_ceilng.c
    ${DOOM_SRC_DIR}/p_doors.c
    ${DOOM_SRC_DIR}/p_enemy.c
    ${DOOM_SRC_DIR}/p_floor.c
    ${DOOM_SRC_DIR}/p_inter.c
    ${DOOM_SRC_DIR}/p_lights.c
    ${DOOM_SRC_DIR}/p_map.c
    ${DOOM_SRC_DIR}/p_maputl.c
    ${DOOM_SRC_DIR}/p_mobj.c
    ${DOOM_SRC_DIR}/p_plats.c
    ${DOOM_SRC_DIR}/p_pspr.c
    ${DOOM_SRC_DIR}/p_saveg.c
    ${DOOM_SRC_DIR}/p_setup.c
    ${DOOM_SRC_DIR}/p_sight.c
    ${DOOM_SRC_DIR}/p_spec.c
    ${DOOM_SRC_DIR}/p_switch.c
    ${DOOM_SRC_DIR}/p_telept.c
    ${DOOM_SRC_DIR}/p_tick.c
    ${DOOM_SRC_DIR}/p_user.c
    ${DOOM_SRC_DIR}/r_bsp.c
    ${DOOM_SRC_DIR}/r_data.c
    ${DOOM_SRC_DIR}/r_draw.c
    ${DOOM_SRC_DIR}/r_main.c
    ${DOOM_SRC_DIR}/r_plane.c
    ${DOOM_SRC_DIR}/r_segs.c
    ${DOOM_SRC_DIR}/r_sky.c
    ${DOOM_SRC_DIR}/r_things.c
    ${DOOM_SRC_DIR}/s_sound.c
    ${DOOM_SRC_DIR}/sounds.c
    ${DOOM_SRC_DIR}/st_lib.c
    ${DOOM_SRC_DIR}/st_stuff.c
    ${DOOM_SRC_DIR}/tables.c
    ${DOOM_SRC_DIR}/v_video.c
    ${DOOM_SRC_DIR}/w_wad.c
    ${DOOM_SRC_DIR}/wi_stuff.c
    ${DOOM_SRC_DIR}/z_zone.c
)

if(WIN32)
    set(DOOM_PLATFORM_SOURCES
        ${DOOM_SRC_DIR}/i_net_win.c
        ${DOOM_SRC_DIR}/i_sound_win.c
        ${DOOM_SRC_DIR}/i_system_win.c
        ${DOOM_SRC_DIR}/i_video_win.c
        ${DOOM_SRC_DIR}/win_compat.c
    )
else()
    set(DOOM_PLATFORM_SOURCES
        ${DOOM_SRC_DIR}/i_main.c
        ${DOOM_SRC_DIR}/i_net.c
        ${DOOM_SRC_DIR}/i_sound.c
        ${DOOM_SRC_DIR}/i_system.c
        ${DOOM_SRC_DIR}/i_video.c
    )
endif()

add_library(doom_core OBJECT ${DOOM_COMMON_SOURCES} ${DOOM_PLATFORM_SOURCES})
target_include_directories(doom_core PRIVATE ${DOOM_SRC_DIR})

if(WIN32)
    target_compile_definitions(doom_core PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NORMALUNIX)
    target_compile_options(doom_core PRIVATE /FI${DOOM_SRC_DIR}/doom_win32_compat.h)
endif()

add_executable(doom $<TARGET_OBJECTS:doom_core> ${DOOM_SRC_DIR}/i_main.c)
target_include_directories(doom PRIVATE ${DOOM_SRC_DIR})
set_target_properties(doom PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(WIN32)
    target_link_libraries(doom PRIVATE winmm)
    add_executable(doom-game WIN32 $<TARGET_OBJECTS:doom_core> ${DOOM_SRC_DIR}/i_main_win.c)
    target_include_directories(doom-game PRIVATE ${DOOM_SRC_DIR})
    target_link_libraries(doom-game PRIVATE winmm)
    set_target_properties(doom-game PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
