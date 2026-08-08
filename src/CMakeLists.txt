cmake_minimum_required(VERSION 3.20)
project(composer C)

option(BDO_GPU "Use SDL_GPU rendering when available (with SDL_Renderer fallback)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- SDL3 via FetchContent ---
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # Pin SDL: a moving branch made Linux and Windows builds change underneath us.
    GIT_TAG        release-3.2.20
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)


# --- Sources ---
set(COMPOSER_SOURCES
    main.c
    app.c
    model.c
    instruments.c
    undo.c
    ui_piano_roll.c
    ui_toolbar.c
    ui_render.c
    ui_menu.c
    audio.c
    bdo_format.c
    ice.c
    midi_import.c
    midi_export.c
    composition_tools.c
    bdo_validate.c
    midi_input.c
    muse_format.c
    sample_extract.c
    wem2ogg.c
    gpu_backend.c
)

if(WIN32)
    list(APPEND COMPOSER_SOURCES composer.rc)
endif()

add_executable(composer WIN32 ${COMPOSER_SOURCES})

if(BDO_GPU)
    target_compile_definitions(composer PRIVATE MUSE_GPU=1)
endif()

# The application deliberately runs relative to its executable directory.
# Keep all visual runtime assets beside every build artifact so a build-tree
# executable behaves exactly like a packaged one on Linux and Windows.
add_custom_command(TARGET composer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:composer>/Data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../Data/font_atlas.bin"
            "$<TARGET_FILE_DIR:composer>/Data/font_atlas.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../Data/font_atlas.png"
            "$<TARGET_FILE_DIR:composer>/Data/font_atlas.png"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Icons"
            "$<TARGET_FILE_DIR:composer>/Data/Icons"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/inst.vert.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/inst.frag.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/text.vert.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/text.frag.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/line.vert.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/line.frag.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/geom.vert.spv"
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_shaders/geom.frag.spv"
            "$<TARGET_FILE_DIR:composer>/Data"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:composer>/Data/samples"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../Data/samples/key_zones.json"
            "$<TARGET_FILE_DIR:composer>/Data/samples/key_zones.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../Data/samples/loop_points.json"
            "$<TARGET_FILE_DIR:composer>/Data/samples/loop_points.json"
    VERBATIM)

include(CTest)
if(BUILD_TESTING)
    add_executable(core_tests
        tests/core_tests.c model.c instruments.c composition_tools.c
        midi_export.c midi_import.c bdo_validate.c bdo_format.c ice.c undo.c)
    target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(UNIX AND NOT APPLE)
        target_link_libraries(core_tests PRIVATE m)
    endif()
    add_test(NAME core COMMAND core_tests)
endif()

target_include_directories(composer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(composer PRIVATE SDL3::SDL3-static)

if(UNIX AND NOT APPLE)
    target_link_libraries(composer PRIVATE pthread dl m)
    find_package(ALSA)
    if(ALSA_FOUND)
        target_compile_definitions(composer PRIVATE HAVE_ALSA_MIDI=1)
        target_link_libraries(composer PRIVATE ALSA::ALSA)
    endif()
    include(GNUInstallDirs)
    install(TARGETS composer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES Icons/TrebleClef.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME composer.png)
    install(FILES composer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME composer.desktop)
elseif(WIN32)
    target_link_libraries(composer PRIVATE ole32 winmm -static)
endif()
