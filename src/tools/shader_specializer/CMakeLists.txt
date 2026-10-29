cmake_minimum_required(VERSION 3.20)
project(shader_specializer)
set(CMAKE_CXX_STANDARD 17)

add_executable(shader_specializer "shader_specializer.cpp")
target_link_libraries(shader_specializer libzstd_static)
target_include_directories(shader_specializer PRIVATE ${PROJECT_SOURCE_DIR}/../../contrib/zstd/lib)

# Built with Emscripten, the tool runs in node at build time, as file_to_c does.
if (EMSCRIPTEN)
    target_link_options(shader_specializer PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -O1)
endif()
