cmake_minimum_required(VERSION 3.16)

# PS Vita:  cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
# Desktop:  cmake -B build      (needs SDL2, SDL2_ttf, SDL2_image via pkg-config)

project(VitaSpot C)

# App version, shown in Settings and at the top of vam.log. A "d" suffix marks
# a development build; it changes only when a release is cut.
set(VITASPOT_VERSION "0.0.3")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Backend selection:
#   mock      fake catalogue for UI work (default)
#   external  link a prebuilt static library implementing include/vam_backend.h
set(VAM_BACKEND "mock" CACHE STRING "mock or external")
set(VAM_BACKEND_LIB "" CACHE FILEPATH "Path to the backend static library (external)")
set(VAM_BACKEND_LINK_LIBS "" CACHE STRING "Extra libraries the backend needs (external)")

set(FRONTEND_SOURCES
    src/main.c
    src/app.c
    src/lyrics.c
    src/model.c
    src/power.c
    src/prefs.c
    src/gfx/gfx.c
    src/gfx/text.c
    src/gfx/images.c
    src/ui/ui.c
    src/screens/widgets.c
    src/screens/sections.c
    src/screens/browse.c
    src/screens/page.c
    src/screens/player.c
    src/screens/sheet.c
    src/screens/login.c
    src/screens/settings.c
)

if(VITA)
    list(APPEND FRONTEND_SOURCES src/platform/platform_vita.c)
else()
    list(APPEND FRONTEND_SOURCES src/platform/platform_desktop.c src/platform/devscript.c)
endif()

if(VAM_BACKEND STREQUAL "mock")
    list(APPEND FRONTEND_SOURCES backend/mock/mock_backend.c)
elseif(NOT VAM_BACKEND STREQUAL "external")
    message(FATAL_ERROR "VAM_BACKEND must be 'mock' or 'external'")
endif()

add_executable(VitaSpot ${FRONTEND_SOURCES})
target_include_directories(VitaSpot PRIVATE include src)
target_compile_definitions(VitaSpot PRIVATE VAM_APP_VERSION="${VITASPOT_VERSION}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(VitaSpot PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(VAM_BACKEND STREQUAL "external")
    if(NOT VAM_BACKEND_LIB)
        message(FATAL_ERROR "Set VAM_BACKEND_LIB to the backend library")
    endif()
    target_link_libraries(VitaSpot PRIVATE ${VAM_BACKEND_LIB} ${VAM_BACKEND_LINK_LIBS})
endif()

if(VITA)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

    set(VITA_APP_NAME "VitaSpot")
    set(VITA_TITLEID "VTAM00001")
    set(VITA_VERSION "01.00")

    target_include_directories(VitaSpot PRIVATE $ENV{VITASDK}/arm-vita-eabi/include/SDL2)

    # Static link order matters. If the linker reports missing symbols, the
    # installed vdpm packages were built with different options: add the
    # library it names (e.g. brotlidec, webpdemux) to this list.
    target_link_libraries(VitaSpot PRIVATE
        SDL2_ttf SDL2_image SDL2
        freetype harfbuzz png jpeg webpdemux webp sharpyuv z bz2
        SceDisplay_stub SceCtrl_stub SceAudio_stub SceAudioIn_stub SceSysmodule_stub
        SceGxm_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceTouch_stub
        SceHid_stub SceMotion_stub ScePower_stub SceProcessmgr_stub SceKernelDmacMgr_stub
        SceIme_stub SceAVConfig_stub SceRegistryMgr_stub pthread m
    )

    vita_create_self(eboot.bin VitaSpot UNSAFE)

    # Bundle everything under assets/ (fonts etc.).
    file(GLOB_RECURSE VAM_ASSETS RELATIVE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/assets/*)
    set(VAM_ASSET_ARGS "")
    foreach(a ${VAM_ASSETS})
        if(NOT a MATCHES "README")
            list(APPEND VAM_ASSET_ARGS FILE ${CMAKE_SOURCE_DIR}/${a} ${a})
        endif()
    endforeach()

    vita_create_vpk(VitaSpot.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME}
        FILE sce_sys/icon0.png sce_sys/icon0.png
        FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
        ${VAM_ASSET_ARGS}
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf SDL2_image)
    target_link_libraries(VitaSpot PRIVATE PkgConfig::SDL m)
    if(WIN32)
        # Keep a console for SDL_Log output; drop -mwindows.
        target_link_options(VitaSpot PRIVATE -mconsole)
    endif()
    add_custom_command(TARGET VitaSpot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:VitaSpot>/assets)
endif()
