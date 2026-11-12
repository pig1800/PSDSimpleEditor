cmake_minimum_required(VERSION 3.25)

# Static Qt (x64-windows-static) -> static CRT, one self-contained exe.
if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(PSDSimpleEditor VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)

option(PSE_BUILD_GUI "Build the Qt editor (off: only the PSD core and psdcheck)" ON)

# ---- PSD core: parser + byte-copy writer, no Qt -------------------------------------------------
add_library(psd_core STATIC
    src/psd/FileIO.h src/psd/FileIO.cpp
    src/psd/PsdFile.h src/psd/PsdFile.cpp
    src/psd/PsdWriter.h src/psd/PsdWriter.cpp
    src/psd/Codepage.h src/psd/Codepage.cpp)
target_include_directories(psd_core PUBLIC src)

add_executable(psdcheck tools/psdcheck.cpp)
target_link_libraries(psdcheck PRIVATE psd_core)

if(NOT PSE_BUILD_GUI)
    return()
endif()

# ---- Shaders: dxc (Windows SDK) -> DXIL byte arrays in generated headers ------------------------
file(GLOB PSE_DXC_CANDIDATES "$ENV{WindowsSdkVerBinPath}x64/dxc.exe"
     "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
list(SORT PSE_DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
find_program(PSE_DXC NAMES dxc HINTS ${PSE_DXC_CANDIDATES} NO_DEFAULT_PATH)
if(NOT PSE_DXC AND PSE_DXC_CANDIDATES)
    list(GET PSE_DXC_CANDIDATES 0 PSE_DXC)
endif()
if(NOT PSE_DXC)
    message(FATAL_ERROR "dxc.exe (Windows SDK) not found")
endif()

set(PSE_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${PSE_SHADER_DIR}")
set(PSE_SHADER_HEADERS)
# pse_shader(<array name> <hlsl> <profile> <entry> [defines...])
function(pse_shader name hlsl profile entry)
    set(defs)
    foreach(d ${ARGN})
        list(APPEND defs -D ${d})
    endforeach()
    set(header "${PSE_SHADER_DIR}/${name}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${PSE_DXC}" -nologo -T ${profile} -E ${entry} ${defs} -O3 -Fh "${header}" -Vn ${name}
                "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl}" VERBATIM COMMENT "dxc ${name}")
    set(PSE_SHADER_HEADERS ${PSE_SHADER_HEADERS} "${header}" PARENT_SCOPE)
endfunction()

pse_shader(kClearCs   shaders/decode.hlsl cs_6_0 pp_clear  PP_CLEAR)
pse_shader(kUnpackCs  shaders/decode.hlsl cs_6_0 pp_unpack PP_UNPACK)
pse_shader(kMergeCs   shaders/decode.hlsl cs_6_0 pp_merge  PP_MERGE)
pse_shader(kResizeCs  shaders/decode.hlsl cs_6_0 pp_resize PP_RESIZE)
foreach(taps 8 12 16 24)
    pse_shader(kResize${taps}Cs shaders/decode.hlsl cs_6_0 pp_resize PP_RESIZE FIXED=${taps})
endforeach()
pse_shader(kPresentVs shaders/present.hlsl vs_6_0 vs_main)
pse_shader(kPresentPs shaders/present.hlsl ps_6_0 ps_main)
add_custom_target(pse_shaders DEPENDS ${PSE_SHADER_HEADERS} SOURCES shaders/decode.hlsl shaders/present.hlsl)

# ---- GPU decode + display (no Qt) -----------------------------------------------------------------
find_package(ZLIB REQUIRED)
add_library(pse_gpu STATIC
    src/gpu/Gpu.h src/gpu/Gpu.cpp
    src/gpu/LayerDecoder.h src/gpu/LayerDecoder.cpp)
add_dependencies(pse_gpu pse_shaders)
target_include_directories(pse_gpu PRIVATE "${PSE_SHADER_DIR}")
target_link_libraries(pse_gpu PUBLIC psd_core ZLIB::ZLIB d3d12 dxgi dxguid)

# decodetest: the GPU decoder's output as a PAM file, for checking against a reference decoder.
add_executable(decodetest tools/decodetest.cpp)
target_link_libraries(decodetest PRIVATE pse_gpu)

# ---- Editor ---------------------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

qt_add_executable(PSDSimpleEditor WIN32
    src/main.cpp
    src/MainWindow.h src/MainWindow.cpp
    src/Document.h src/Document.cpp
    src/LayerTree.h src/LayerTree.cpp
    src/Canvas.h src/Canvas.cpp)
target_link_libraries(PSDSimpleEditor PRIVATE pse_gpu Qt6::Widgets Qt6::Concurrent)
