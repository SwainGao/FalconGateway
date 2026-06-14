cmake_minimum_required(VERSION 3.24)
project(FalconGateway VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release with debug info
option(FALCON_MARCH_NATIVE "Use -march=native for host CPU optimization" ON)
if(FALCON_MARCH_NATIVE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -march=native -flto")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -flto")
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -Wall -Wextra")

# Include paths
set(CTP_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/ctp)
set(CTP_LIB_DIR ${CMAKE_SOURCE_DIR}/lib)

# Import CTP shared libraries
add_library(ctp_trader SHARED IMPORTED)
set_target_properties(ctp_trader PROPERTIES
    IMPORTED_LOCATION ${CTP_LIB_DIR}/thosttraderapi_se.so
    INTERFACE_INCLUDE_DIRECTORIES ${CTP_INCLUDE_DIR}
)

add_library(ctp_md SHARED IMPORTED)
set_target_properties(ctp_md PROPERTIES
    IMPORTED_LOCATION ${CTP_LIB_DIR}/thostmduserapi_se.so
    INTERFACE_INCLUDE_DIRECTORIES ${CTP_INCLUDE_DIR}
)

# Logger (Quill-based async logger, quill provided as git submodule)
add_subdirectory(logger)

# Gateway static library
add_library(falcon_gateway STATIC
    src/order_manager.cpp
    src/position_manager.cpp
    src/trader_handler.cpp
    src/md_handler.cpp
    src/gateway.cpp
    src/config_loader.cpp
)

target_include_directories(falcon_gateway PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CTP_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/logger/include
)

target_link_libraries(falcon_gateway PUBLIC
    ctp_trader ctp_md pthread logger
)

# Example executable
add_executable(falcon_main main.cpp)
target_link_libraries(falcon_main falcon_gateway)

# Copy .so files to build directory for runtime
file(COPY ${CTP_LIB_DIR}/thosttraderapi_se.so DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CTP_LIB_DIR}/thostmduserapi_se.so DESTINATION ${CMAKE_BINARY_DIR})

# Performance flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(falcon_gateway PRIVATE -funroll-loops -fomit-frame-pointer)
endif()

# ================================================================
# Install targets — for packaging / distribution
# ================================================================
include(GNUInstallDirs)

# FalconGateway static library
install(TARGETS falcon_gateway logger
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
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
