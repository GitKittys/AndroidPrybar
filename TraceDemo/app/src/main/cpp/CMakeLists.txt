cmake_minimum_required(VERSION 3.22.1)
project(tracedemo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(trace SHARED IMPORTED)
set_target_properties(trace PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtrace.so"
)

add_library(${CMAKE_PROJECT_NAME} SHARED native-lib.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -march=armv8.3-a+lse
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    trace
    android
    atomic
    log
)
