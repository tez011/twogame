cmake_minimum_required(VERSION 3.31)

set(RESOURCE_PATH ${CMAKE_BINARY_DIR}/$<CONFIG>/d0.pk2)
add_custom_command(OUTPUT "${RESOURCE_PATH}/duck.tgs"
                   COMMAND $<TARGET_FILE:collage> -o "${RESOURCE_PATH}/duck.tgs" ${CMAKE_CURRENT_SOURCE_DIR}/duck.xml
                   DEPENDS collage ${CMAKE_CURRENT_SOURCE_DIR}/duck.xml)
add_custom_command(OUTPUT "${RESOURCE_PATH}/fox.tgs"
                   COMMAND $<TARGET_FILE:collage> -o "${RESOURCE_PATH}/fox.tgs" ${CMAKE_CURRENT_SOURCE_DIR}/fox.xml
                   DEPENDS collage ${CMAKE_CURRENT_SOURCE_DIR}/fox.xml)
add_custom_command(OUTPUT "${RESOURCE_PATH}/bars.tgs"
                   COMMAND $<TARGET_FILE:collage> -o "${RESOURCE_PATH}/bars.tgs" ${CMAKE_CURRENT_SOURCE_DIR}/bars.xml
                   DEPENDS collage ${CMAKE_CURRENT_SOURCE_DIR}/bars.xml)
add_custom_command(OUTPUT "${RESOURCE_PATH}/sponza_base.tgs"
                   COMMAND $<TARGET_FILE:collage> -o "${RESOURCE_PATH}/sponza_base.tgs" ${CMAKE_CURRENT_SOURCE_DIR}/sponza.xml
                   DEPENDS collage ${CMAKE_CURRENT_SOURCE_DIR}/sponza.xml)

add_custom_target(test_resources
                  DEPENDS "${RESOURCE_PATH}/duck.tgs"
                          "${RESOURCE_PATH}/fox.tgs"
                          "${RESOURCE_PATH}/bars.tgs"
                          "${RESOURCE_PATH}/sponza_base.tgs")
