option(BMRF_USE_MBELIB "Enable the mbelib-backed AMBE decoder scaffold" OFF)
option(BMRF_USE_LIQUID "Use vendored liquid-dsp for DMR symbol timing recovery" ON)
option(BMRF_USE_DSDCC "Use the vendored DSDcc digital-voice decoder for DMR" OFF)

set(BMRF_CORE_SOURCES
    src/common/log.cpp
    src/common/sha256.cpp
    src/dmr/call.cpp
    src/dmr/lc_encode.cpp
    src/dmr/event.cpp
    src/dmr/event_bus.cpp
    src/dmr/call_manager.cpp
    src/brandmeister/homebrew.cpp
    src/brandmeister/rx.cpp
    src/brandmeister/tx.cpp
    src/brandmeister/transport.cpp
    src/brandmeister/client.cpp
    src/brandmeister/uplink.cpp
    src/capture/capture.cpp
    src/vocoder/ambe_decoder.cpp
    src/audio/jitter_buffer.cpp
    src/audio/audio_output.cpp
    src/audio/voice_pipeline.cpp
    src/rf/dmr_sync.cpp
    src/rf/fec.cpp
    src/rf/rf_decoder.cpp
    src/rf/sample_source.cpp
    src/rf/burst_source.cpp
    src/rf/rf_receiver.cpp
    src/rf/rtl_sdr.cpp
    src/rf/fsk_demod.cpp
    src/rf/dmr_phy.cpp
    src/rf/aligned_demod.cpp
    src/rf/rtl_sdr_burst_source.cpp
)

if(BMRF_USE_LIQUID)
    list(APPEND BMRF_CORE_SOURCES src/rf/liquid_demod.cpp)
endif()

if(BMRF_USE_MBELIB)
    list(APPEND BMRF_CORE_SOURCES src/vocoder/mbelib_decoder.cpp)
endif()

if(BMRF_USE_DSDCC)
    list(APPEND BMRF_CORE_SOURCES src/rf/dsdcc_demod.cpp)
endif()

add_library(bmrf_core STATIC ${BMRF_CORE_SOURCES})

target_include_directories(bmrf_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(bmrf_core PUBLIC cxx_std_17)

if(BMRF_USE_MBELIB)
    target_compile_definitions(bmrf_core PUBLIC BMRF_USE_MBELIB=1)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mbelib
        ${CMAKE_CURRENT_BINARY_DIR}/mbelib)
    target_link_libraries(bmrf_core PUBLIC mbelib)
endif()

if(BMRF_USE_LIQUID)
    target_compile_definitions(bmrf_core PUBLIC BMRF_HAVE_LIQUID=1)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/liquid-dsp
        ${CMAKE_CURRENT_BINARY_DIR}/liquid)
    target_link_libraries(bmrf_core PUBLIC liquid)
endif()

if(WIN32)
    target_link_libraries(bmrf_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(bmrf_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(bmrf_core PRIVATE -Wall -Wextra)
endif()

# Vendored DSDcc (GPL-3.0): proven DMR digital-voice decoder (symbol sync, FEC,
# AMBE). Input is FM-discriminated S16LE audio at 48 kS/s. Uses our vendored
# mbelib for AMBE synthesis.
if(BMRF_USE_DSDCC)
    if(NOT BMRF_USE_MBELIB)
        message(FATAL_ERROR "BMRF_USE_DSDCC requires BMRF_USE_MBELIB=ON")
    endif()
    set(LIBMBE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mbelib")
    set(LIBMBE_LIBRARY mbelib)
    set(USE_MBELIB ON)
    set(BUILD_TOOL OFF)
    cmake_policy(SET CMP0077 NEW) # honor the BUILD_TOOL=OFF above
    list(APPEND CMAKE_MODULE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dsdcc/cmake/Modules")
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dsdcc"
        "${CMAKE_CURRENT_BINARY_DIR}/dsdcc")
    target_compile_definitions(bmrf_core PUBLIC BMRF_USE_DSDCC=1)
    target_include_directories(bmrf_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dsdcc")
    target_link_libraries(bmrf_core PUBLIC dsdcc)
endif()
