cmake_minimum_required(VERSION 3.21)
project(InventoryEditor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone mod DLL: reads InventoryEditor.catalog (deployed beside it) and surfaces the SoW item
# catalog in HagUI. No external deps - Win32 + the shared/ headers (HagUI resolved at runtime).
add_library(InventoryEditor SHARED src/InventoryEditor.cpp)

target_include_directories(InventoryEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
target_compile_definitions(InventoryEditor PRIVATE UNICODE _UNICODE)
target_compile_options(InventoryEditor PRIVATE /utf-8)

set_target_properties(InventoryEditor PROPERTIES PREFIX "")   # InventoryEditor.dll
