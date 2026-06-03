cmake_minimum_required(VERSION 3.25)

project(womp
    VERSION 0.1.0
    DESCRIPTION "C++23 project"
    LANGUAGES C CXX
)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_SHELL_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(COMPILED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

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

add_executable(womp
    "${GENERATED_DIR}/xdg-shell-protocol.c"
    src/app/App.cpp
    src/main.cpp
    src/platform/WaylandWindow.cpp
    src/renderer/VulkanRenderer.cpp
)

add_dependencies(womp womp_shaders)

target_compile_features(womp PRIVATE cxx_std_23)

target_include_directories(womp PRIVATE
    include
    "${GENERATED_DIR}"
)

target_compile_definitions(womp PRIVATE
    WOMP_SHADER_DIR="${COMPILED_SHADER_DIR}"
)

target_link_libraries(womp PRIVATE
    Vulkan::Vulkan
    PkgConfig::WAYLAND
)

set_target_properties(womp PROPERTIES
    CXX_EXTENSIONS OFF
)
