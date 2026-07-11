cmake_minimum_required(VERSION 3.31)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resample.spv"
                   COMMAND $<TARGET_FILE:Vulkan::glslc> "-c" "--target-env=vulkan1.3" "-fshader-stage=compute" "-mfmt=c" "-O" "-o" "${CMAKE_CURRENT_BINARY_DIR}/resample.spv" "${CMAKE_CURRENT_SOURCE_DIR}/resample.glsl"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resample.glsl")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resample.h" "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "/* Generated file - DO NOT EDIT */" > "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "/* Generated file - DO NOT EDIT */" > "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "#include <stddef.h>" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#include <stdint.h>" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#ifdef __cplusplus" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "extern \"C\" {" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#endif" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "extern const uint32_t resample_spv[]; extern const size_t resample_spv_size;" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#ifdef __cplusplus" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "}" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#endif" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.h"
                   COMMAND ${CMAKE_COMMAND} -E echo "#include \"resample.h\"" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "const uint32_t resample_spv[] = " >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E cat "${CMAKE_CURRENT_BINARY_DIR}/resample.spv" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   COMMAND ${CMAKE_COMMAND} -E echo "; const size_t resample_spv_size = sizeof(resample_spv);" >> "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
                   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/resample.spv"
                   VERBATIM)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/resample.h" PROPERTIES GENERATED TRUE)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/resample.cpp" PROPERTIES GENERATED TRUE)
include_directories(AFTER "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(gltf2tg
    "${CMAKE_CURRENT_BINARY_DIR}/resample.c"
    "image.cpp"
    "normals.cpp"
    "gltf2tg.cpp")
target_link_libraries(gltf2tg
    cglm
    fastgltf
    flatbuffers
    ktx
    mikktspace
    schema
    stb_image
    Vulkan::Vulkan
    Vulkan::volk
    VulkanMemoryAllocator)
