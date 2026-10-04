# Vendored librtlsdr (GPL-2.0-or-later, see COPYING) built for Android.
#
# A small Android patch is applied directly to src/librtlsdr.c: when a file
# descriptor is provided via rtlsdr_set_android_fd(), the driver wraps it with
# libusb_wrap_sys_device() instead of enumerating devices (required because
# Android's USB stack is not directly accessible from native code).
add_library(rtlsdr STATIC
    src/librtlsdr.c
    src/tuner_e4k.c
    src/tuner_fc0012.c
    src/tuner_fc0013.c
    src/tuner_fc2580.c
    src/tuner_r82xx.c
)

target_include_directories(rtlsdr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(rtlsdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rtlsdr PUBLIC libusb)

target_compile_options(rtlsdr PRIVATE -w)

if(ANDROID)
    target_link_libraries(rtlsdr PUBLIC log)
endif()
