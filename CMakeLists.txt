cmake_minimum_required(VERSION 3.22.1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ROCKET_R_VERSION LIMIT_COUNT 1)
if(NOT ROCKET_R_VERSION MATCHES "^([0-9]+[.][0-9]+[.][0-9]+)([-+][0-9A-Za-z._-]+)?$")
    message(FATAL_ERROR "VERSION must contain a semantic version")
endif()
project(RocketR VERSION ${CMAKE_MATCH_1} LANGUAGES C CXX)

# This project can be the top-level desktop build or a subdirectory of the
# SDL2 Android Gradle project. CMAKE_SOURCE_DIR points at the Gradle JNI
# wrapper in the latter case, so keep one canonical path to Rocket-R itself.
set(ROCKET_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ROCKET_SOURCE_ROOT}/VERSION")

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT ANDROID)
    enable_testing()
    add_executable(RocketPresentationTests tests/presentation_identity_tests.cpp
        src/graphics_enhancements.cpp
        extern/rt64/src/common/rt64_math.cpp
        extern/rt64/src/hle/rt64_rigid_body.cpp)
    target_include_directories(RocketPresentationTests PRIVATE
        "${ROCKET_SOURCE_ROOT}/extern/n64-modern-runtime/N64Recomp/include"
        "${ROCKET_SOURCE_ROOT}/extern/rt64/src"
        "${ROCKET_SOURCE_ROOT}/extern/rt64/include"
        "${ROCKET_SOURCE_ROOT}/extern/rt64/src/contrib/hlslpp/include")
    add_test(NAME presentation_identity COMMAND RocketPresentationTests)
    find_package(Threads REQUIRED)
    add_executable(RocketRuntimeLogTests tests/runtime_log_tests.cpp src/runtime_log.cpp)
    target_include_directories(RocketRuntimeLogTests PRIVATE "${ROCKET_SOURCE_ROOT}/src")
    target_link_libraries(RocketRuntimeLogTests PRIVATE Threads::Threads)
    if(WIN32)
        set_target_properties(RocketRuntimeLogTests PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(RocketRuntimeLogTests PRIVATE /ENTRY:mainCRTStartup)
    endif()
    add_test(NAME runtime_log COMMAND RocketRuntimeLogTests)
    set_tests_properties(runtime_log PROPERTIES TIMEOUT 60)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    # ROCKET-R UI V36 WINDOWS ICON RESOURCE
    enable_language(RC)
endif()

set(ROCKET_RUNTIME_DIR "${ROCKET_SOURCE_ROOT}/extern/n64-modern-runtime")
set(ROCKET_RT64_DIR "${ROCKET_SOURCE_ROOT}/extern/rt64")
set(ROCKET_GENERATED_CPU "${ROCKET_SOURCE_ROOT}/runtime-recomp/RecompiledFuncs")
set(ROCKET_GENERATED_RSP "${ROCKET_SOURCE_ROOT}/runtime-recomp/RecompiledRSP")

foreach(required_path
    "${ROCKET_RUNTIME_DIR}/CMakeLists.txt"
    "${ROCKET_RT64_DIR}/CMakeLists.txt"
    "${ROCKET_SOURCE_ROOT}/generated/rom_identity.generated.hpp"
    "${ROCKET_SOURCE_ROOT}/generated/bootstrap.generated.hpp"
    "${ROCKET_SOURCE_ROOT}/generated/mod_protection.generated.hpp"
    "${ROCKET_SOURCE_ROOT}/generated/mod_camera.generated.hpp"
    "${ROCKET_GENERATED_CPU}/recomp_overlays.inl")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Missing generated/dependency file: ${required_path}\n"
            "Run ONE-CLICK-BUILD.cmd from the repository root first.")
    endif()
endforeach()

file(GLOB ROCKET_RECOMPILED_C CONFIGURE_DEPENDS "${ROCKET_GENERATED_CPU}/*.c")
file(GLOB ROCKET_RECOMPILED_CXX CONFIGURE_DEPENDS "${ROCKET_GENERATED_CPU}/*.cpp")
file(GLOB ROCKET_RSP_CXX CONFIGURE_DEPENDS "${ROCKET_GENERATED_RSP}/*.cpp")
if(NOT ROCKET_RECOMPILED_C AND NOT ROCKET_RECOMPILED_CXX)
    message(FATAL_ERROR "N64Recomp produced no CPU source files. Run ONE-CLICK-BUILD.cmd.")
endif()
if(NOT ROCKET_RSP_CXX)
    message(FATAL_ERROR "RSPRecomp produced no audio microcode source. Run ONE-CLICK-BUILD.cmd.")
endif()

# Keep the same core dependency family used by DKR-R.
set(RT64_STATIC TRUE CACHE BOOL "" FORCE)
set(RT64_SDL_WINDOW_VULKAN TRUE CACHE BOOL "" FORCE)
add_compile_definitions(HLSL_CPU)
add_subdirectory("${ROCKET_RT64_DIR}" "${CMAKE_BINARY_DIR}/rt64")
# Rocket renders into a deliberate 18x14 safe area inside its 320x240 N64
# framebuffer. Enable the Rocket-only RT64 presentation crop so that active
# area fills the selected 4:3/Expand output instead of preserving overscan.
target_compile_definitions(rt64 PRIVATE RT64_ROCKET_SAFE_AREA_CROP=1)
add_subdirectory("${ROCKET_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/N64ModernRuntime")

add_library(RocketModsCore STATIC src/mods/mod_library.cpp)
target_include_directories(RocketModsCore PUBLIC "${ROCKET_SOURCE_ROOT}/src"
    "${ROCKET_RUNTIME_DIR}/thirdparty")
target_link_libraries(RocketModsCore PUBLIC miniz)
if(NOT ANDROID)
    add_executable(RocketModsTests tests/mods_tests.cpp)
    target_link_libraries(RocketModsTests PRIVATE RocketModsCore)
    add_test(NAME mods COMMAND RocketModsTests)
    add_executable(RocketCameraModTests tests/camera_mod_tests.cpp tests/camera_mod_logic_tests.cpp src/mods/mod_runtime.cpp)
    target_link_libraries(RocketCameraModTests PRIVATE RocketModsCore)
    target_include_directories(RocketCameraModTests PRIVATE
        "${ROCKET_SOURCE_ROOT}/modding/include"
        "${ROCKET_SOURCE_ROOT}/generated"
        "${ROCKET_RUNTIME_DIR}/N64Recomp/include"
        "${ROCKET_RUNTIME_DIR}/ultramodern/include"
        "${ROCKET_RUNTIME_DIR}/librecomp/include"
        "$<TARGET_PROPERTY:ultramodern,INCLUDE_DIRECTORIES>"
        "$<TARGET_PROPERTY:librecomp,INCLUDE_DIRECTORIES>")
    add_test(NAME camera_mod COMMAND RocketCameraModTests)
    add_executable(RocketGraphicsCameraTests tests/graphics_camera_tests.cpp src/graphics_enhancements.cpp)
    target_include_directories(RocketGraphicsCameraTests PRIVATE
        "${ROCKET_SOURCE_ROOT}/src"
        "${ROCKET_RUNTIME_DIR}/N64Recomp/include")
    target_link_libraries(RocketGraphicsCameraTests PRIVATE Threads::Threads)
    add_test(NAME graphics_camera COMMAND RocketGraphicsCameraTests)
endif()

add_library(RocketRecompiled STATIC ${ROCKET_RECOMPILED_C} ${ROCKET_RECOMPILED_CXX})
target_include_directories(RocketRecompiled PRIVATE
    "${ROCKET_GENERATED_CPU}"
    "${ROCKET_RUNTIME_DIR}/N64Recomp/include"
    "${ROCKET_RUNTIME_DIR}/ultramodern/include"
    "${ROCKET_RUNTIME_DIR}/librecomp/include")
if(WIN32)
    # Windows builds deliberately use clang-cl. It accepts -W flags directly,
    # while -f switches need to pass through the clang compatibility escape.
    target_compile_options(RocketRecompiled PRIVATE
        /clang:-fno-strict-aliasing
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>)
else()
    target_compile_options(RocketRecompiled PRIVATE
        -fvisibility=hidden -fno-strict-aliasing
        -Wno-unused-variable -Wno-unused-but-set-variable
        -Wno-builtin-declaration-mismatch
        $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>)
endif()

set(ROCKET_RUNTIME_SOURCES
    src/main.cpp
    src/platform.cpp
    src/runtime_input.cpp
    src/binding_capture.cpp
    src/controls_studio.cpp
    src/graphics_enhancements.cpp
    src/runtime_ui.cpp
    src/rt64_renderer.cpp
    src/renderer_snapshot.cpp
    src/presentation_identity.cpp
    src/render_telemetry_compat.cpp
    src/widescreen_culling.cpp
    src/game_registration.cpp
    src/mods/mod_runtime.cpp
    src/mods/mod_ui.cpp
    src/mods/mod_android.cpp
    src/crash_handler.cpp
    src/runtime_log.cpp
    src/register_overlays.cpp
    "${ROCKET_RT64_DIR}/src/contrib/imgui/backends/imgui_impl_sdlrenderer2.cpp"
    ${ROCKET_RSP_CXX})

if(WIN32)
    list(APPEND ROCKET_RUNTIME_SOURCES "${ROCKET_SOURCE_ROOT}/src/rocket-r.rc")
endif()

if(ANDROID)
    # SDLActivity loads libmain.so and calls its exported SDL_main entrypoint.
    add_library(RocketR SHARED ${ROCKET_RUNTIME_SOURCES})
    set_target_properties(RocketR PROPERTIES
        OUTPUT_NAME "main"
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
    add_executable(RocketR ${ROCKET_RUNTIME_SOURCES})
    set(ROCKET_EXECUTABLE_NAME "Rocket-R" CACHE STRING "Executable name for side-by-side development tests")
    if(WIN32)
        # Start as a GUI process without creating/flashing a console. Keep the
        # normal CRT main(argc, argv) startup and inherited redirected streams.
        set_target_properties(RocketR PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(RocketR PRIVATE /ENTRY:mainCRTStartup)
    endif()
    set_target_properties(RocketR PROPERTIES
        OUTPUT_NAME "${ROCKET_EXECUTABLE_NAME}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(RocketR PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}")
    endforeach()
endif()

target_include_directories(RocketR PRIVATE
    # Do not add the repository root here. On case-insensitive Windows, the
    # project VERSION file otherwise shadows the C++20 standard <version> header.
    "${ROCKET_SOURCE_ROOT}/src"
    "${ROCKET_SOURCE_ROOT}/generated"
    "${ROCKET_RUNTIME_DIR}/N64Recomp/include"
    "${ROCKET_RUNTIME_DIR}/ultramodern/include"
    "${ROCKET_RUNTIME_DIR}/librecomp/include"
    "${ROCKET_RUNTIME_DIR}/thirdparty"
    "${ROCKET_GENERATED_RSP}"
    "$<TARGET_PROPERTY:rt64,INCLUDE_DIRECTORIES>"
    "${ROCKET_RT64_DIR}/src"
    "${ROCKET_RT64_DIR}/src/include"
    "${ROCKET_RT64_DIR}/src/contrib"
    "${ROCKET_RT64_DIR}/src/contrib/hlslpp/include"
    "${ROCKET_RT64_DIR}/src/contrib/plume"
    "${ROCKET_RT64_DIR}/src/contrib/imgui"
    "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-core/src/api"
    "${ROCKET_RT64_DIR}/src/contrib/nativefiledialog-extended/src/include"
    "${CMAKE_BINARY_DIR}/src")

target_compile_definitions(RocketR PRIVATE
    RT64_SDL_WINDOW_VULKAN=1
    ROCKET_R_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ROCKET_R_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ROCKET_R_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ROCKET_R_VERSION="${ROCKET_R_VERSION}")
# librecomp's generated RSP vector implementation uses SSSE3/SSE4.1
# intrinsics on x86-64. clang-cl does not enable these intrinsics by default,
# so pass the target feature through its MSVC-compatible command line.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RocketR PRIVATE /clang:-msse4.1)
elseif(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(RocketR PRIVATE -msse4.1)
endif()

# Generated code first: GNU ld scans static archives from left to right.
target_link_libraries(RocketR PRIVATE RocketRecompiled ultramodern librecomp rt64 RocketModsCore)

if(WIN32)
    target_include_directories(RocketR PRIVATE
        "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/include")
    target_link_directories(RocketR PRIVATE
        "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
    target_link_libraries(RocketR PRIVATE SDL2 user32 shell32 comdlg32 dbghelp dxgi winmm)
    add_custom_command(TARGET RocketR POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:RocketR>/SDL2.dll"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:RocketR>/assets/ui"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROCKET_SOURCE_ROOT}/src/UI/Rocket-R-green-full-resolution.png"
            "$<TARGET_FILE_DIR:RocketR>/assets/ui/Rocket-R-green-full-resolution.png"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROCKET_RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "$<TARGET_FILE_DIR:RocketR>/dxcompiler.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROCKET_RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll"
            "$<TARGET_FILE_DIR:RocketR>/dxil.dll"
        VERBATIM)
elseif(ANDROID)
    if(NOT TARGET SDL2)
        message(FATAL_ERROR "Android build requires SDL2 to be added before Rocket-R. Use packaging/android/jni/CMakeLists.txt.")
    endif()
    target_link_libraries(RocketR PRIVATE SDL2 android log dl)
else()
    find_package(SDL2 REQUIRED)
    target_include_directories(RocketR PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(RocketR PRIVATE ${SDL2_LIBRARIES})
endif()

if(NOT ANDROID)
    add_executable(RocketControlsTests tests/controls_tests.cpp src/runtime_input.cpp src/binding_capture.cpp)
    target_include_directories(RocketControlsTests PRIVATE "${ROCKET_SOURCE_ROOT}/src")
    if(WIN32)
        target_include_directories(RocketControlsTests PRIVATE
            "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/include")
        target_link_directories(RocketControlsTests PRIVATE
            "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
        target_link_libraries(RocketControlsTests PRIVATE SDL2)
        add_custom_command(TARGET RocketControlsTests POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROCKET_RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:RocketControlsTests>/SDL2.dll")
    else()
        target_include_directories(RocketControlsTests PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(RocketControlsTests PRIVATE ${SDL2_LIBRARIES})
    endif()
    add_test(NAME controls COMMAND RocketControlsTests)
    set_tests_properties(controls PROPERTIES TIMEOUT 30)
    add_executable(RocketControlsUiTests tests/controls_ui_tests.cpp src/controls_studio.cpp src/runtime_input.cpp
        "${ROCKET_RT64_DIR}/src/contrib/imgui/imgui.cpp"
        "${ROCKET_RT64_DIR}/src/contrib/imgui/imgui_draw.cpp"
        "${ROCKET_RT64_DIR}/src/contrib/imgui/imgui_tables.cpp"
        "${ROCKET_RT64_DIR}/src/contrib/imgui/imgui_widgets.cpp")
    target_compile_definitions(RocketControlsUiTests PRIVATE IMGUI_ENABLE_TEST_ENGINE)
    target_include_directories(RocketControlsUiTests PRIVATE
        "$<TARGET_PROPERTY:RocketControlsTests,INCLUDE_DIRECTORIES>"
        "${ROCKET_RT64_DIR}/src/contrib/imgui")
    if(WIN32)
        target_link_directories(RocketControlsUiTests PRIVATE
            "$<TARGET_PROPERTY:RocketControlsTests,LINK_DIRECTORIES>")
    endif()
    target_link_libraries(RocketControlsUiTests PRIVATE "$<TARGET_PROPERTY:RocketControlsTests,LINK_LIBRARIES>")
    add_dependencies(RocketControlsUiTests RocketControlsTests)
    add_test(NAME controls_ui COMMAND RocketControlsUiTests)
    set_tests_properties(controls_ui PROPERTIES TIMEOUT 30)
endif()
