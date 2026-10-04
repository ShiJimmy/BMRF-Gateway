add_executable(bmrf_frame_dump frame_dump.cpp)
target_link_libraries(bmrf_frame_dump PRIVATE bmrf_core)

add_executable(bmrf_rf_analyze rf_analyze.cpp)
target_link_libraries(bmrf_rf_analyze PRIVATE bmrf_core)

add_executable(bmrf_rf_voice_dump rf_voice_dump.cpp)
target_link_libraries(bmrf_rf_voice_dump PRIVATE bmrf_core)

add_executable(bmrf_rf_disc_dump rf_disc_dump.cpp)
target_link_libraries(bmrf_rf_disc_dump PRIVATE bmrf_core)

add_executable(bmrf_rf_lc_dump rf_lc_dump.cpp)
target_link_libraries(bmrf_rf_lc_dump PRIVATE bmrf_core)

add_executable(bmrf_capture_replay capture_replay.cpp)
target_link_libraries(bmrf_capture_replay PRIVATE bmrf_core)

if(BMRF_USE_DSDCC)
    add_executable(bmrf_rf_dsd_dump rf_dsd_dump.cpp)
    target_link_libraries(bmrf_rf_dsd_dump PRIVATE bmrf_core)
endif()
