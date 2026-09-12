#!/bin/bash
# ============================================================================
#  build.sh —— 把 main.cpp 编成 arm64 命令行可执行文件,链接 libtrace.so。
#  产物: demo_trace(在本目录)。
#
#  用法:  NDK=/path/to/android-ndk bash build.sh
#         (未设 NDK 时尝试 ANDROID_NDK_HOME)
# ============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"        # .../demo
ROOT="$(cd "$HERE/.." && pwd)"               # AndroidPrybar 根
API=${API:-24}

[ -z "$NDK" ] && NDK="$ANDROID_NDK_HOME"
[ -z "$NDK" ] && { echo "请设 NDK 或 ANDROID_NDK_HOME 指向 Android NDK(r25 系)"; exit 1; }
case "$(uname -s)" in Linux) HOST=linux-x86_64;; Darwin) HOST=darwin-x86_64;; *) HOST=windows-x86_64;; esac
CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android$API-clang++"
[ -x "$CXX" ] || CXX="$CXX.cmd"

LIBS="$ROOT/libs/prebuilt/arm64-v8a"
OUT="$HERE/demo_trace"

"$CXX" "$HERE/main.cpp" -o "$OUT" \
  -I"$ROOT/include" \
  -L"$LIBS" -ltrace \
  -llog

echo "==> $OUT"
echo "运行:"
echo "  adb push $OUT $LIBS/libtrace.so /data/local/tmp/"
echo "  adb shell 'cd /data/local/tmp && chmod +x demo_trace && LD_LIBRARY_PATH=. ./demo_trace /data/local/tmp/prybar_trace'"
echo "  adb pull /data/local/tmp/prybar_trace . && python ../tools/trace_receiver.py decode prybar_trace/*.lz4"
