cmake_minimum_required(VERSION 3.24)
project(bl2vr LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Borderlands 2 is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(OPENXR_BUILD ${TP}/build-openxr CACHE PATH "OpenXR-SDK build directory (static loader)")

add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c
    ${TP}/minhook/src/hook.c
    ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/log.cpp
    src/config.cpp
    src/ue3.cpp
    src/d3d.cpp
    src/stereo.cpp
    src/vr.cpp
    src/hud.cpp
    src/xr.cpp
    src/xr_input.cpp
    src/gamepad.cpp
    src/d3d9.def)
target_include_directories(d3d9 PRIVATE ${TP}/OpenXR-SDK/include)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D12)
target_compile_options(d3d9 PRIVATE /W3 /permissive- /Zi)
target_link_options(d3d9 PRIVATE /DEBUG /OPT:REF /OPT:ICF /SAFESEH:NO)
target_link_libraries(d3d9 PRIVATE minhook
    ${OPENXR_BUILD}/src/loader/$<IF:$<CONFIG:Debug>,Debug/openxr_loaderd.lib,Release/openxr_loader.lib>
    dxgi dxguid advapi32 pathcch)
