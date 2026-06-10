cmake_minimum_required(VERSION 3.31)

set(FLATBUFFERS_SCHEMAS
    "asset.fbs")

set(GENERATED_FILES)

make_directory(${CMAKE_CURRENT_BINARY_DIR}/generated)
foreach(schema ${FLATBUFFERS_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${schema} NAME_WE)
    set(OUTPUT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/${SCHEMA_NAME}.fbs.hpp)
    add_custom_command(OUTPUT ${OUTPUT_HEADER}
                       COMMAND $<TARGET_FILE:flatc>
                               --cpp
                               --cpp-std c++17
                               --scoped-enums
                               --gen-object-api
                               --filename-suffix ""
                               --filename-ext "fbs.hpp"
                               -o ${CMAKE_CURRENT_BINARY_DIR}/generated
                               ${CMAKE_CURRENT_SOURCE_DIR}/${schema}
                       DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${schema}
                       COMMENT "Generating bindings for ${schema}"
                       VERBATIM)
    list(APPEND GENERATED_FILES ${OUTPUT_HEADER})
endforeach()

add_library(schema INTERFACE ${GENERATED_FILES})
target_include_directories(schema INTERFACE INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/generated)
