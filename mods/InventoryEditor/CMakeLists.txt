cmake_minimum_required(VERSION 3.21)
project(InventoryEditor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(InventoryEditor SHARED src/InventoryEditor.cpp src/EarlyRingUnlock.cpp src/RingGrantGuard.cpp)
add_custom_target(CheckHeadlessMods COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/HeadlessMods.cmake" VERBATIM)
add_dependencies(InventoryEditor CheckHeadlessMods)

# Reuse the loader's existing static MinHook installation when not configured with a toolchain.
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../SoWLoader/build/vcpkg_installed/x64-windows-static-md")
find_package(minhook CONFIG REQUIRED)
target_link_libraries(InventoryEditor PRIVATE minhook::minhook)

target_include_directories(InventoryEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
target_compile_definitions(InventoryEditor PRIVATE UNICODE _UNICODE)
target_compile_options(InventoryEditor PRIVATE /utf-8)

set_target_properties(InventoryEditor PROPERTIES PREFIX "")   # InventoryEditor.dll

include(CTest)
if(BUILD_TESTING)
    add_executable(RingUnlockTests tests/RingUnlockTests.cpp)
    target_include_directories(RingUnlockTests PRIVATE src)
    add_test(NAME RingUnlockPolicy COMMAND RingUnlockTests)
endif()
