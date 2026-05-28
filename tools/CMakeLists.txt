cmake_minimum_required(VERSION 3.31)

set(SHADERS
    "gltf2tg/resample.comp")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(OPT_LEVEL "-O0")
else()
    set(OPT_LEVEL "-O")
endif()

foreach(GLSL ${SHADERS})
    list(APPEND SPV_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.internal.spv")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.internal.spv"
                       COMMAND $<TARGET_FILE:Vulkan::glslc> "-c" "--target-env=vulkan1.3" "-mfmt=c" "${OPT_LEVEL}" "-g" "-o" "${CMAKE_CURRENT_BINARY_DIR}/${GLSL}.internal.spv" "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL}"
                       DEPENDS ${GLSL})
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h" "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp"
                   COMMAND ${CMAKE_COMMAND} -D "GROOT=${CMAKE_CURRENT_BINARY_DIR}"
                                            -D "SHADERS=${SHADERS}"
                                            -P "${CMAKE_SOURCE_DIR}/shaders/embed_spv.cmake"
                   DEPENDS "${CMAKE_SOURCE_DIR}/shaders/embed_spv.cmake" ${SPV_OUTPUTS}
                   VERBATIM)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h" PROPERTIES GENERATED TRUE)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp" PROPERTIES GENERATED TRUE)
include_directories(AFTER "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(gltf2tg
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.cpp"
    "gltf2tg/image.cpp"
    "gltf2tg/main.cpp")
target_link_libraries(gltf2tg
    ktx
    stb_image
    Vulkan::Vulkan
    Vulkan::volk
    VulkanMemoryAllocator)
