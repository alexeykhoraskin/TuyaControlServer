cmake_minimum_required(VERSION 3.14)
project(tuya_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(tuya_cli
    src/main.cpp
    src/server_api.cpp
)
target_include_directories(tuya_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(tuya_cli PRIVATE ${CURL_LIBRARIES})
