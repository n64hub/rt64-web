cmake_minimum_required(VERSION 3.20)
project(file_to_c)
set(CMAKE_CXX_STANDARD 17)

add_executable(file_to_c "file_to_c.cpp")

if (APPLE)
    set_target_properties(file_to_c PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../rt64.entitlements
    )
endif()

# Built with Emscripten, the tool runs in node at build time and reads and writes the host's files. It is linked at -O1,
# whatever the build's level: every shader waits for it, and Binaryen's whole-module optimizations at -O3 took 10-50 s
# of a fresh build, for a tool that runs a few dozen times.
if (EMSCRIPTEN)
    target_link_options(file_to_c PRIVATE -sNODERAWFS=1 -O1)
endif()
