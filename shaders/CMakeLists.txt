cmake_minimum_required(VERSION 3.21)

file(GLOB_RECURSE GLSL_SOURCE_FILES
     LIST_DIRECTORIES FALSE
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS
     *.glsl)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/resources/shaders")
    foreach(GLSL ${GLSL_SOURCE_FILES})
        cmake_path(REMOVE_EXTENSION GLSL LAST_ONLY OUTPUT_VARIABLE GLSL_NAME)
        set(SPV_OUTPUT "${CMAKE_SOURCE_DIR}/resources/shaders/${GLSL_NAME}.spv")
        list(APPEND SPV_OUTPUTS "${SPV_OUTPUT}")
        add_custom_command(OUTPUT "${SPV_OUTPUT}"
                           COMMAND $<TARGET_FILE:Vulkan::glslangValidator> -o "${SPV_OUTPUT}" -V "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL}" --quiet
                           DEPENDS ${GLSL})
    endforeach()
    add_custom_target(shaders DEPENDS ${SPV_OUTPUTS})
else()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders.pk2")
    foreach(GLSL ${GLSL_SOURCE_FILES})
        cmake_path(REMOVE_EXTENSION GLSL LAST_ONLY OUTPUT_VARIABLE GLSL_NAME)
        set(SPV_OUTPUT "${CMAKE_BINARY_DIR}/shaders.pk2/${GLSL_NAME}.spv")
        list(APPEND SPV_OUTPUTS "${SPV_OUTPUT}")
        add_custom_command(OUTPUT "${SPV_OUTPUT}"
                           COMMAND $<TARGET_FILE:Vulkan::glslangValidator> -o "${SPV_OUTPUT}" -V "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL}" --quiet
                           DEPENDS ${GLSL})
    endforeach()
    add_custom_target(shaders DEPENDS ${SPV_OUTPUTS})
endif()