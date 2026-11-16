# Silent Hill VR - Meta Quest standalone native build.
# Builds libSDL2.so + libmain.so (the game, pc_port) for arm64-v8a.
cmake_minimum_required(VERSION 3.22)
project(SilentHillQuest C CXX)

set(SHQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SHQ_DEPS "${SHQ_ROOT}/deps")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wno-error=format-security -ferror-limit=0)

# --- SDL2 (shared: SDLActivity loads libSDL2.so then libmain.so) -------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHQ_DEPS}/SDL2" sdl2 EXCLUDE_FROM_ALL)
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2)
endif()
set(SDL2_FOUND TRUE)

# --- OpenAL Soft (static, AAudio/OpenSL backends) ----------------------------
set(LIBTYPE SHARED)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EMBED_HRTF_DATA OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHQ_DEPS}/openal-soft" openal EXCLUDE_FROM_ALL)
set(OPENAL_INCLUDE_DIR "${SHQ_DEPS}/openal-soft/include" CACHE PATH "" FORCE)

# --- libjpeg-turbo (static; FMV MJPG decode) ---------------------------------
# Prebuilt by build-native.sh into deps/jpeg-build (it refuses add_subdirectory).
add_library(jpeg-static STATIC IMPORTED GLOBAL)
set_target_properties(jpeg-static PROPERTIES
    IMPORTED_LOCATION "${SHQ_DEPS}/jpeg-build/libjpeg.a")
set(JPEG_LIBRARY jpeg-static CACHE STRING "" FORCE)
set(JPEG_INCLUDE_DIR "${SHQ_DEPS}/libjpeg-turbo" CACHE PATH "" FORCE)

# --- OpenXR (Khronos Android loader, prebuilt) -------------------------------
add_library(openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${SHQ_DEPS}/openxr/lib/${ANDROID_ABI}/libopenxr_loader.so")

# --- The game ----------------------------------------------------------------
set(SH_FMV_FFMPEG OFF CACHE BOOL "" FORCE)
set(SH_ENABLE_RETROACHIEVEMENTS OFF CACHE BOOL "" FORCE)
set(SH_STATIC_MAPS ON CACHE BOOL "" FORCE)
# Game source: the dev layout keeps it beside android/ (silent-hill-decomp/),
# the GitHub repo has this project in quest/ at the repo root.
if(EXISTS "${SHQ_ROOT}/silent-hill-decomp/pc_port/CMakeLists.txt")
    set(SHQ_GAME "${SHQ_ROOT}/silent-hill-decomp")
else()
    set(SHQ_GAME "${SHQ_ROOT}/..")
endif()
add_subdirectory("${SHQ_GAME}/pc_port" game)

target_include_directories(SilentHillPC PRIVATE
    "${SHQ_DEPS}/openxr/include"
    "${SHQ_DEPS}/libjpeg-turbo"
    "${SHQ_DEPS}/jpeg-build")
# libopenxr_loader.so comes from the Khronos AAR (app/build.gradle); vr_openxr.c dlopens it.
