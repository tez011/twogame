cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0054 NEW)
project("twogame")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
include_directories(BEFORE ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/lib/include)
message("CMake build type: ${CMAKE_BUILD_TYPE}")
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/prefs)

# ---- Global compiler options ----
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    foreach (flag_var
             CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
             CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
             CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
             CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REPLACE "/W3" "/W4" ${flag_var} "${${flag_var}}")
        string(REPLACE "/GR" "" ${flag_var} "${${flag_var}}")
    endforeach()
    add_compile_options(
        /wd4267
        $<$<CONFIG:DEBUG>:/Od>
        $<$<CONFIG:DEBUG>:/Zi>
        $<$<CONFIG:RELEASE>:/O2>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    foreach (flag_var
             CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
             CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
             CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
             CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REPLACE "-frtti" "" ${flag_var} "${${flag_var}}")
    endforeach()
    add_compile_options(
        $<$<CONFIG:DEBUG>:-O0>
        $<$<CONFIG:DEBUG>:-ggdb>
        $<$<CONFIG:RELEASE>:-O3>)
endif()
add_compile_definitions(
    $<$<CONFIG:DEBUG>:TWOGAME_DEBUG_BUILD>
    TWOGAME_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    TWOGAME_BINARY_ROOT="${CMAKE_BINARY_DIR}"
)

# ---- CPM ----
set(CPM_DOWNLOAD_VERSION 0.38.0)
if(CPM_SOURCE_CACHE)
    # Expand relative path. This is important if the provided path contains a tilde (~)
    get_filename_component(CPM_SOURCE_CACHE ${CPM_SOURCE_CACHE} ABSOLUTE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
    message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
    file(DOWNLOAD https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake ${CPM_DOWNLOAD_LOCATION})
endif()
include(${CPM_DOWNLOAD_LOCATION})

# ---- External dependencies ----
find_package(SDL2 REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(lib)
add_subdirectory(shaders)
add_subdirectory(src)
