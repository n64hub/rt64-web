cmake_minimum_required(VERSION 3.20)
project(rt64 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if (APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
endif()

option(RT64_STATIC "Build RT64 as a static library" OFF)


option(RT64_WGSL "Compile the shaders to WGSL for WebGPU, in place of DXIL, SPIR-V and Metal (always on with Emscripten)" OFF)
option(RT64_SDL_WINDOW_VULKAN "Build RT64 to expect an SDL Window outside of Windows" OFF)
option(RT64_PER_DRAW_TEXTURES "Bind textures per draw on every device, as devices without descriptor indexing (WebGPU) need; to test that path" OFF)
option(RT64_PACKED_RSP_INPUTS "Gather the RSP compute shaders' inputs into one buffer on every device, as WebGPU's 8 storage buffers per stage need; to test that path" OFF)
option(RT64_FLIPPED_RECT_HIGH_RES "Sample a high-res replacement on a rect drawn backwards on the texel the RDP picks, not one native pixel behind it (always on with Emscripten)" OFF)
option(RT64_UNEVEN_FRAME_RATES "Interpolate a game whose last frames took an uneven number of VIs (the slowest at most twice the fastest) at the rate of the fastest, not leave it uninterpolated (always on with Emscripten)" OFF)
option(RT64_GAME_FRAME_WORKLOADS "The workloads a game frame is drawn in (one per RSP task the game ends with a full sync) are interpolated as one frame, not each as a frame of its own, so a game that draws a frame in several tasks interpolates (always on with Emscripten)" OFF)
option(RT64_FREE_DISCARDED_TARGETS "Free the render targets of a framebuffer whose RAM the CPU overwrote once nothing draws to it again for a few seconds, so a game that moves its framebuffers (one per level) does not keep every one on the GPU (always on with Emscripten)" OFF)
option(RT64_INTERPOLATE_CAMERA_EYE "Interpolate a camera's eye between two game frames on the line between its two places, not by lerping the view's translation, which is the eye in the view's own axes (always on with Emscripten)" OFF)
option(RT64_SKIP_COLLAPSED_MATCHES "Never pair a transform that collapses what it draws (a zero determinant: a scale of 0, or a matrix of zeros) with one that does not, from one game frame to the next, so that an object the game hides is not interpolated between its last matrix and none (always on with Emscripten)" OFF)
option(RT64_SCISSOR_INSIDE_FRAMEBUFFER "Count an extended scissor reaching past the framebuffer's left or right edge only to that edge in the area a framebuffer pair drew in, so that what covers the frame's whole width (a projection RT64 keeps at its aspect, a fill it widens) is the same on a frame that draws under one (always on with Emscripten)" OFF)

if (NOT ${RT64_STATIC})
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

function(preprocess INFILE OUTFILE OPTIONS)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            add_custom_command(OUTPUT ${OUTFILE}
                COMMAND clang -x c -E -P ${INFILE} -o ${OUTFILE} ${OPTIONS}
                DEPENDS ${INFILE})
        else()
            add_custom_command(OUTPUT ${OUTFILE}
                COMMAND ${CMAKE_CXX_COMPILER} /Zs /EP ${INFILE} ${OPTIONS} > ${OUTFILE}
                DEPENDS ${INFILE})
        endif()
    else()
        add_custom_command(OUTPUT ${OUTFILE}
            COMMAND ${CMAKE_CXX_COMPILER} -x c -E -P ${INFILE} -o ${OUTFILE} ${OPTIONS}
            DEPENDS ${INFILE})
    endif()
endfunction()

if (WIN32)
    set (DXC "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
    add_compile_definitions(
        NOMINMAX
        __PRFCHWINTRIN_H
    )

    # Dependencies that must be next to the DLL.
    configure_file("${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll" "dxcompiler.dll" COPYONLY)
    configure_file("${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxil.dll" "dxil.dll" COPYONLY)
elseif (APPLE)
    if (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        # find zlib and include in dyld path
        find_library(ZLIB_LIBRARY NAMES z)
        get_filename_component(ZLIB_PATH ${ZLIB_LIBRARY} DIRECTORY)

        set (DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/x64:${ZLIB_PATH}" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc-macos")
        set (SPIRVCROSS "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/spirv-cross/lib/x64:$ENV{DYLD_LIBRARY_PATH}" "${PROJECT_SOURCE_DIR}/src/contrib/spirv-cross/bin/x64/spirv-cross")
    else()
        set (DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/arm64:${ZLIB_PATH}" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/arm64/dxc-macos")
        set (SPIRVCROSS "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/spirv-cross/lib/arm64:$ENV{DYLD_LIBRARY_PATH}" "${PROJECT_SOURCE_DIR}/src/contrib/spirv-cross/bin/arm64/spirv-cross")
    endif()
else()
    if (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc-linux")
    else()
        set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/arm64/dxc-linux")
    endif()
endif()

# WebGPU: dxc (on the host) compiles to SPIR-V with -D WEBGPU, naga turns that into WGSL, and plume's PlumeWGSL.cmake
# moves the push constants into the uniform buffer plume's WebGPU backend gives them. WEBGPU is what the shaders check
# (shaders/WebGPU.hlsli).
if (EMSCRIPTEN)
    set (RT64_WGSL ON)
    if (CMAKE_HOST_WIN32)
        set (DXC "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
    elseif (CMAKE_HOST_APPLE)
        set (DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/arm64/dxc-macos")
    else()
        set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc-linux")
    endif()
endif()
if (RT64_WGSL)
    find_program(RT64_NAGA_EXECUTABLE naga)
    if (NOT RT64_NAGA_EXECUTABLE)
        message(FATAL_ERROR "naga not found: install naga-cli (cargo install naga-cli) or set RT64_NAGA_EXECUTABLE.")
    endif()
    set (RT64_PLUME_WGSL "${PROJECT_SOURCE_DIR}/src/contrib/plume/examples/cmake/modules/PlumeWGSL.cmake")
endif()

set(ZSTD_LEGACY_SUPPORT OFF)
set(ZSTD_BUILD_STATIC ON)
set(ZSTD_BUILD_SHARED OFF)

add_subdirectory(src/tools/file_to_c)
add_subdirectory(src/contrib/re-spirv)
# A browser has no native file dialogs.
if (NOT EMSCRIPTEN)
    add_subdirectory(src/contrib/nativefiledialog-extended)
endif()
add_subdirectory(src/contrib/zstd/build/cmake)

if (CMAKE_SYSTEM_NAME MATCHES "Linux" AND RT64_SDL_WINDOW_VULKAN)
    set(PLUME_SDL_VULKAN_ENABLED TRUE)
elseif (APPLE)
    set(PLUME_APPLE_RETINA_ENABLED OFF)
endif()

add_subdirectory(src/contrib/plume)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Use /W4 for clang-cl, as -Wall maps to MSVC's /Wall which in turn maps to clang's -Weverything
    if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        add_compile_options(/W4)
    else()
        add_compile_options(-Wall -Wextra)
    endif()
    add_compile_options(-Wno-nullability-completeness -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-missing-field-initializers -Wno-switch -Wno-tautological-undefined-compare)
    # imgui currently triggers some compiler warnings on clang 19 for function casts which leads to a failed build, so disable that warning on clang 19+
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.0)
        add_compile_options(-Wno-cast-function-type-mismatch)
    endif()
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20.0)
        add_compile_options(-Wno-deprecated-literal-operator -Wno-nontrivial-memcall)
    endif()
endif()

set(ANDROID_PLATFORM android-24)
set(ANDROID_ABI arm64-v8a)

# For DXC
set (DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
# WebGPU has no descriptor indexing, so its shaders always bind textures per draw (TextureSampler.hlsli).
if (RT64_PER_DRAW_TEXTURES OR RT64_WGSL)
    list(APPEND DXC_COMMON_OPTS "-D" "PER_DRAW_TEXTURES")
endif()
# WebGPU guarantees 8 storage buffers per shader stage, so the RSP compute shaders read their inputs from one
# (RSPPackedInputs, rt64_rsp_processor.h).
if (RT64_PACKED_RSP_INPUTS OR RT64_WGSL)
    set (RT64_PACKED_RSP_INPUTS ON)
    list(APPEND DXC_COMMON_OPTS "-D" "PACKED_RSP_INPUTS")
endif()
# A high-res replacement on a rect drawn backwards (a negative dsdx or dtdy) sampled on the texel the RDP picks
# (RasterPS.hlsl): a fix for any game with a texture pack, behind an option since it changes what upstream draws.
if (RT64_FLIPPED_RECT_HIGH_RES OR RT64_WGSL)
    list(APPEND DXC_COMMON_OPTS "-D" "FLIPPED_RECT_HIGH_RES")
endif()
set (DXC_DXIL_OPTS "-Wno-ignored-attributes")
set (DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set (DXC_LB_OPTS "${DXC_COMMON_OPTS}" "-D" "LIBRARY" "-T" "lib_6_3")
set (DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_3")
set (DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_3" "-fvk-invert-y")
set (DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_3")
set (DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_3")
set (DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

# A shader's .hlsli and shared headers are in the depfile dxc writes with -MF, so a change to one rebuilds every shader
# that includes it. -MF only preprocesses, so the compile is a second command. Other generators (Visual Studio, Xcode)
# rebuild a shader only when its own file changes.
function(add_dxc_command OUTPUT FILENAME)
    if (CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        add_custom_command(OUTPUT ${OUTPUT}
            COMMAND ${DXC} ${ARGN} ${FILENAME} /Fo ${OUTPUT} -MF ${OUTPUT}.d
            COMMAND ${DXC} ${ARGN} ${FILENAME} /Fo ${OUTPUT}
            DEPENDS ${FILENAME}
            DEPFILE ${OUTPUT}.d)
    else()
        add_custom_command(OUTPUT ${OUTPUT}
            COMMAND ${DXC} ${ARGN} ${FILENAME} /Fo ${OUTPUT}
            DEPENDS ${FILENAME})
    endif()
endfunction()

function(build_shader_spirv_impl TARGETOBJ FILENAME TARGET_NAME OUTNAME)
    add_dxc_command(${OUTNAME}.spv ${FILENAME} ${DXC_SPV_OPTS} ${ARGN})
    add_custom_command(OUTPUT ${OUTNAME}.spirv.c
        COMMAND file_to_c ${OUTNAME}.spv ${TARGET_NAME}BlobSPIRV ${OUTNAME}.spirv.c ${OUTNAME}.spirv.h
        DEPENDS ${OUTNAME}.spv file_to_c
        BYPRODUCTS ${OUTNAME}.spirv.h)
    target_sources(${TARGETOBJ} PRIVATE ${OUTNAME}.spirv.c)
endfunction()

function(build_shader_wgsl_impl TARGETOBJ FILENAME TARGET_NAME OUTNAME)
    # WebGPU's clip space is D3D's, so the vertex shaders keep their y.
    set(OPTIONS ${ARGN})
    list(REMOVE_ITEM OPTIONS "-fvk-invert-y")
    add_dxc_command(${OUTNAME}.wgsl.spv ${FILENAME} ${DXC_SPV_OPTS} -D WEBGPU ${OPTIONS})
    add_custom_command(OUTPUT ${OUTNAME}.wgsl ${OUTNAME}.wgsl.c ${OUTNAME}.wgsl.h
        COMMAND ${CMAKE_COMMAND} -DNAGA=${RT64_NAGA_EXECUTABLE} -DINPUT=${OUTNAME}.wgsl.spv -DOUTPUT=${OUTNAME}.wgsl
            -DVAR_NAME=${TARGET_NAME}BlobWGSL -DOUTPUT_C=${OUTNAME}.wgsl.c -DOUTPUT_H=${OUTNAME}.wgsl.h -P ${RT64_PLUME_WGSL}
        DEPENDS ${OUTNAME}.wgsl.spv ${RT64_PLUME_WGSL})
    target_sources(${TARGETOBJ} PRIVATE ${OUTNAME}.wgsl.c)
endfunction()

function(build_shader_dxil_impl TARGETOBJ FILENAME TARGET_NAME OUTNAME)
    add_dxc_command(${OUTNAME}.dxil ${FILENAME} ${DXC_DXIL_OPTS} ${ARGN})
    add_custom_command(OUTPUT ${OUTNAME}.dxil.c
        COMMAND file_to_c ${OUTNAME}.dxil ${TARGET_NAME}BlobDXIL ${OUTNAME}.dxil.c ${OUTNAME}.dxil.h
        DEPENDS ${OUTNAME}.dxil file_to_c
        BYPRODUCTS ${OUTNAME}.dxil.h)
    target_sources(${TARGETOBJ} PRIVATE ${OUTNAME}.dxil.c)
endfunction()

function(build_shader_msl_impl TARGETOBJ FILENAME TARGET_NAME OUTNAME)
    add_dxc_command(${OUTNAME}.spv ${FILENAME} ${DXC_SPV_OPTS} ${ARGN})
    add_custom_command(OUTPUT ${OUTNAME}.metal
            COMMAND spirv_cross_msl ${OUTNAME}.spv ${OUTNAME}.metal
            DEPENDS ${OUTNAME}.spv spirv_cross_msl)
    add_custom_command(OUTPUT ${OUTNAME}.ir
            COMMAND xcrun -sdk macosx metal -o ${OUTNAME}.ir -c ${OUTNAME}.metal $<$<CONFIG:Debug>:-frecord-sources>
            DEPENDS ${OUTNAME}.metal)
    add_custom_command(OUTPUT ${OUTNAME}.metallib
            COMMAND xcrun -sdk macosx metallib ${OUTNAME}.ir -o ${OUTNAME}.metallib
            DEPENDS ${OUTNAME}.ir)
    add_custom_command(OUTPUT ${OUTNAME}.metal.c
            COMMAND file_to_c ${OUTNAME}.metallib ${TARGET_NAME}BlobMSL ${OUTNAME}.metal.c ${OUTNAME}.metal.h
            DEPENDS ${OUTNAME}.metallib file_to_c
            BYPRODUCTS ${OUTNAME}.metal.h)
    target_sources(${TARGETOBJ} PRIVATE ${OUTNAME}.metal.c)
endfunction()

function(build_shader TARGETOBJ SHADERNAME OPTIONS)
    set(FILENAME "${PROJECT_SOURCE_DIR}/${SHADERNAME}")
    if (${ARGC} GREATER 3)
        set(OUTNAME "${CMAKE_BINARY_DIR}/${ARGV3}")
    else()
        set(OUTNAME "${CMAKE_BINARY_DIR}/${SHADERNAME}")
    endif()
    # Get any optional compiler args passed to this function
    if (${ARGC} GREATER 4)
        set(EXTRA_ARGS "${ARGN}")
        list(REMOVE_AT EXTRA_ARGS 0)
    endif()
    cmake_path(GET OUTNAME STEM TARGET_NAME)
    cmake_path(GET OUTNAME PARENT_PATH OUTPUT_DIR)
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    if (RT64_WGSL)
        build_shader_wgsl_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
        return()
    endif()
    # Compile DXIL shader binaries if building on Windows
    if (WIN32)
        build_shader_dxil_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
    endif()
    # Compile Metal shader binaries if building on macOS
    if (APPLE)
        build_shader_msl_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
    endif()
    build_shader_spirv_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
endfunction()

function(build_shader_dxil TARGETOBJ SHADERNAME OPTIONS)
    set(FILENAME "${PROJECT_SOURCE_DIR}/${SHADERNAME}")
    if (${ARGC} GREATER 3)
        set(OUTNAME "${CMAKE_BINARY_DIR}/${ARGV3}")
    else()
        set(OUTNAME "${CMAKE_BINARY_DIR}/${SHADERNAME}")
    endif()
    # Get any optional compiler args passed to this function
    if (${ARGC} GREATER 4)
        set(EXTRA_ARGS "${ARGN}")
        list(REMOVE_AT EXTRA_ARGS 0)
    endif()
    cmake_path(GET OUTNAME STEM TARGET_NAME)
    cmake_path(GET OUTNAME PARENT_PATH OUTPUT_DIR)
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    # Compile DXIL shader binaries if building on Windows
    build_shader_dxil_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
endfunction()

function(build_shader_spirv TARGETOBJ SHADERNAME OPTIONS)
    set(FILENAME "${PROJECT_SOURCE_DIR}/${SHADERNAME}")
    if (${ARGC} GREATER 3)
        set(OUTNAME "${CMAKE_BINARY_DIR}/${ARGV3}")
    else()
        set(OUTNAME "${CMAKE_BINARY_DIR}/${SHADERNAME}")
    endif()
    # Get any optional compiler args passed to this function
    if (${ARGC} GREATER 4)
        set(EXTRA_ARGS "${ARGN}")
        list(REMOVE_AT EXTRA_ARGS 0)
    endif()
    cmake_path(GET OUTNAME STEM TARGET_NAME)
    cmake_path(GET OUTNAME PARENT_PATH OUTPUT_DIR)
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    if (RT64_WGSL)
        build_shader_wgsl_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
        return()
    endif()
    build_shader_spirv_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
endfunction()

function(build_shader_msl TARGETOBJ SHADERNAME OPTIONS)
    set(FILENAME "${PROJECT_SOURCE_DIR}/${SHADERNAME}")
    if (${ARGC} GREATER 3)
        set(OUTNAME "${CMAKE_BINARY_DIR}/${ARGV3}")
    else()
        set(OUTNAME "${CMAKE_BINARY_DIR}/${SHADERNAME}")
    endif()
    # Get any optional compiler args passed to this function
    if (${ARGC} GREATER 4)
        set(EXTRA_ARGS "${ARGN}")
        list(REMOVE_AT EXTRA_ARGS 0)
    endif()
    cmake_path(GET OUTNAME STEM TARGET_NAME)
    cmake_path(GET OUTNAME PARENT_PATH OUTPUT_DIR)
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    build_shader_msl_impl(${TARGETOBJ} ${FILENAME} ${TARGET_NAME} ${OUTNAME} ${OPTIONS} ${EXTRA_ARGS})
endfunction()

function(preprocess_shader TARGETOBJ SHADERNAME)
    set(FILENAME "${PROJECT_SOURCE_DIR}/${SHADERNAME}")
    set(OUTNAME "${CMAKE_BINARY_DIR}/${SHADERNAME}")
    cmake_path(GET SHADERNAME STEM TARGET_NAME)
    cmake_path(GET OUTNAME PARENT_PATH OUTPUT_DIR)
    preprocess(${FILENAME} ${OUTNAME}.rw ${DXC_COMMON_OPTS})
    add_custom_command(OUTPUT ${OUTNAME}.rw.c
        COMMAND file_to_c ${OUTNAME}.rw ${TARGET_NAME}Text ${OUTNAME}.rw.c ${OUTNAME}.rw.h
        DEPENDS ${OUTNAME}.rw file_to_c
        BYPRODUCTS ${OUTNAME}.rw.h)
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    target_sources(${TARGETOBJ} PRIVATE ${OUTNAME}.rw.c)
endfunction()

function(build_library_shader TARGETOBJ SHADERNAME)
    build_shader_dxil(${TARGETOBJ} ${SHADERNAME} "${DXC_LB_OPTS}" ${ARGN})
endfunction()

function(build_pixel_shader TARGETOBJ SHADERNAME)
    build_shader(${TARGETOBJ} ${SHADERNAME} "${DXC_PS_OPTS}" ${ARGN})
endfunction()

function(build_vertex_shader TARGETOBJ SHADERNAME)
    build_shader(${TARGETOBJ} ${SHADERNAME} "${DXC_VS_OPTS}" ${ARGN})
endfunction()

function(build_pixel_shader_spec_constants TARGETOBJ SHADERNAME)
    build_shader_spirv(${TARGETOBJ} ${SHADERNAME} "${DXC_PS_OPTS}" ${ARGN})
    if (APPLE)
        build_shader_msl(${TARGETOBJ} ${SHADERNAME} "${DXC_PS_OPTS}" ${ARGN})
    endif()
endfunction()

function(build_vertex_shader_spec_constants TARGETOBJ SHADERNAME)
    build_shader_spirv(${TARGETOBJ} ${SHADERNAME} "${DXC_VS_OPTS}" ${ARGN})
    if (APPLE)
        build_shader_msl(${TARGETOBJ} ${SHADERNAME} "${DXC_VS_OPTS}" ${ARGN})
    endif()
endfunction()

function(build_compute_shader TARGETOBJ SHADERNAME)
    build_shader(${TARGETOBJ} ${SHADERNAME} "${DXC_CS_OPTS}" ${ARGN})
endfunction()

function(build_ray_shader TARGETOBJ SHADERNAME)
    build_shader(${TARGETOBJ} ${SHADERNAME} "${DXC_RT_OPTS}" ${ARGN})
endfunction()

# Point cmake at src/contrib/mupen64plus-win32-deps/SDL2-2.26.3 for SDL2 on windows, look for an installed package on other systems
if (WIN32)
    set(SDL2_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/include")
    set(SDL2_LIBRARIES "SDL2" "SDL2main")
    link_directories("${PROJECT_SOURCE_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/lib/x64")
elseif (EMSCRIPTEN)
    # Emscripten's own port of SDL2.
    add_compile_options("-sUSE_SDL=2")
    set(SDL2_LIBRARIES "-sUSE_SDL=2")
    set(SDL2_INCLUDE_DIRS "${EMSCRIPTEN_SYSROOT}/include/SDL2")
else()
    find_package(SDL2 REQUIRED)
endif()

message(STATUS "${SDL2_INCLUDE_DIRS} ${SDL2_LIBRARIES}")

add_compile_definitions(
    HLSL_CPU
    FFX_GCC
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

if (CMAKE_SYSTEM_NAME MATCHES "Linux" AND RT64_SDL_WINDOW_VULKAN)
    add_compile_definitions("PLUME_SDL_VULKAN_ENABLED")
    add_compile_definitions("RT64_SDL_WINDOW_VULKAN")
endif()

set (SOURCES
    "${PROJECT_SOURCE_DIR}/src/common/rt64_common.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_dynamic_libraries.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_elapsed_timer.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_emulator_configuration.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_enhancement_configuration.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_filesystem_zip.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_load_types.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_mapped_file.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_math.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_profiling_timer.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_replacement_database.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_thread.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_timer.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_user_configuration.cpp"
    "${PROJECT_SOURCE_DIR}/src/common/rt64_user_paths.cpp"

    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3d.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dex.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dex2.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dgolden.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dpd.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dwave.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_f3dzex2.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_l3dex2.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_rdp.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_s2dex.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_s2dex2.cpp"
    "${PROJECT_SOURCE_DIR}/src/gbi/rt64_gbi_extended.cpp"

    "${PROJECT_SOURCE_DIR}/src/gui/rt64_camera_controller.cpp"
    "${PROJECT_SOURCE_DIR}/src/gui/rt64_debugger_inspector.cpp"
    "${PROJECT_SOURCE_DIR}/src/gui/rt64_file_dialog.cpp"
    "${PROJECT_SOURCE_DIR}/src/gui/rt64_inspector.cpp"

    "${PROJECT_SOURCE_DIR}/src/hle/rt64_application.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_application_window.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_color_converter.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_command_warning.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_draw_call.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_framebuffer.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_framebuffer_changes.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_framebuffer_manager.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_framebuffer_pair.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_framebuffer_storage.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_game_frame.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_interpreter.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_light_manager.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_present_queue.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_projection.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_rdp.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_rdp_tmem.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_rigid_body.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_rsp.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_state.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_vi.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_workload.cpp"
    "${PROJECT_SOURCE_DIR}/src/hle/rt64_workload_queue.cpp"

    "${PROJECT_SOURCE_DIR}/src/imgui/imgui_impl_sdl2_custom.cpp"

    "${PROJECT_SOURCE_DIR}/src/render/rt64_buffer_uploader.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_framebuffer_renderer.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_geometry_mode.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_look_at_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_native_target.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_optimus.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_projection_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_raster_shader.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_raster_shader_cache.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_render_target.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_render_target_manager.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_render_worker.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_rsp_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_shader_common.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_shader_compiler.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_shader_library.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_texture_cache.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_tile_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_transform_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_upscaler.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_vertex_processor.cpp"
    "${PROJECT_SOURCE_DIR}/src/render/rt64_vi_renderer.cpp"

    "${PROJECT_SOURCE_DIR}/src/preset/rt64_preset.cpp"
    "${PROJECT_SOURCE_DIR}/src/preset/rt64_preset_draw_call.cpp"
    "${PROJECT_SOURCE_DIR}/src/preset/rt64_preset_light.cpp"
    "${PROJECT_SOURCE_DIR}/src/preset/rt64_preset_material.cpp"
    "${PROJECT_SOURCE_DIR}/src/preset/rt64_preset_scene.cpp"

    "${PROJECT_SOURCE_DIR}/src/shared/rt64_hlsl_json.cpp"

    "${PROJECT_SOURCE_DIR}/src/rhi/rt64_render_hooks.cpp"

    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/imgui.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/imgui_demo.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/imgui_draw.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/imgui_tables.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/imgui_widgets.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui/backends/imgui_impl_vulkan.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/im3d/im3d.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/implot/implot.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/implot/implot_demo.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/implot/implot_items.cpp"
    "${PROJECT_SOURCE_DIR}/src/contrib/miniz/miniz.c"
)

include_directories(
    "${SDL2_INCLUDE_DIRS}"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/contrib"
    "${PROJECT_SOURCE_DIR}/src/contrib/plume"
    "${PROJECT_SOURCE_DIR}/src/contrib/imgui"
    "${PROJECT_SOURCE_DIR}/src/contrib/hlslpp/include"
    "${PROJECT_SOURCE_DIR}/src/contrib/mupen64plus-core/src/api"
    "${PROJECT_SOURCE_DIR}/src/contrib/nativefiledialog-extended/src/include"
)

if (WIN32)
    # DXC only needs to be included during runtime for Windows.
    include_directories("${PROJECT_SOURCE_DIR}/src/contrib/dxc/inc")
endif()

if (${RT64_STATIC})
    add_library(rt64 STATIC ${SOURCES})
else()
    add_library(rt64 SHARED ${SOURCES})
endif()

set_target_properties(rt64 PROPERTIES OUTPUT_NAME "rt64")
set_target_properties(rt64 PROPERTIES PREFIX "")

if (RT64_PER_DRAW_TEXTURES)
    target_compile_definitions(rt64 PUBLIC RT64_PER_DRAW_TEXTURES=1)
endif()

if (RT64_PACKED_RSP_INPUTS)
    target_compile_definitions(rt64 PUBLIC RT64_PACKED_RSP_INPUTS=1)
endif()
if (RT64_WGSL)
    target_compile_definitions(rt64 PUBLIC RT64_WGSL=1)
endif()
if (RT64_GAME_FRAME_WORKLOADS OR RT64_WGSL)
    target_compile_definitions(rt64 PUBLIC RT64_GAME_FRAME_WORKLOADS=1)
endif()
if (RT64_FREE_DISCARDED_TARGETS OR RT64_WGSL)
    target_compile_definitions(rt64 PRIVATE RT64_FREE_DISCARDED_TARGETS=1)
endif()
if (RT64_UNEVEN_FRAME_RATES OR RT64_WGSL)
    target_compile_definitions(rt64 PUBLIC RT64_UNEVEN_FRAME_RATES=1)
endif()
if (RT64_INTERPOLATE_CAMERA_EYE OR RT64_WGSL)
    target_compile_definitions(rt64 PRIVATE RT64_INTERPOLATE_CAMERA_EYE=1)
endif()
if (RT64_SKIP_COLLAPSED_MATCHES OR RT64_WGSL)
    target_compile_definitions(rt64 PRIVATE RT64_SKIP_COLLAPSED_MATCHES=1)
endif()
if (RT64_SCISSOR_INSIDE_FRAMEBUFFER OR RT64_WGSL)
    target_compile_definitions(rt64 PRIVATE RT64_SCISSOR_INSIDE_FRAMEBUFFER=1)
endif()

# Add common libraries.
target_link_libraries(rt64 re-spirv)
if (NOT EMSCRIPTEN)
    target_link_libraries(rt64 nfd)
endif()
target_link_libraries(rt64 libzstd_static)
target_link_libraries(rt64 plume)

# Add tools.
add_subdirectory(src/tools/texture_hasher)
add_subdirectory(src/tools/texture_packer)

# Add any Apple-specific source files and libraries
if (APPLE)
    add_subdirectory(src/tools/spirv_cross_msl)

    target_sources(rt64 PRIVATE
        "${PROJECT_SOURCE_DIR}/src/apple/rt64_apple.mm"
    )
    target_include_directories(rt64 PRIVATE "${PROJECT_SOURCE_DIR}/src/contrib/metal-cpp")
    target_link_libraries(rt64 "-framework Metal -framework QuartzCore -framework CoreGraphics -framework IOKit")
endif()

# Add any Windows-specific source files and libraries
if (WIN32)
    target_sources(rt64 PRIVATE
        "${PROJECT_SOURCE_DIR}/src/contrib/imgui/backends/imgui_impl_dx12.cpp"
        "${PROJECT_SOURCE_DIR}/src/contrib/imgui/backends/imgui_impl_win32.cpp"
    )
    target_link_libraries(rt64
        delayimp.lib
        D3D12
        DXGI
        Shcore.lib
        ${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/x64/dxcompiler.lib)

    target_include_directories(rt64 PRIVATE ${zstd_SOURCE_DIR}/lib)
endif()

target_link_directories(rt64 PRIVATE ${PROJECT_SOURCE_DIR}/src/contrib/dxc)

if (NOT ANDROID)
    target_link_libraries(rt64 ${SDL2_LIBRARIES})
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    if (NOT RT64_SDL_WINDOW_VULKAN)
        find_package(X11 REQUIRED)
        target_include_directories(rt64 PUBLIC ${X11_INCLUDE_DIR} ${X11_Xrandr_INCLUDE_PATH})
        target_link_libraries(rt64 ${X11_LIBRARIES} ${X11_Xrandr_LIB})
    endif()
endif()

preprocess_shader(rt64 "src/shaders/RenderParams.hlsli")

if (${WIN32})
    build_library_shader(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSLibrary.hlsl")
    build_library_shader(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSLibraryMS.hlsl" "-D MULTISAMPLING")
    build_library_shader(rt64 "src/shaders/RasterVS.hlsl" "src/shaders/RasterVSLibrary.hlsl")
endif()

build_pixel_shader(  rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSDynamic.hlsl" "-D DYNAMIC_RENDER_PARAMS")
build_pixel_shader(  rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSDynamicMS.hlsl" "-D DYNAMIC_RENDER_PARAMS" "-D MULTISAMPLING")
build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstant.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS")
build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantMS.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D MULTISAMPLING")
build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlat.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR")
build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatMS.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D MULTISAMPLING")
if (RT64_WGSL)
    # Dual-source blending is optional in WebGPU, so RasterPS also comes with it, for the devices that have it; the
    # variants above write one output (RasterPS.hlsl).
    build_pixel_shader(  rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSDynamicDualSource.hlsl" "-D DYNAMIC_RENDER_PARAMS" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader(  rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSDynamicMSDualSource.hlsl" "-D DYNAMIC_RENDER_PARAMS" "-D MULTISAMPLING" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantMSDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D MULTISAMPLING" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatMSDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D MULTISAMPLING" "-D DUAL_SOURCE_BLENDING")
    # The RasterPS the browser specializes at run time with override values (ShaderCacheWGSL, rt64_raster_shader.h)
    # keeps the ubershader's loops (SPEC_CONSTANT_LOOPS): nothing strips it of dead code before the browser parses all of
    # it, and Chrome parses a module on its GPU main thread, where every present waits behind it. The unrolled ones
    # above are what the prebuilt shaders are specialized from (spirv-opt), which removes what a shader doesn't use.
    # Unrolled it is 670 KB of WGSL, which Chrome 155 parsed in 683 ms and built pipelines from in 1.0-1.6 s each; with
    # the loops 250 KB, 149 ms and 0.29-0.46 s (a test page, six of GoldenEye's shaders, RTX 2070).
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatLoops.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D SPEC_CONSTANT_LOOPS")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatLoopsMS.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D SPEC_CONSTANT_LOOPS" "-D MULTISAMPLING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatLoopsDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D SPEC_CONSTANT_LOOPS" "-D DUAL_SOURCE_BLENDING")
    build_pixel_shader_spec_constants(rt64 "src/shaders/RasterPS.hlsl" "src/shaders/RasterPSSpecConstantFlatLoopsMSDualSource.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D SPEC_CONSTANT_LOOPS" "-D MULTISAMPLING" "-D DUAL_SOURCE_BLENDING")
endif()
build_vertex_shader( rt64 "src/shaders/RasterVS.hlsl" "src/shaders/RasterVSDynamic.hlsl" "-D DYNAMIC_RENDER_PARAMS")
build_vertex_shader_spec_constants( rt64 "src/shaders/RasterVS.hlsl" "src/shaders/RasterVSSpecConstant.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS")
build_vertex_shader_spec_constants( rt64 "src/shaders/RasterVS.hlsl" "src/shaders/RasterVSSpecConstantFlat.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR")
# A Vulkan device without descriptor indexing (an old GPU, lavapipe before Mesa 23) gets the framebuffer renderer's
# per-draw texture sets at run time (perDrawTextures() in rt64_descriptor_sets.h), so the SPIR-V of every shader that
# uses those sets is built for them too, as <name>PerDraw, and picked by the same test. A WGSL build has only those.
if (NOT RT64_WGSL)
    set(PER_DRAW "-D PER_DRAW_TEXTURES")
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSDynamicPerDraw.hlsl" "-D DYNAMIC_RENDER_PARAMS" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSDynamicMSPerDraw.hlsl" "-D DYNAMIC_RENDER_PARAMS" "-D MULTISAMPLING" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSSpecConstantPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSSpecConstantMSPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D MULTISAMPLING" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSSpecConstantFlatPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/RasterPSSpecConstantFlatMSPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" "-D MULTISAMPLING" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterVS.hlsl" "${DXC_VS_OPTS}" "src/shaders/RasterVSDynamicPerDraw.hlsl" "-D DYNAMIC_RENDER_PARAMS" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterVS.hlsl" "${DXC_VS_OPTS}" "src/shaders/RasterVSSpecConstantPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/RasterVS.hlsl" "${DXC_VS_OPTS}" "src/shaders/RasterVSSpecConstantFlatPerDraw.hlsl" "-D SPEC_CONSTANT_RENDER_PARAMS" "-D VERTEX_FLAT_COLOR" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "${DXC_PS_OPTS}" "src/shaders/PostBlendDitherNoiseAddPSPerDraw.hlsl" "-D ADD_MODE" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "${DXC_PS_OPTS}" "src/shaders/PostBlendDitherNoiseSubPSPerDraw.hlsl" "-D SUB_MODE" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "${DXC_PS_OPTS}" "src/shaders/PostBlendDitherNoiseSubNegativePSPerDraw.hlsl" "-D SUB_MODE" "-D NEGATIVE_MODE" ${PER_DRAW})
    build_shader_spirv(rt64 "src/shaders/DebugPS.hlsl" "${DXC_PS_OPTS}" "src/shaders/DebugPSPerDraw.hlsl" ${PER_DRAW})
endif()

# Prebuilt shaders for WebGPU. A game that sets RT64_PREBUILT_SHADER_LIST to a list of the shader descriptions it uses
# (one per line, five hex words: other mode L and H, color combiner L and H, flags, as its page prints them when opened
# with ?record-shaders) gets each one specialized here: spirv-opt (SPIRV-Tools) sets the spec constant SPIR-V's
# constants to the description, freezes them and optimizes what is left, naga turns that into WGSL, and
# shader_specializer puts the lot into rt64 compressed. RasterShaderCache builds them in every browser
# (startPrebuild()), with the game waiting: those above the list's first line reading "section <name>" (all of them
# without one) before it gets past its first frame, and each section after that when the game first draws with its
# shaders, a level at a time. They are made without MSAA, with RasterPS both with dual-source blending (.ps.wgsl) and
# without (.ps-single.wgsl).
# -O runs twice: once leaves much of what the constants decided (a small RasterPS 42 KB of WGSL against 15). Without
# --strip-debug naga names every value after dxc's debug names. re-spirv, which RT64 specializes with on Vulkan, can't
# take a loop the shader keeps, which the WebGPU pixel shaders have (TextureSampler.hlsli, RasterPS.hlsl), and on
# shaders without one spirv-opt alone comes within 1% of re-spirv followed by spirv-opt -O. Against re-spirv alone it
# makes a RasterPS about 45% smaller and descriptions that differ only in what RasterPS ignores the same, kept once
# (GoldenEye's 340: 624 different stages to 210, 103 MB of WGSL to 16), and Chrome builds them in about 60% of the time.
# pack moves the variables naga makes of SPIR-V's phis out of their functions, which Chrome parses about four times
# faster, on the thread that takes the page's presents (shader_specializer.cpp, privatizePhis()).
if (RT64_WGSL AND RT64_PREBUILT_SHADER_LIST)
    find_program(RT64_SPIRV_OPT_EXECUTABLE spirv-opt)
    if (NOT RT64_SPIRV_OPT_EXECUTABLE)
        message(FATAL_ERROR "spirv-opt not found: the prebuilt shaders need it. Install SPIRV-Tools or set RT64_SPIRV_OPT_EXECUTABLE.")
    endif()
    add_subdirectory(src/tools/shader_specializer)
    # A decal's pixel shader writes its depth bounds instead of reading the depth buffer (RasterPS.hlsl,
    # DECAL_DEPTH_BOUNDS), so that it draws in the pass that writes depth: SPIR-V only, for these.
    foreach(PREBUILT_DECAL_VARIANT "" "Flat" "DualSource" "FlatDualSource")
        set(PREBUILT_DECAL_DEFINES "-D" "SPEC_CONSTANT_RENDER_PARAMS" "-D" "DECAL_DEPTH_BOUNDS")
        if (PREBUILT_DECAL_VARIANT MATCHES "Flat")
            list(APPEND PREBUILT_DECAL_DEFINES "-D" "VERTEX_FLAT_COLOR")
        endif()
        if (PREBUILT_DECAL_VARIANT MATCHES "DualSource")
            list(APPEND PREBUILT_DECAL_DEFINES "-D" "DUAL_SOURCE_BLENDING")
        endif()
        add_dxc_command(${CMAKE_BINARY_DIR}/src/shaders/RasterPSSpecConstant${PREBUILT_DECAL_VARIANT}Decal.hlsl.wgsl.spv
            ${PROJECT_SOURCE_DIR}/src/shaders/RasterPS.hlsl ${DXC_SPV_OPTS} -D WEBGPU ${DXC_PS_OPTS} ${PREBUILT_DECAL_DEFINES})
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RT64_PREBUILT_SHADER_LIST})
    file(STRINGS ${RT64_PREBUILT_SHADER_LIST} PREBUILT_LINES REGEX "^[0-9a-fA-F]+( +[0-9a-fA-F]+)( +[0-9a-fA-F]+)( +[0-9a-fA-F]+)( +[0-9a-fA-F]+)")
    set(PREBUILT_DIR "${CMAKE_BINARY_DIR}/rt64_prebuilt_shaders")
    set(PREBUILT_SPIRV "${CMAKE_BINARY_DIR}/src/shaders")
    file(MAKE_DIRECTORY ${PREBUILT_DIR})
    set(PREBUILT_OUTPUTS)
    foreach(PREBUILT_LINE ${PREBUILT_LINES})
        string(REGEX MATCHALL "[0-9a-fA-F]+" PREBUILT_WORDS "${PREBUILT_LINE}")
        list(GET PREBUILT_WORDS 4 PREBUILT_FLAGS)
        string(JOIN "_" PREBUILT_KEY ${PREBUILT_WORDS})
        # The spec constants' ids are RT64's: other mode L and H, color combiner L and H, flags.
        set(PREBUILT_SPEC_CONSTANTS)
        set(PREBUILT_ID 0)
        foreach(PREBUILT_WORD ${PREBUILT_WORDS})
            math(EXPR PREBUILT_VALUE "0x${PREBUILT_WORD}" OUTPUT_FORMAT DECIMAL)
            list(APPEND PREBUILT_SPEC_CONSTANTS "${PREBUILT_ID}:${PREBUILT_VALUE}")
            math(EXPR PREBUILT_ID "${PREBUILT_ID} + 1")
        endforeach()
        string(JOIN " " PREBUILT_SPEC_CONSTANTS ${PREBUILT_SPEC_CONSTANTS})
        # The flags' fourth bit is smoothShade, and other mode L's bits 10 and 11 both set are a decal (ZMODE_DEC).
        math(EXPR PREBUILT_SMOOTH "(0x${PREBUILT_FLAGS} >> 3) & 1")
        list(GET PREBUILT_WORDS 0 PREBUILT_OTHER_MODE_L)
        math(EXPR PREBUILT_ZMODE "(0x${PREBUILT_OTHER_MODE_L} >> 10) & 3")
        set(PREBUILT_PS_DECAL "")
        if (PREBUILT_ZMODE EQUAL 3)
            set(PREBUILT_PS_DECAL "Decal")
        endif()
        if (PREBUILT_SMOOTH)
            set(PREBUILT_SPIRV_vs "${PREBUILT_SPIRV}/RasterVSSpecConstant.hlsl.wgsl.spv")
            set(PREBUILT_SPIRV_ps "${PREBUILT_SPIRV}/RasterPSSpecConstantDualSource${PREBUILT_PS_DECAL}.hlsl.wgsl.spv")
            set(PREBUILT_SPIRV_ps-single "${PREBUILT_SPIRV}/RasterPSSpecConstant${PREBUILT_PS_DECAL}.hlsl.wgsl.spv")
        else()
            set(PREBUILT_SPIRV_vs "${PREBUILT_SPIRV}/RasterVSSpecConstantFlat.hlsl.wgsl.spv")
            set(PREBUILT_SPIRV_ps "${PREBUILT_SPIRV}/RasterPSSpecConstantFlatDualSource${PREBUILT_PS_DECAL}.hlsl.wgsl.spv")
            set(PREBUILT_SPIRV_ps-single "${PREBUILT_SPIRV}/RasterPSSpecConstantFlat${PREBUILT_PS_DECAL}.hlsl.wgsl.spv")
        endif()
        set(PREBUILT_OUT "${PREBUILT_DIR}/${PREBUILT_KEY}")
        set(PREBUILT_COMMANDS)
        foreach(PREBUILT_STAGE vs ps ps-single)
            list(APPEND PREBUILT_COMMANDS
                COMMAND ${RT64_SPIRV_OPT_EXECUTABLE} --set-spec-const-default-value "${PREBUILT_SPEC_CONSTANTS}" --freeze-spec-const
                    --fold-spec-const-op-composite --strip-debug -O -O ${PREBUILT_SPIRV_${PREBUILT_STAGE}} -o ${PREBUILT_OUT}.${PREBUILT_STAGE}.spv
                COMMAND ${RT64_NAGA_EXECUTABLE} --input-kind spv --keep-coordinate-space ${PREBUILT_OUT}.${PREBUILT_STAGE}.spv ${PREBUILT_OUT}.${PREBUILT_STAGE}.wgsl)
        endforeach()
        add_custom_command(OUTPUT ${PREBUILT_OUT}.vs.wgsl ${PREBUILT_OUT}.ps.wgsl ${PREBUILT_OUT}.ps-single.wgsl
            ${PREBUILT_COMMANDS}
            DEPENDS ${PREBUILT_SPIRV_vs} ${PREBUILT_SPIRV_ps} ${PREBUILT_SPIRV_ps-single}
            VERBATIM)
        list(APPEND PREBUILT_OUTPUTS ${PREBUILT_OUT}.vs.wgsl ${PREBUILT_OUT}.ps.wgsl ${PREBUILT_OUT}.ps-single.wgsl)
    endforeach()
    add_custom_command(OUTPUT ${PREBUILT_DIR}/rt64_prebuilt_shaders.c
        COMMAND shader_specializer pack ${RT64_PREBUILT_SHADER_LIST} ${PREBUILT_DIR} ${PREBUILT_DIR}/rt64_prebuilt_shaders.c "${PROJECT_SOURCE_DIR}/src/render/rt64_prebuilt_shaders.h"
        DEPENDS shader_specializer ${RT64_PREBUILT_SHADER_LIST} ${PREBUILT_OUTPUTS})
    target_sources(rt64 PRIVATE ${PREBUILT_DIR}/rt64_prebuilt_shaders.c)
    target_compile_definitions(rt64 PRIVATE RT64_PREBUILT_SHADERS)
endif()
build_compute_shader(rt64 "src/shaders/FbChangesClearCS.hlsl")
build_pixel_shader(  rt64 "src/shaders/FbChangesDrawColorPS.hlsl")
build_pixel_shader(  rt64 "src/shaders/FbChangesDrawDepthPS.hlsl")
build_compute_shader(rt64 "src/shaders/FbReadAnyChangesCS.hlsl" "src/shaders/FbReadAnyChangesCS.hlsl" "-O0")
build_compute_shader(rt64 "src/shaders/FbReinterpretCS.hlsl" "src/shaders/FbReinterpretCS.hlsl" "-O0")
build_compute_shader(rt64 "src/shaders/FbReadAnyFullCS.hlsl" "src/shaders/FbReadAnyFullCS.hlsl" "-O0")
build_compute_shader(rt64 "src/shaders/FbWriteColorCS.hlsl" "src/shaders/FbWriteColorCS.hlsl" "-O0")
build_compute_shader(rt64 "src/shaders/FbWriteDepthCS.hlsl")
build_compute_shader(rt64 "src/shaders/FbWriteDepthCS.hlsl" "src/shaders/FbWriteDepthCSMS.hlsl" "-D MULTISAMPLING")
build_compute_shader(rt64 "src/shaders/GaussianFilterRGB3x3CS.hlsl")
build_compute_shader(rt64 "src/shaders/BoxFilterCS.hlsl")
build_compute_shader(rt64 "src/shaders/BicubicScalingCS.hlsl")
build_compute_shader(rt64 "src/shaders/HistogramAverageCS.hlsl")
build_compute_shader(rt64 "src/shaders/HistogramClearCS.hlsl")
build_compute_shader(rt64 "src/shaders/HistogramSetCS.hlsl")
build_compute_shader(rt64 "src/shaders/IdleCS.hlsl")
build_compute_shader(rt64 "src/shaders/LuminanceHistogramCS.hlsl")
build_pixel_shader(  rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "src/shaders/PostBlendDitherNoiseAddPS.hlsl" "-D ADD_MODE")
build_pixel_shader(  rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "src/shaders/PostBlendDitherNoiseSubPS.hlsl" "-D SUB_MODE")
build_pixel_shader(  rt64 "src/shaders/PostBlendDitherNoisePS.hlsl" "src/shaders/PostBlendDitherNoiseSubNegativePS.hlsl" "-D SUB_MODE" "-D NEGATIVE_MODE")
build_compute_shader(rt64 "src/shaders/RSPModifyCS.hlsl")
build_compute_shader(rt64 "src/shaders/RSPProcessCS.hlsl")
build_compute_shader(rt64 "src/shaders/RSPWorldCS.hlsl")
build_compute_shader(rt64 "src/shaders/RSPSmoothNormalCS.hlsl")
build_compute_shader(rt64 "src/shaders/RSPVertexTestZCS.hlsl")
build_compute_shader(rt64 "src/shaders/RSPVertexTestZCS.hlsl" "src/shaders/RSPVertexTestZCSMS.hlsl" "-D MULTISAMPLING")
build_pixel_shader(  rt64 "src/shaders/RtCopyColorToDepthPS.hlsl")
build_pixel_shader(  rt64 "src/shaders/RtCopyColorToDepthPS.hlsl" "src/shaders/RtCopyColorToDepth2XPS.hlsl" "-D SAMPLES_2X")
build_pixel_shader(  rt64 "src/shaders/RtCopyColorToDepthPS.hlsl" "src/shaders/RtCopyColorToDepth4XPS.hlsl" "-D SAMPLES_4X")
build_pixel_shader(  rt64 "src/shaders/RtCopyColorToDepthPS.hlsl" "src/shaders/RtCopyColorToDepth8XPS.hlsl" "-D SAMPLES_8X")
build_pixel_shader(  rt64 "src/shaders/RtCopyDepthToColorPS.hlsl")
build_pixel_shader(  rt64 "src/shaders/RtCopyDepthToColorPS.hlsl" "src/shaders/RtCopyDepthToColor2XPS.hlsl" "-D SAMPLES_2X")
build_pixel_shader(  rt64 "src/shaders/RtCopyDepthToColorPS.hlsl" "src/shaders/RtCopyDepthToColor4XPS.hlsl" "-D SAMPLES_4X")
build_pixel_shader(  rt64 "src/shaders/RtCopyDepthToColorPS.hlsl" "src/shaders/RtCopyDepthToColor8XPS.hlsl" "-D SAMPLES_8X")
build_pixel_shader(  rt64 "src/shaders/TextureCopyPS.hlsl")
build_compute_shader(rt64 "src/shaders/TextureDecodeCS.hlsl")
build_pixel_shader(  rt64 "src/shaders/TextureResolvePS.hlsl" "src/shaders/TextureResolveSamples2XPS.hlsl" "-D SAMPLES_2X")
build_pixel_shader(  rt64 "src/shaders/TextureResolvePS.hlsl" "src/shaders/TextureResolveSamples4XPS.hlsl" "-D SAMPLES_4X")
build_pixel_shader(  rt64 "src/shaders/TextureResolvePS.hlsl" "src/shaders/TextureResolveSamples8XPS.hlsl" "-D SAMPLES_8X")
build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSRegular.hlsl")
build_pixel_shader(  rt64 "src/shaders/VideoInterfacePS.hlsl" "src/shaders/VideoInterfacePSPixel.hlsl" "-D PIXEL_ANTIALIASING")
build_vertex_shader( rt64 "src/shaders/FullScreenVS.hlsl")
build_vertex_shader( rt64 "src/shaders/Im3DVS.hlsl")
build_pixel_shader(  rt64 "src/shaders/ComposePS.hlsl")
build_pixel_shader(  rt64 "src/shaders/DebugPS.hlsl")
build_pixel_shader(  rt64 "src/shaders/Im3DPS.hlsl")
build_pixel_shader(  rt64 "src/shaders/PostProcessPS.hlsl")

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/src)

