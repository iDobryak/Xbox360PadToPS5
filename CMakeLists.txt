cmake_minimum_required(VERSION 3.16)
project(ps5_xb360_receiver VERSION 0.1.0 LANGUAGES C)

option(PS5_XB360_BUILD_TESTS "Build host-side protocol tests" ON)

add_library(ps5_xb360 STATIC
    src/x360_receiver.c
    src/dualsense_mapper.c
    src/receiver_usb.c
    src/bridge.c
)

target_include_directories(ps5_xb360 PUBLIC include)
target_compile_features(ps5_xb360 PUBLIC c_std_11)
target_compile_options(ps5_xb360 PRIVATE -Wall -Wextra -Wpedantic)

if(PS5_XB360_BUILD_TESTS)
    enable_testing()
    add_executable(ps5_xb360_tests tests/test_main.c)
    target_link_libraries(ps5_xb360_tests PRIVATE ps5_xb360)
    target_compile_options(ps5_xb360_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ps5_xb360_tests COMMAND ps5_xb360_tests)

    add_executable(ps5_xb360_web_tests
        tests/test_web.c
        src/ps5_web.c
        src/ps5_log.c
    )
    target_include_directories(ps5_xb360_web_tests PRIVATE include)
    target_compile_features(ps5_xb360_web_tests PRIVATE c_std_11)
    target_compile_definitions(ps5_xb360_web_tests PRIVATE
        _DEFAULT_SOURCE _DARWIN_C_SOURCE
        PS5_WEB_LOG_PATH="/tmp/ps5-xb360-web-test.log"
        PS5_WEB_TELEMETRY_PATH="/tmp/ps5-xb360-web-test.csv")
    target_compile_options(ps5_xb360_web_tests PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME ps5_xb360_web_tests COMMAND ps5_xb360_web_tests)

    add_executable(ps5_xb360_vpad_tests
        tests/test_vpad.c
        src/ps5_vpad.c
        src/ps5_log.c
    )
    target_include_directories(ps5_xb360_vpad_tests PRIVATE include)
    target_compile_features(ps5_xb360_vpad_tests PRIVATE c_std_11)
    target_compile_definitions(ps5_xb360_vpad_tests PRIVATE
        _DEFAULT_SOURCE _DARWIN_C_SOURCE PS5_VPAD_LOG_PORT=23232)
    target_compile_options(ps5_xb360_vpad_tests PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME ps5_xb360_vpad_tests COMMAND ps5_xb360_vpad_tests)

    add_executable(ps5_xb360_telemetry_tests
        tests/test_telemetry.c
        src/ps5_telemetry.c
        src/ps5_log.c
    )
    target_include_directories(ps5_xb360_telemetry_tests PRIVATE include)
    target_compile_features(ps5_xb360_telemetry_tests PRIVATE c_std_11)
    target_compile_definitions(ps5_xb360_telemetry_tests PRIVATE
        _DEFAULT_SOURCE _DARWIN_C_SOURCE
        PS5_TELEMETRY_CSV_PATH="/tmp/ps5-xb360-telemetry-test.csv")
    target_compile_options(ps5_xb360_telemetry_tests PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME ps5_xb360_telemetry_tests COMMAND ps5_xb360_telemetry_tests)
endif()
