cmake_minimum_required(VERSION 3.16)
project(BMRF-Gateway VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BMRF_BUILD_TESTS "Build BMRF-Gateway unit tests" ON)
option(BMRF_BUILD_TOOLS "Build BMRF-Gateway command line tools" ON)

add_subdirectory(core)

if(BMRF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BMRF_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
