cmake_minimum_required(VERSION 3.22)
project(nfsug2 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Interpreter and HLE are hot paths even in debug builds.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O2)
endif ()
add_compile_options(-Wall -Wno-unused-function -fno-strict-aliasing -fvisibility=hidden)

file(GLOB_RECURSE NFSUG2_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hle/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/loader/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gfx/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp)

# Statically recompiled game code (tools/recomp): large and machine-generated,
# so skip debug info and always optimise.
file(GLOB NFSUG2_GENERATED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp)
set_source_files_properties(${NFSUG2_GENERATED} PROPERTIES COMPILE_OPTIONS "-g0;-O2;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")

add_library(nfsug2 SHARED jni_bridge.cpp ${NFSUG2_SOURCES})
target_include_directories(nfsug2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nfsug2 PRIVATE android log EGL GLESv3 aaudio)

# HLE exports register themselves through static constructors; keep every
# object file even if nothing references it directly.
target_link_options(nfsug2 PRIVATE -Wl,--no-gc-sections)
