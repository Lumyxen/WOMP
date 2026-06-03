set(output_dir "${CMAKE_ARGV3}")
set(shader_args "${CMAKE_ARGV4}")

if (NOT output_dir)
    message(FATAL_ERROR "missing output directory")
endif ()

if (NOT shader_args)
    message(FATAL_ERROR "missing shader list")
endif ()

string(REPLACE "|" ";" shaders "${shader_args}")

set(header "${output_dir}/womp/renderer/EmbeddedShaders.h")
set(source "${output_dir}/womp/renderer/EmbeddedShaders.cpp")

file(MAKE_DIRECTORY "${output_dir}/womp/renderer")

file(WRITE "${header}" [=[
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace womp {

struct EmbeddedShader {
    const std::uint32_t* code = nullptr;
    std::size_t size = 0;
};

EmbeddedShader embeddedShader(std::string_view name);

} // namespace womp
]=])

file(WRITE "${source}" [=[
#include "womp/renderer/EmbeddedShaders.h"

#include <stdexcept>
#include <string>

namespace womp {
namespace {

]=])

foreach (shader IN LISTS shaders)
    get_filename_component(shader_name "${shader}" NAME)
    string(MAKE_C_IDENTIFIER "${shader_name}" shader_id)
    file(READ "${shader}" shader_hex HEX)
    file(SIZE "${shader}" shader_size)

    math(EXPR remainder "${shader_size} % 4")
    if (NOT remainder EQUAL 0)
        message(FATAL_ERROR "SPIR-V shader is not 4-byte aligned: ${shader}")
    endif ()

    string(LENGTH "${shader_hex}" shader_hex_length)
    math(EXPR shader_hex_stop "${shader_hex_length} - 8")
    set(shader_words "")

    foreach (offset RANGE 0 ${shader_hex_stop} 8)
        string(SUBSTRING "${shader_hex}" ${offset} 2 byte0)
        math(EXPR byte_offset "${offset} + 2")
        string(SUBSTRING "${shader_hex}" ${byte_offset} 2 byte1)
        math(EXPR byte_offset "${offset} + 4")
        string(SUBSTRING "${shader_hex}" ${byte_offset} 2 byte2)
        math(EXPR byte_offset "${offset} + 6")
        string(SUBSTRING "${shader_hex}" ${byte_offset} 2 byte3)
        string(APPEND shader_words "0x${byte3}${byte2}${byte1}${byte0}, ")
    endforeach ()

    file(APPEND "${source}" "constexpr std::uint32_t ${shader_id}_words[] = {\n    ${shader_words}\n};\n\n")
endforeach ()

file(APPEND "${source}" [=[
} // namespace

EmbeddedShader embeddedShader(std::string_view name)
{
]=])

foreach (shader IN LISTS shaders)
    get_filename_component(shader_name "${shader}" NAME)
    string(MAKE_C_IDENTIFIER "${shader_name}" shader_id)
    file(SIZE "${shader}" shader_size)

    file(APPEND "${source}" "    if (name == \"${shader_name}\") {\n")
    file(APPEND "${source}" "        return {.code = ${shader_id}_words, .size = ${shader_size}};\n")
    file(APPEND "${source}" "    }\n\n")
endforeach ()

file(APPEND "${source}" [=[
    throw std::runtime_error("unknown embedded shader: " + std::string(name));
}

} // namespace womp
]=])
