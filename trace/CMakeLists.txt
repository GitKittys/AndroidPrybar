# ============================================================================
#  trace 层(开源)——链接预编译的 libvcpu.a(闭源资产:VCPU + 引擎合并、符号隐藏)
#  编出 libtrace.so。也可直接跑 build_trace.sh(纯 NDK,无需 CMake)。
#
#  用法(独立构建):
#    cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -GNinja -S trace -B build
#    cmake --build build
#  或被上层工程 add_subdirectory(trace)。
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(trace)

get_filename_component(PRYBAR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(LIBDIR "${PRYBAR_ROOT}/libs/${ANDROID_ABI}")

# 只编 trace 自身源(src/);Utils 的符号已在 libvcpu.a 里,这里仅用其头文件。
file(GLOB_RECURSE TRACE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(trace SHARED ${TRACE_SRC})

target_include_directories(trace PRIVATE
    ${PRYBAR_ROOT}/include                        # 公开头:vcpu.h / trace.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include            # trace 自己的头(含 ARM64Emulator.h 垫片)
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils/dlfc
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/include/unicorn
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/include/capstone
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/include/dobby
)

target_compile_options(trace PRIVATE
    -O2 -march=armv8.1-a+lse
    -ffunction-sections -fdata-sections -fno-rtti -fomit-frame-pointer -s
)

# 闭源资产 + trace 依赖(capstone/dobby)只链进 libtrace.so 自身(避免上层重复静态链)。
# libvcpu.a 已含引擎并与 capstone 互相引用,用 --start-group 成组解析。
target_link_libraries(trace PRIVATE
    -Wl,--start-group
    ${LIBDIR}/libvcpu.a
    ${LIBDIR}/libcapstone.a
    ${LIBDIR}/libdobby.a
    -Wl,--end-group
    android log atomic m dl
)

target_link_options(trace PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/trace.exports
)
