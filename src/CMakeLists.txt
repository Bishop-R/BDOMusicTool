cmake_minimum_required(VERSION 3.20)
project(composer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- SDL3 via FetchContent ---
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        main
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
    audio.c
    bdo_format.c
    ice.c
    midi_import.c
    muse_format.c
    sample_extract.c
    wem2ogg.c
)

if(WIN32)
    list(APPEND COMPOSER_SOURCES composer.rc)
endif()

add_executable(composer WIN32 ${COMPOSER_SOURCES})

target_include_directories(composer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(composer PRIVATE SDL3::SDL3-static)

if(UNIX AND NOT APPLE)
    target_link_libraries(composer PRIVATE pthread dl m)
    include(GNUInstallDirs)
    install(TARGETS composer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES Icons/TrebleClef.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME composer.png)
    install(FILES composer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME composer.desktop)
elseif(WIN32)
    target_link_libraries(composer PRIVATE ole32 winmm -static)
endif()
