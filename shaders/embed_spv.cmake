cmake_minimum_required(VERSION 3.31)
set(HEADER_FILE "${GROOT}/embedded_shaders.h")
set(CPP_FILE    "${GROOT}/embedded_shaders.cpp")

file(WRITE "${HEADER_FILE}"
"// Generated file - DO NOT EDIT
#pragma once
#include <cstddef>
#include <cstdint>
namespace twogame::shaders {
")
file(WRITE "${CPP_FILE}"
"// Generated file - DO NOT EDIT
#include \"embedded_shaders.h\"
namespace twogame::shaders {
")

foreach(GLSL ${SHADERS})
    set(SPV "${GROOT}/${GLSL}.internal.spv")
    string(MAKE_C_IDENTIFIER ${GLSL} SHADER_NAME)

    file(READ "${SPV}" SPV_DATA)
    file(APPEND "${HEADER_FILE}" "extern const uint32_t ${SHADER_NAME}_spv[]; extern const size_t ${SHADER_NAME}_size;\n")
    file(APPEND "${CPP_FILE}" "const uint32_t ${SHADER_NAME}_spv[] = ${SPV_DATA}; const size_t ${SHADER_NAME}_size = sizeof(${SHADER_NAME}_spv);\n")
endforeach()
file(APPEND "${HEADER_FILE}" "}\n")
file(APPEND "${CPP_FILE}" "}\n")
