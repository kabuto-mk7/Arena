cmake_minimum_required(VERSION 3.20)
project(ArenaFpsNetworkingDemo LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # Make distributable Windows binaries self-contained (no VC++ redistributable required).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
)
FetchContent_MakeAvailable(raylib)

set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.4
)
FetchContent_MakeAvailable(assimp)

add_executable(arena_server
    src/server.cpp
)

add_executable(arena_client
    src/client.cpp
)

if (WIN32)
    target_link_libraries(arena_server PRIVATE ws2_32 assimp::assimp)
    target_link_libraries(arena_client PRIVATE ws2_32 raylib assimp::assimp)
else()
    message(FATAL_ERROR "This demo currently targets Windows because the networking layer uses Winsock.")
endif()
