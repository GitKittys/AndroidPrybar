// ============================================================================
//  最简示例:一个命令行程序(无 Android app 壳),用 libtrace.so 的 trace()
//  把自己的一个 C 函数放进 VCPU 里执行,并逐指令落 trace 日志。
//
//  说明:trace.h 是 C++ 接口(trace 有重载、符号 C++ mangled),所以本文件用 .cpp;
//        但写法保持极简,只用到 stdio —— 你把它当纯 C 看即可。
//
//  编译:  bash build.sh          (见 build.sh,用 NDK 编成 arm64 可执行文件)
//  运行:  adb push demo_trace ../libs/prebuilt/arm64-v8a/libtrace.so /data/local/tmp/
//         adb shell "cd /data/local/tmp && chmod +x demo_trace && \
//                    LD_LIBRARY_PATH=. ./demo_trace /data/local/tmp/prybar_trace"
//         adb pull /data/local/tmp/prybar_trace .        # 取回 .lz4
//         python ../tools/trace_receiver.py decode prybar_trace/*.lz4   # 还原成文本
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include "trace.h"        // 只需 trace.h(顶部已 #include "vcpu.h")

// ---- 我们要被 trace 的目标 C 函数:一段带分支和循环的纯计算 ----
//  noinline:别让编译器把它内联没了,trace 才有东西可看。
__attribute__((noinline))
static int demo_target(int a, int b) {
    int acc = 0;
    for (int i = 0; i < b; i++) {
        acc += a * i;
        if (acc > 1000) acc -= 500;   // 制造点分支
    }
    return acc;
}

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "/data/local/tmp/prybar_trace";

    // trace(目标函数, 输出目录) 返回一个"同签名"的函数指针:
    //   调它 == 让目标函数在 VCPU 里跑一遍,同时把每条指令写进 out 目录(LZ4)。
    int (*traced)(int, int) = (int (*)(int, int)) trace((void*) demo_target, (char*) out);
    if (!traced) {
        printf("trace() 失败\n");
        return 1;
    }

    int host = demo_target(21, 10);   // 原生直接调,作对照
    int vm   = traced(21, 10);        // 进 VCPU 执行,产出 trace

    printf("native = %d\n", host);
    printf("vcpu   = %d\n", vm);
    printf("两者应相等: %s\n", host == vm ? "OK" : "不一致!");
    printf("trace 已写到目录: %s/  (用 trace_receiver.py decode *.lz4 还原为文本)\n", out);

    freeTrace((uint64_t) traced);     // 释放句柄(不调也行,只是回收资源)
    return 0;
}
