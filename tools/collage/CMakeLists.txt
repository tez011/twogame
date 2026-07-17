cmake_minimum_required(VERSION 3.31)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resample.spv"
                   COMMAND $<TARGET_FILE:Vulkan::glslc> "-c" "--target-env=vulkan1.3" "-fshader-stage=compute" "-mfmt=c" "-O" "-o" "${CMAKE_CURRENT_BINARY_DIR}/resample.spv" "${CMAKE_CURRENT_SOURCE_DIR}/resample.glsl"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resample.glsl")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "/* Generated file - DO NOT EDIT */" > "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "#include <stdint.h>" > "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "const uint32_t resample_spv[] = " >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E cat "${CMAKE_CURRENT_BINARY_DIR}/resample.spv" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "; const uint32_t resample_spv_size = sizeof(resample_spv);" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/resample.spv"
                   VERBATIM)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/resample.c" PROPERTIES GENERATED TRUE)

add_executable(collage
    "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
    "image.cpp"
    "normals.cpp"
    "gltf.cpp"
    "collage.cpp")
target_link_libraries(collage
    fastgltf
    flatbuffers
    ktx
    mikktspace
    pugixml
    schema
    stb_image
    Vulkan::Vulkan
    Vulkan::volk
    VulkanMemoryAllocator)
