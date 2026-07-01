cmake_minimum_required(VERSION 3.21)
project(SoWLoader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(minhook CONFIG REQUIRED)

# A near-dependency-free proxy DLL. All 796 steam_api64 exports are forwarded to the renamed
# original (steam_api64_org.dll) via /EXPORT: directives in src/exports_forward.h; our own code
# lives in DllMain + the worker thread, which installs a D3D11 Present hook (src/Overlay.*).
add_library(SoWLoader SHARED
    src/dllmain.cpp
    src/Loader.cpp
    src/Log.cpp
    src/Overlay.cpp
    src/GameHooks.cpp
    src/HagUI.cpp
    src/HagApiExport.cpp
)

# ../shared gives us GameOffsets.h once we start hooking; harmless to include now.
target_include_directories(SoWLoader PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../shared")
target_precompile_headers(SoWLoader PRIVATE src/PCH.h)
target_compile_definitions(SoWLoader PRIVATE UNICODE _UNICODE)
target_compile_options(SoWLoader PRIVATE /utf-8)
target_link_libraries(SoWLoader PRIVATE minhook::minhook d3d11 dxgi d3dcompiler)

# The output DLL MUST be named exactly steam_api64.dll (it impersonates the Steam API DLL).
set_target_properties(SoWLoader PROPERTIES PREFIX "" OUTPUT_NAME "steam_api64")

# The 796 forwarder exports come from #pragma comment(linker,"/EXPORT:...") in src/exports_forward.h
# (MSVC .def "name=other.name" forwarders don't link — LNK2001; /EXPORT directives do).
