#!/bin/bash
# ============================================================================
#  build_trace.sh —— 用开源 trace 源码 + 预编译 libvcpu.a 编出 libtrace.so。
#
#  分层:libvcpu.a(闭源二进制资产:VCPU + 引擎合并、符号隐藏)在 ../libs 下;
#        trace 层开源,链 libvcpu.a + libcapstone.a + libdobby.a 成 libtrace.so。
#  产物: ../libs/prebuilt/arm64-v8a/libtrace.so
#
#  用法:  NDK=/path/to/ndk bash trace/build_trace.sh
#         (未设 NDK 时尝试 ANDROID_NDK_HOME)
# ============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"       # .../trace
ROOT="$(cd "$HERE/.." && pwd)"              # AndroidPrybar 根
API=${API:-24}

[ -z "$NDK" ] && NDK="$ANDROID_NDK_HOME"
[ -z "$NDK" ] && { echo "请设 NDK 或 ANDROID_NDK_HOME 指向 Android NDK(r25 系)"; exit 1; }
case "$(uname -s)" in Linux) HOST=linux-x86_64;; Darwin) HOST=darwin-x86_64;; *) HOST=windows-x86_64;; esac
CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android$API-clang++"
[ -x "$CXX" ] || CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android$API-clang++.cmd"

LIBS="$ROOT/libs/arm64-v8a"
OUTDIR="$ROOT/libs/prebuilt/arm64-v8a"; mkdir -p "$OUTDIR"
OUT="$OUTDIR/libtrace.so"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

INCS="-I$ROOT/include \
  -I$HERE/compat \
  -I$HERE/include \
  -I$HERE/api/trace -I$HERE/api/trace_inner_include \
  -I$HERE/Utils -I$HERE/Utils/dlfc \
  -I$HERE/thirdparty/include/unicorn \
  -I$HERE/thirdparty/include/capstone \
  -I$HERE/thirdparty/include/dobby"

CXXFLAGS="-O2 -fPIC -std=c++17 -march=armv8.1-a+lse -DANDROID \
  -ffunction-sections -fdata-sections -fno-rtti -fomit-frame-pointer"

CCBASE="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android$API-clang"
CC="$CCBASE"; [ -x "$CC" ] || CC="$CCBASE.cmd"
echo "编译 trace 源(NDK=$NDK)..."
i=0; OBJS=""
# 只编 trace 自身源(api/);Utils 的符号已在 libvcpu.a 里,这里仅用其头文件,不重复编译
for s in $(find "$HERE/api" \( -name '*.cpp' -o -name '*.c' \)); do
  o="$W/$i.o"; i=$((i+1))
  case "$s" in
    *.c)  "$CC"  -O2 -fPIC -std=c11 -march=armv8.1-a+lse -DANDROID -ffunction-sections -fdata-sections $INCS -c "$s" -o "$o" ;;
    *)    "$CXX" $CXXFLAGS $INCS -c "$s" -o "$o" ;;
  esac
  OBJS="$OBJS $o"
done

echo "链接 libtrace.so(链 libvcpu.a + capstone + dobby)..."
"$CXX" -shared -o "$OUT" $OBJS \
  -Wl,--gc-sections -Wl,--strip-all \
  -Wl,--version-script="$HERE/trace.exports" \
  -Wl,--start-group "$LIBS/libvcpu.a" "$LIBS/libcapstone.a" "$LIBS/libdobby.a" -Wl,--end-group \
  -landroid -llog -latomic -lm -ldl

echo "==> $OUT ($(du -h "$OUT" | cut -f1))"
