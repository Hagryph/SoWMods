cmake_minimum_required(VERSION 3.21)
project(SampleMod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A standalone mod DLL: no external deps, no vcpkg — just Win32 + the shared/ headers, which it uses
# to talk to HagUI at runtime (resolved via GetProcAddress, never linked). Output = SampleMod.dll,
# dropped into <game>\x64\mods\ by mods/build_mods.ps1.
add_library(SampleMod SHARED src/SampleMod.cpp)

target_include_directories(SampleMod PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
target_compile_definitions(SampleMod PRIVATE UNICODE _UNICODE)
target_compile_options(SampleMod PRIVATE /utf-8)

set_target_properties(SampleMod PROPERTIES PREFIX "")   # SampleMod.dll (no lib prefix)
