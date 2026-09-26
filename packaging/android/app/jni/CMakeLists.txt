cmake_minimum_required(VERSION 3.22.1)
project(RocketRAndroid LANGUAGES C CXX)
add_link_options("-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")

# app/jni -> app -> android-project -> build -> Rocket-R root.
get_filename_component(ROCKET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT EXISTS "${ROCKET_ROOT}/VERSION")
    message(FATAL_ERROR "Could not locate Rocket-R root from Android JNI wrapper: ${ROCKET_ROOT}")
endif()

# SDLActivity expects libSDL2.so plus the game's libmain.so. Build SDL2 from the
# exact source revision pinned by Rocket-R so the APK does not depend on a host
# Android Studio installation beyond the SDK/NDK toolchain itself.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROCKET_ROOT}/extern/sdl2" "${CMAKE_BINARY_DIR}/sdl2")
set(SDL2_INCLUDE_DIRS "${ROCKET_ROOT}/extern/sdl2/include" CACHE STRING "Rocket-R Android SDL2 headers" FORCE)
add_subdirectory("${ROCKET_ROOT}" "${CMAKE_BINARY_DIR}/rocket-r")
