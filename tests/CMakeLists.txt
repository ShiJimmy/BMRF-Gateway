add_executable(bmrf_tests
    test_main.cpp
    test_homebrew.cpp
    test_call_manager.cpp
    test_audio.cpp
    test_rf.cpp
    test_uplink.cpp
    test_client_uplink.cpp
    test_phy.cpp
    test_capture.cpp
    test_lc_encode.cpp
)

target_link_libraries(bmrf_tests PRIVATE bmrf_core)

if(MSVC)
    target_compile_options(bmrf_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(bmrf_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME bmrf_tests COMMAND bmrf_tests)
