cmake_minimum_required(VERSION 3.22.1)
project(tracedemo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# AndroidPrybar 根(本文件在 demo/app/src/main/cpp/,上溯 5 层)
get_filename_component(PRYBAR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# 预编译的 trace 动态库(开源 trace 源 + libvcpu.a 编出;运行期从 jniLibs 加载)
add_library(trace SHARED IMPORTED)
set_target_properties(trace PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtrace.so"
)

add_library(${CMAKE_PROJECT_NAME} SHARED native-lib.cpp)

# 分离后的公开头:vcpu.h(VCPU 核心) + trace.h(trace 工具,已 #include vcpu.h)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${PRYBAR_ROOT}/include"
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
