cmake_minimum_required(VERSION 3.31)

add_executable(demo
    "main.cpp"
)
target_link_libraries(demo twogame)

if(MSVC)
target_compile_options(demo PUBLIC "/EHsc" "/GR-" "/W4")
else(MSVC)
target_compile_options(demo PUBLIC "-Wall" "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
endif(MSVC)
