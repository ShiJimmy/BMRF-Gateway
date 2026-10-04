cmake_minimum_required(VERSION 3.16)
project(bmrf_android)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ship real AMBE voice: build the vendored mbelib decoder into the core.
set(BMRF_USE_MBELIB ON CACHE BOOL "Enable vendored mbelib AMBE decoder" FORCE)

# Use the vendored DSDcc decoder for DMR (proven; replaces the custom demod).
set(BMRF_USE_DSDCC ON CACHE BOOL "Enable vendored DSDcc DMR decoder" FORCE)

# Shared native core (same sources as the desktop CMake build).
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core
    ${CMAKE_CURRENT_BINARY_DIR}/bmrf_core
)

# Let the core compile the RTL-SDR wrapper against the vendored librtlsdr.
target_compile_definitions(bmrf_core PUBLIC BMRF_HAVE_RTLSDR=1)
target_include_directories(bmrf_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/librtlsdr/include
)

# Vendored USB / RTL-SDR driver (Android only).
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/libusb
    ${CMAKE_CURRENT_BINARY_DIR}/libusb
)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/librtlsdr
    ${CMAKE_CURRENT_BINARY_DIR}/librtlsdr
)

add_library(bmrf SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../jni/bmrf_jni.cpp
)

target_include_directories(bmrf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/include
)

target_link_libraries(bmrf PRIVATE bmrf_core rtlsdr)

# Native audio sink (AAudio, Android 8.0+) and logcat logging.
if(ANDROID)
    target_link_libraries(bmrf PRIVATE aaudio log)
endif()
