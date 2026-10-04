# Vendored libusb (LGPL-2.1-or-later, see COPYING) built for Android.
#
# libusb upstream ships autotools/ndk-build only; this is a minimal CMake
# target covering the Linux/Android (usbfs) backend.
add_library(libusb STATIC
    libusb/core.c
    libusb/descriptor.c
    libusb/hotplug.c
    libusb/io.c
    libusb/sync.c
    libusb/strerror.c
    libusb/os/linux_usbfs.c
    libusb/os/events_posix.c
    libusb/os/threads_posix.c
    libusb/os/linux_netlink.c
)

target_include_directories(libusb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb
)

# Provides <config.h> (the Android libusb configuration).
target_include_directories(libusb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)

target_compile_options(libusb PRIVATE -pthread -fvisibility=hidden -w)

if(ANDROID)
    target_link_libraries(libusb PUBLIC log)
endif()
