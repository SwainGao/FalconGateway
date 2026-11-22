cmake_minimum_required(VERSION 3.24)
project(FalconGateway VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FALCON_MARCH_NATIVE "Use -march=native (binaries may not run on other CPUs)" OFF)
option(FALCON_ENABLE_LTO "Enable link-time optimization in Release builds" ON)
option(FALCON_WERROR "Treat warnings in FalconGateway sources as errors" OFF)
option(FALCON_BUILD_TESTS "Build unit tests (GoogleTest)" OFF)
set(FALCON_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or address,undefined")

# Keep CMake's default Release flags (-O3 -DNDEBUG); only append to them
add_compile_options($<$<CONFIG:Release>:-g>)
if(FALCON_MARCH_NATIVE)
    add_compile_options(-march=native)
endif()

if(FALCON_SANITIZER)
    add_compile_options(-fsanitize=${FALCON_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${FALCON_SANITIZER})
elseif(FALCON_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT falcon_ipo_ok OUTPUT falcon_ipo_msg)
    if(falcon_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

set(FALCON_WARNINGS -Wall -Wextra $<$<BOOL:${FALCON_WERROR}>:-Werror>)

# Include paths
set(CTP_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/ctp)
set(CTP_LIB_DIR ${CMAKE_SOURCE_DIR}/lib)

# CTP shared libraries. The SDK .so files have no SONAME and no "lib" prefix:
# link them by file name (-l:) so binaries record "thosttraderapi_se.so" as
# NEEDED instead of the absolute build path.
add_library(ctp_trader INTERFACE)
target_include_directories(ctp_trader SYSTEM INTERFACE ${CTP_INCLUDE_DIR})
target_link_directories(ctp_trader INTERFACE ${CTP_LIB_DIR})
target_link_libraries(ctp_trader INTERFACE -l:thosttraderapi_se.so)

add_library(ctp_md INTERFACE)
target_include_directories(ctp_md SYSTEM INTERFACE ${CTP_INCLUDE_DIR})
target_link_directories(ctp_md INTERFACE ${CTP_LIB_DIR})
target_link_libraries(ctp_md INTERFACE -l:thostmduserapi_se.so)
list(APPEND CMAKE_BUILD_RPATH ${CTP_LIB_DIR})

# Logger (Quill-based async logger, quill provided as git submodule)
add_subdirectory(logger)

# Gateway static library
add_library(falcon_gateway STATIC
    src/order_manager.cpp
    src/position_manager.cpp
    src/flow_controller.cpp
    src/trader_handler.cpp
    src/md_handler.cpp
    src/gateway.cpp
    src/config_loader.cpp
)

target_include_directories(falcon_gateway PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/logger/include
)
target_include_directories(falcon_gateway SYSTEM PUBLIC ${CTP_INCLUDE_DIR})
# nlohmann/json (header-only, git submodule) — used only by config_loader.cpp
target_include_directories(falcon_gateway SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/external/json/single_include)

target_link_libraries(falcon_gateway PUBLIC
    ctp_trader ctp_md pthread logger
)
target_compile_options(falcon_gateway PRIVATE ${FALCON_WARNINGS})
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT FALCON_SANITIZER)
    target_compile_options(falcon_gateway PRIVATE -funroll-loops -fomit-frame-pointer)
endif()

# Example executable (monitor mode by default; --run-order-test places orders)
add_executable(falcon_main main.cpp)
target_link_libraries(falcon_main falcon_gateway)
target_compile_options(falcon_main PRIVATE ${FALCON_WARNINGS})

# Copy .so files to build directory for runtime
file(COPY ${CTP_LIB_DIR}/thosttraderapi_se.so DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CTP_LIB_DIR}/thostmduserapi_se.so DESTINATION ${CMAKE_BINARY_DIR})

if(FALCON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ================================================================
# Install targets — for packaging / distribution
# ================================================================
include(GNUInstallDirs)
set_target_properties(falcon_main PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

install(TARGETS falcon_gateway logger falcon_main
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Public headers
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/falcon
    FILES_MATCHING PATTERN "*.h"
)

# Logger headers
install(DIRECTORY logger/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/falcon
    FILES_MATCHING PATTERN "*.h"
)

# CTP shared libraries (runtime)
install(FILES
    ${CTP_LIB_DIR}/thosttraderapi_se.so
    ${CTP_LIB_DIR}/thostmduserapi_se.so
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Config template
install(FILES config.json.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/falcon
    RENAME config.json.example
)
