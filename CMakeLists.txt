cmake_minimum_required(VERSION 3.25)

project(womp
    VERSION 0.1.0
    DESCRIPTION "C++23 project"
    LANGUAGES C CXX
)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LIBRSVG REQUIRED IMPORTED_TARGET librsvg-2.0)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GSTREAMER_PBUTILS REQUIRED IMPORTED_TARGET gstreamer-pbutils-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
pkg_check_modules(GSTREAMER_TAG REQUIRED IMPORTED_TARGET gstreamer-tag-1.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_SHELL_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(COMPILED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(EMBEDDED_SHADER_HEADER "${GENERATED_DIR}/womp/renderer/EmbeddedShaders.h")
set(EMBEDDED_SHADER_SOURCE "${GENERATED_DIR}/womp/renderer/EmbeddedShaders.cpp")

add_custom_command(
    OUTPUT
        "${GENERATED_DIR}/xdg-shell-client-protocol.h"
        "${GENERATED_DIR}/xdg-shell-protocol.c"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_PROTOCOL}" "${GENERATED_DIR}/xdg-shell-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_PROTOCOL}" "${GENERATED_DIR}/xdg-shell-protocol.c"
    DEPENDS "${XDG_SHELL_PROTOCOL}"
    VERBATIM
)

set(SHADERS
    shaders/background.vert
    shaders/background.frag
    shaders/sdf.vert
    shaders/sdf.frag
    shaders/text.vert
    shaders/text.frag
    shaders/svg.vert
    shaders/svg.frag
)

foreach(SHADER IN LISTS SHADERS)
    get_filename_component(SHADER_NAME "${SHADER}" NAME)
    set(SPIRV "${COMPILED_SHADER_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT "${SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${COMPILED_SHADER_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o "${SPIRV}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}"
        VERBATIM
    )

    list(APPEND COMPILED_SHADERS "${SPIRV}")
endforeach()

add_custom_target(womp_shaders DEPENDS ${COMPILED_SHADERS})

list(JOIN COMPILED_SHADERS "|" EMBEDDED_SHADER_INPUTS)
add_custom_command(
    OUTPUT
        "${EMBEDDED_SHADER_HEADER}"
        "${EMBEDDED_SHADER_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake" "${GENERATED_DIR}" "${EMBEDDED_SHADER_INPUTS}"
    DEPENDS
        ${COMPILED_SHADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    VERBATIM
)

add_library(womp_core
    src/library/ImportService.cpp
    src/library/LibraryStore.cpp
    src/mpris/MprisService.cpp
    src/playback/AudioPlayer.cpp
    src/playback/PlaybackQueue.cpp
    src/playback/PlaybackStats.cpp
)

target_compile_features(womp_core PUBLIC cxx_std_23)
target_include_directories(womp_core PUBLIC
    include
    "${GENERATED_DIR}"
)
target_link_libraries(womp_core PUBLIC
    PkgConfig::SQLITE
    PkgConfig::GSTREAMER
    PkgConfig::GSTREAMER_PBUTILS
    PkgConfig::GSTREAMER_APP
    PkgConfig::GSTREAMER_VIDEO
    PkgConfig::GSTREAMER_TAG
    PkgConfig::GIO
    PkgConfig::CAIRO
)
set_target_properties(womp_core PROPERTIES CXX_EXTENSIONS OFF)

add_library(womp_scene
    src/scene/Primitive.cpp
)
target_compile_features(womp_scene PUBLIC cxx_std_23)
target_include_directories(womp_scene PUBLIC include)
set_target_properties(womp_scene PROPERTIES CXX_EXTENSIONS OFF)

add_executable(womp
    "${EMBEDDED_SHADER_SOURCE}"
    "${GENERATED_DIR}/xdg-shell-protocol.c"
    src/app/App.cpp
    src/main.cpp
    src/platform/WaylandWindow.cpp
    src/renderer/VulkanRenderer.cpp
)

add_dependencies(womp womp_shaders)
target_compile_features(womp PRIVATE cxx_std_23)
target_include_directories(womp PRIVATE include "${GENERATED_DIR}")
target_link_libraries(womp PRIVATE
    womp_core
    womp_scene
    Vulkan::Vulkan
    PkgConfig::WAYLAND
    PkgConfig::WAYLAND_CURSOR
    PkgConfig::FREETYPE
    PkgConfig::FONTCONFIG
    PkgConfig::LIBRSVG
    PkgConfig::CAIRO
)

set_target_properties(womp PROPERTIES
    CXX_EXTENSIONS OFF
)

include(CTest)

if(BUILD_TESTING)
    add_executable(womp_core_tests tests/CoreTests.cpp)
    target_compile_features(womp_core_tests PRIVATE cxx_std_23)
    target_link_libraries(womp_core_tests PRIVATE womp_core womp_scene)
    set_target_properties(womp_core_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME womp_core_tests COMMAND womp_core_tests)
endif()
