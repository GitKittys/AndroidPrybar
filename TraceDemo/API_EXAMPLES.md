# API_EXAMPLES

这份文档对应 `TraceDemo/app/src/main/cpp/native-lib.cpp` 里的实际示例。

它不是单纯把接口罗列一遍，而是把最常见的几种使用方式拆开讲清楚：  
这个 API 怎么用、适合什么场景、看示例时应该重点注意什么。

## 1. `vc_make_handle`：创建 VM 托管函数包装

```cpp
vm_context* ctx = nullptr;
auto fn = reinterpret_cast<int(*)(int, int)>(
    vc_make_handle(reinterpret_cast<void*>(target_func), &ctx));

int result = fn(1, 2);
vc_free(ctx);
```

这段代码想说明的是：

- 原始函数可以被包装成一个新的可调用句柄
- 调用方式仍然像普通函数，但实际执行已经进入 VM
- 后续很多控制能力都要依赖这里返回的 `ctx`

适合场景：

- 想先验证某个函数能不能在 VM 中正常执行
- 想对比 host 和 VM 结果是否一致
- 想为后面的回调、寄存器读写准备上下文

## 2. 自定义回调：在执行过程中参与决策

```cpp
struct CustomCallbackState {
    const char* symbolToSkip = "strlen";
    uint64_t forcedReturnX0 = 6;
};

void custom_event_callback(vm_context* ctx, vc_cpu_event_info* event, void* user_data) {
    auto* state = reinterpret_cast<CustomCallbackState*>(user_data);
    if (event->type == VC_CPU_EVENT_EXTERNAL_JUMP &&
        event->symbol_name != nullptr &&
        std::strcmp(event->symbol_name, state->symbolToSkip) == 0) {
        vc_reg_write(ctx, VC_REG_X0, &state->forcedReturnX0);
        event->action = VC_ACTION_SKIP;
    }
}
```

这段代码想说明的是：

- 如何定义自己的 `vc_cpu_event_callback`
- 如何用 `user_data` 给回调传入配置和状态
- 如何在外部跳转事件里伪造返回值并跳过真实调用

适合场景：

- 想拦截某个外部函数
- 想统计执行过程中的关键事件
- 想让同一个回调服务多个目标函数，但各自有不同策略

## 3. `vc_reg_read` / `vc_reg_write`：读写寄存器

```cpp
vc_reg_read(ctx, VC_REG_PC, &state->lastPc);
vc_reg_read(ctx, VC_REG_X0, &state->lastX0);
vc_reg_write(ctx, VC_REG_X0, &state->forcedReturnX0);
```

这段代码想说明的是：

- 可以在回调里直接读当前寄存器状态
- 可以把返回值、参数寄存器改成你想要的内容
- 这组接口是做运行时干预时最直接的一把刀

适合场景：

- 想知道当前执行到了哪里
- 想伪造某次外部调用的返回值
- 想在关键节点修正寄存器状态

## 4. `vc_lookup_symbol`：把地址尽量变成可读名字

```cpp
const char* resolved = vc_lookup_symbol(ctx, event->address);
```

这段代码想说明的是：

- 在回调里拿到地址后，不一定只能看裸地址
- 可以尽量把它解析成符号名，让日志更可读

适合场景：

- 想分析外部跳转具体去了哪里
- 想让日志更容易读
- 想把事件信息做成人能直接理解的输出

## 5. `vc_set_event_mask`：控制哪些事件会进入回调

```cpp
vc_set_event_mask(ctx, VC_EVENT_DEFAULT_MASK | VC_EVENT_ENABLE_CODE);
```

这段代码想说明的是：

- 不是所有事件都要一直开着
- `CODE` 这类事件很细，但也更重
- 你可以按排查需要动态调节事件粒度

适合场景：

- 想临时打开逐条指令观察
- 想减少性能开销和日志噪音
- 想把回调聚焦在某一类问题上

## 6. `trace`：一键创建内置 trace 包装

```cpp
auto traced = reinterpret_cast<uint64_t(*)(uint64_t)>(
    trace(reinterpret_cast<void*>(plain_trace_target), writablePath.data()));

uint64_t result = traced(21);
freeTrace(reinterpret_cast<uint64_t>(traced));
```

这段代码想说明的是：

- 如果只是想先拿一份日志，`trace` 是最快的入口
- 它会返回一个“已经带 trace”的包装函数
- 跑完后要配对调用 `freeTrace`

适合场景：

- 第一次接入时先跑通最短链路
- 想先观察执行路径
- 暂时不需要自己接回调

## 7. JNI trace 示例：验证 Android 场景的实际接法

```cpp
auto traced = reinterpret_cast<jstring(*)(JavaVM*)>(
    trace(reinterpret_cast<void*>(jni_demo_target), writablePath.data()));

jstring result = traced(javaVm);
freeTrace(reinterpret_cast<uint64_t>(traced));
```

这段代码想说明的是：

- `trace` 可以直接用于 JNI-heavy 的目标函数
- 日志可以直接写到应用自己的文件目录
- 在 Android 项目里，这种接法很适合做第一轮验证

示例里包含的 JNI 行为包括：

- `FindClass`
- `GetMethodID`
- `GetStaticMethodID`
- `CallIntMethod`
- `CallStaticObjectMethod`
- `NewIntArray`
- `GetIntArrayElements`
- `NewObject`
- `NewGlobalRef`
- `MonitorEnter` / `MonitorExit`

日志文件：

- `filesDir/prybar-jni-trace.log`

## 8. 原子测试示例：验证低层行为是否稳定

示例里覆盖了这些常见原子场景：

- `LDXR / STXR`
- `LDAXR / STLXR`
- `CAS`
- `SWP`
- `LDADD`
- `LDARH / STLRH`
- `LDADDB`
- `std::atomic::fetch_add`
- `std::atomic::compare_exchange_strong`
- `std::atomic` 位运算

这组示例想说明的是：

- VM 对一批 ARM64 原子指令的处理是否正确
- `std::atomic` 相关逻辑在 VM 下是否稳定
- 做兼容性验证时，应该先拿这种小而硬的函数做自测

日志文件：

- `filesDir/prybar-atomic-trace.log`

## 9. 普通 trace 示例：最小接入路径

这组示例是刻意保留的最小路径。

它想讲的不是复杂能力，而是最实用的一件事：  
如果你手上已经有一个 Native 函数，想先确认这套库能不能接进去、能不能出日志，那么先看它就够了。

日志文件：

- `filesDir/prybar-plain-trace.log`

## 10. 对外头文件和预编译 so

当前导出项目对外统一使用：

- `app/src/main/cpp/include/ARM64Emulator.h`

预编译库位置：

- `app/src/main/jniLibs/arm64-v8a/libtrace.so`
