cmake_minimum_required(VERSION 3.21)

add_executable(gltf2tg
    "gltf2tg/image.cpp"
    "gltf2tg/main.cpp")
target_link_libraries(gltf2tg
    cglm
    ktx
    nlohmann_json::nlohmann_json
    pugixml
    spdlog
    stb
    volk)
