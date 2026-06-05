cmake_minimum_required(VERSION 3.14)
project(TuyaControlServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──
find_package(OpenSSL REQUIRED)

find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(STATUS "CURL not found — cloud API will be stubbed")
    add_compile_definitions(NO_CURL)
endif()

# ── Sources ──
file(GLOB_RECURSE SOURCES src/*.cpp)

# ── Main target ──
add_executable(tuya_control_server ${SOURCES})

# ── Fetch tool ──
add_executable(fetch_devices tools/fetch_devices.cpp src/cloud_api.cpp src/config.cpp src/crypto.cpp)
target_include_directories(fetch_devices PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(fetch_devices PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
if(CURL_FOUND)
    target_link_libraries(fetch_devices PRIVATE ${CURL_LIBRARIES})
    target_include_directories(fetch_devices PRIVATE ${CURL_INCLUDE_DIRS})
endif()

target_include_directories(tuya_control_server PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(tuya_control_server PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
if(CURL_FOUND)
    target_link_libraries(tuya_control_server PRIVATE ${CURL_LIBRARIES})
    target_include_directories(tuya_control_server PRIVATE ${CURL_INCLUDE_DIRS})
endif()
