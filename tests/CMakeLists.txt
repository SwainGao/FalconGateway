find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(falcon_tests
    test_main.cpp
    ring_buffer_test.cpp
    flow_controller_test.cpp
    order_manager_test.cpp
    position_manager_test.cpp
    config_loader_test.cpp
)
target_link_libraries(falcon_tests PRIVATE falcon_gateway GTest::gtest)
target_compile_options(falcon_tests PRIVATE ${FALCON_WARNINGS})

include(GoogleTest)
gtest_discover_tests(falcon_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
