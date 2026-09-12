# demo —— 最简命令行示例

一个纯命令行程序(**没有 Android app 壳**),演示怎么用 `libtrace.so`:把自己的一个 C 函数
放进 VCPU 里执行,并逐指令落 trace 日志。

- `main.cpp` —— 一个目标函数 `demo_target()` + `main()`。核心就三步:`trace(函数, 输出目录)`
  拿到同签名指针 → 调用它(进 VCPU 执行、出 trace)→ `freeTrace()`。
  > 用 `.cpp` 是因为 `trace.h` 是 C++ 接口(`trace` 有重载、符号 C++ mangled);代码本身只用 `stdio`,当纯 C 看即可。
- `build.sh` —— 用 NDK 把 `main.cpp` 编成 arm64 可执行文件,链 `../libs/prebuilt/arm64-v8a/libtrace.so`。

## 编译

```bash
NDK=/path/to/android-ndk bash build.sh      # 产出 demo_trace
```

## 在设备上运行

`libtrace.so` 是 Android arm64 动态库,所以可执行文件也在设备上跑(adb shell),不是桌面 Linux:

```bash
adb push demo/demo_trace libs/prebuilt/arm64-v8a/libtrace.so /data/local/tmp/
adb shell 'cd /data/local/tmp && chmod +x demo_trace && \
           LD_LIBRARY_PATH=. ./demo_trace /data/local/tmp/prybar_trace'
```

预期输出:

```
native = ...
vcpu   = ...
两者应相等: OK
trace 已写到目录: /data/local/tmp/prybar_trace/  ...
```

## 取回并还原 trace

trace 是 LZ4 压缩的 `.lz4`,用仓库自带脚本还原成文本:

```bash
adb pull /data/local/tmp/prybar_trace .
python ../tools/trace_receiver.py decode prybar_trace/*.lz4
```

想改成自己的目标函数、或换用底层 VCPU API(`vc_make_handle` + 各类 hook),照着 `main.cpp` 改即可;
接口都在 `../include/trace.h` 和 `../include/vcpu.h`。
