# Vendored mbelib (ISC-style license, see COPYRIGHT).
# Only the AMBE/IMBE decode sources required by BMRF-Gateway are built.
add_library(mbelib STATIC
    mbelib.c
    ambe3600x2400.c
    ambe3600x2450.c
    imbe7100x4400.c
    imbe7200x4400.c
    ecc.c
)

target_include_directories(mbelib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Third-party C code is not warning-clean; keep our own -Wall/-Wextra meaningful.
if(MSVC)
    target_compile_options(mbelib PRIVATE /w)
else()
    target_compile_options(mbelib PRIVATE -w)
endif()
