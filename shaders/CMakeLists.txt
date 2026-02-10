cmake_minimum_required(VERSION 3.31)

set(SHADERS)
set(INTERNAL_SHADERS
    "basic.frag"
    "basic.vert")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(OPT_LEVEL "-O0")
else()
    set(OPT_LEVEL "-O")
endif()

foreach(GLSL ${SHADERS})
    list(APPEND SPV_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.spv")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.spv"
                       COMMAND $<TARGET_FILE:Vulkan::glslc> "-c" "--target-env=vulkan1.3" "${OPT_LEVEL}" "-o" "${CMAKE_BINARY_DIR}/shaders/${GLSL}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL}"
                       DEPENDS ${GLSL})
endforeach()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/shaders.pk2"
                   COMMAND ${CMAKE_COMMAND} -E tar c "${CMAKE_BINARY_DIR}/shaders.pk2" --format=zip -- "${SPV_OUTPUTS}"
                   DEPENDS ${SPV_OUTPUTS}
                   COMMAND_EXPAND_LISTS)
add_custom_target(shaders DEPENDS ${SPV_OUTPUTS}
                                  "${CMAKE_BINARY_DIR}/shaders.pk2")

foreach(GLSL ${INTERNAL_SHADERS})
    list(APPEND INTERNAL_SPV_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.internal.spv")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.internal.spv"
                       COMMAND $<TARGET_FILE:Vulkan::glslc> "-c" "--target-env=vulkan1.3" "-mfmt=c" "-O" "-o" "${CMAKE_BINARY_DIR}/shaders/${GLSL}.internal.spv" "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL}"
                       DEPENDS ${GLSL})
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h" "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp"
                   COMMAND ${CMAKE_COMMAND} -D "GROOT=${CMAKE_CURRENT_BINARY_DIR}"
                                            -D "SHADERS=${INTERNAL_SHADERS}"
                                            -P "${CMAKE_SOURCE_DIR}/shaders/embed_spv.cmake"
                   DEPENDS "${CMAKE_SOURCE_DIR}/shaders/embed_spv.cmake" ${INTERNAL_SPV_OUTPUTS}
                   VERBATIM)
add_library(embedded_shaders STATIC EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp")
target_include_directories(embedded_shaders INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h" PROPERTIES GENERATED TRUE)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp" PROPERTIES GENERATED TRUE)
