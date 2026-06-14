# Quill — bundled as git submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/external/quill ${CMAKE_BINARY_DIR}/quill)

file(GLOB LOGGER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(logger SHARED ${LOGGER_SOURCE})

target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(logger PUBLIC quill::quill)
