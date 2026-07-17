cmake_minimum_required(VERSION 3.31)

CPMAddPackage(NAME fastgltf
              VERSION 0.9.0
              GIT_REPOSITORY https://github.com/spnda/fastgltf.git
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "BUILD_SHARED_LIBS OFF")
CPMAddPackage(NAME mikktspace
              GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git
              GIT_TAG 3e895b49d05ea07e4c2133156cfa94369e19e409
              DOWNLOAD_ONLY YES)
CPMAddPackage(NAME pugixml
              VERSION 1.15
              GIT_REPOSITORY https://github.com/zeux/pugixml.git
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "BUILD_SHARED_LIBS OFF")
CPMAddPackage(NAME stb
              VERSION 2.30
              GIT_REPOSITORY https://github.com/nothings/stb.git
              GIT_TAG 013ac3beddff3dbffafd5177e7972067cd2b5083
              DOWNLOAD_ONLY YES)

if (mikktspace_ADDED)
    add_library(mikktspace OBJECT "${mikktspace_SOURCE_DIR}/mikktspace.c")
    target_include_directories(mikktspace PUBLIC "${mikktspace_SOURCE_DIR}")
endif()
if (stb_ADDED)
    add_library(stb_image INTERFACE)
    target_include_directories(stb_image INTERFACE "${stb_SOURCE_DIR}")
endif()

add_subdirectory(collage)
