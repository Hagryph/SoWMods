cmake_minimum_required(VERSION 3.21)
project(HagIPC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Injected into the running ShadowOfWar.exe (see tools/inject.ps1) to expose a localhost TCP protocol
# for live RVA read/write/call/exec + runtime logging hooks (MinHook, src/ipc/HookProbe.*).
find_package(minhook CONFIG REQUIRED)

add_library(HagIPC SHARED
    src/dllmain.cpp
    src/Log.cpp
    src/Config.cpp
    src/ipc/Server.cpp
    src/ipc/MemAccess.cpp
    src/ipc/CallExec.cpp
    src/ipc/HookProbe.cpp
    src/ipc/MemScan.cpp
    src/HagIPCExport.cpp
)

# ../shared owns the raw offset table (GameOffsets.h); src has our headers.
target_include_directories(HagIPC PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../shared")
target_precompile_headers(HagIPC PRIVATE src/PCH.h)
target_compile_definitions(HagIPC PRIVATE UNICODE _UNICODE)
target_compile_options(HagIPC PRIVATE /utf-8)
target_link_libraries(HagIPC PRIVATE Ws2_32 minhook::minhook)

set_target_properties(HagIPC PROPERTIES PREFIX "" OUTPUT_NAME "HagIPC")
