cmake_minimum_required(VERSION 3.20)
project(ArenaFpsNetworkingDemo LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
)
FetchContent_MakeAvailable(raylib)

add_executable(arena_server
    src/server.cpp
)

add_executable(arena_client
    src/client.cpp
)

if (WIN32)
    target_link_libraries(arena_server PRIVATE ws2_32)
    target_link_libraries(arena_client PRIVATE ws2_32 raylib)
else()
    message(FATAL_ERROR "This demo currently targets Windows because the networking layer uses Winsock.")
endif()
