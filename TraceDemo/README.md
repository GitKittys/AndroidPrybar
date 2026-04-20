# AndroidPrybar

用于模拟执行 Android 中任意指令片段，监控函数运行时的行为。

本工程已包含预编译的 `libtrace.so`、头文件、JNI 示例和调用样例。克隆后直接编译运行即可查看结果。
先放出去让大家用用，看看大家的反馈如何，后续会考虑开源

## 这份工程主要演示什么

本工程围绕 `trace` 最常用的几类能力展开：

- 将普通 ARM64 函数包装为 VM 托管版本
- 一键开启目标函数的内置 trace
- 在回调中接收执行事件，并按自定义策略处理
- 读写寄存器，观察或修改运行时状态
- 在 Android / JNI 场景中验证 Native 函数的执行与跟踪

## 最常用的几组 API

### 1. `vc_make_handle`

这是最核心的入口函数。

它不仅用于“做 trace”，更将原始函数包装为一个新的可调用句柄。新句柄的调用方式与原函数基本一致，但实际执行时会进入 VM，而非直接运行于 CPU。

```cpp
vm_context* ctx = nullptr;
auto fn = reinterpret_cast<int(*)(int, int)>(
    vc_make_handle(reinterpret_cast<void*>(target_func), &ctx)); // 创建与原函数功能等价的函数指针

int result = fn(1, 2);
vc_free(ctx);
```

适合场景：

- 验证某个目标函数能否在 VM 中稳定运行
- 对比 host 与 VM 的执行结果
- 获取 `vm_context`，为后续回调、寄存器读写、事件控制做准备

一句话理解：

`vc_make_handle` 将“一个普通函数”变成“一个由 VM 托管、但依然可以像普通函数一样调用的包装函数”。

### 2. 自定义回调 `vc_cpu_event_callback`

如果说 `vc_make_handle` 解决的是“怎么把函数放进 VM 里跑”，那么回调解决的就是“跑的过程中我怎么参与决策”。

```cpp
static void my_callback(vm_context* ctx, vc_cpu_event_info* event, void* user_data) {
    if (event->type != VC_CPU_EVENT_EXTERNAL_JUMP) {
        return;
    }

    if (event->symbol_name != nullptr && strcmp(event->symbol_name, "strlen") == 0) {
        uint64_t fake_ret = 6;
        vc_reg_write(ctx, VC_REG_X0, &fake_ret);
        event->action = VC_ACTION_SKIP;
    }
}
```

这类回调通常与 `vc_make_handle(..., callback, user_data)` 搭配使用。

适合场景：

- 观察逐条指令、基本块、外部跳转、SVC、中断、内存访问
- 拦截某个外部函数并直接跳过
- 伪造某次外部调用的返回值
- 统计执行过程中的关键事件

一句话理解：

自定义回调不仅能监视每条指令的执行，还能让你在执行过程中真正获得控制权，例如修改 PC。

### 3. `trace`

如果你暂时不想自己接回调，只想快速拿到一份日志，最适合从 `trace` 开始。内置了 JNI trace（所有 JNI 信息）和每条汇编指令的执行信息。

```cpp
auto traced = reinterpret_cast<int(*)(int)>(
    trace(reinterpret_cast<void*>(target_func), log_path));

int result = traced(123);
freeTrace(reinterpret_cast<uint64_t>(traced));
```

适合场景：

- 第一次接入时验证 `trace` 是否能跑通
- 先看日志，再决定是否要做更精细的控制
- 快速观察 JNI 调用链或函数执行路径

一句话理解：

`trace` 是“一键开启内置跟踪”的最短路径。

### 4. `vc_reg_read` / `vc_reg_write`

这组接口的作用很直接：在 VM 中读写寄存器。

```cpp
uint64_t pc = 0;
vc_reg_read(ctx, VC_REG_PC, &pc);

uint64_t fake_ret = 0;
vc_reg_write(ctx, VC_REG_X0, &fake_ret);
```

适合场景：

- 获取当前 `PC`、`X0`、`SP` 等寄存器值
- 在回调中伪造返回值
- 直接修改执行状态

一句话理解：

一旦进入回调，这组接口就是最直接的“运行时干预工具”。

### 5. `vc_set_event_mask`

并非所有事件都需要一直开启。`vc_set_event_mask` 的作用是控制哪些事件进入回调，哪些保持关闭。

```cpp
vc_set_event_mask(ctx, VC_EVENT_DEFAULT_MASK | VC_EVENT_ENABLE_CODE);
```

适合场景：

- 开启逐条指令事件做精细观察
- 减少不必要的噪音和性能开销
- 将排查范围收窄到某一类事件上

一句话理解：

这组接口是在“细节”与“性能”之间做取舍的开关。

### 6. `vc_lookup_symbol`

很多时候你拿到的是地址，而非符号名。此时可以用 `vc_lookup_symbol` 将地址尽量解析为可读的函数名，尤其适合与外部跳转事件配合使用。

```cpp
const char* symbol = vc_lookup_symbol(ctx, event->address);
```

适合场景：

- 让日志更易读
- 了解某次外部跳转具体打到了哪个函数
- 将地址信息转换为更易理解的名字

### 7. `freeTrace` / `vc_free`

这两个接口并不花哨，但在实际使用中非常重要。

- `freeTrace` 用于释放 `trace` 返回的包装函数
- `vc_free` 用于释放 `vc_make_handle` 返回上下文后关联的 VM 资源

一句话理解：

前者对应 `trace`，后者对应 `vc_make_handle`，用完务必及时释放。

## 这份 Demo 里准备了哪些实际场景

点击界面上的 `Run Native Demo` 后，会按顺序运行以下几组示例：

- 自定义回调示例
- JNI trace 示例
- ARM64 原子指令 / `std::atomic` 示例
- 普通 trace 包装示例

这些示例并非随意拼凑，而是分别对应不同的接入场景。

### 自定义回调示例

这组示例重点演示：

- 如何声明自己的 `vc_cpu_event_callback`
- 如何通过 `user_data` 传递状态
- 如何在 `EXTERNAL_JUMP` 中伪造返回值并跳过真实调用

更适合：

- 按策略处理外部函数
- 自行掌控执行过程
- 进行行为观测而不只是看日志

### JNI trace 示例

这组示例重点演示：

- 如何在 Android 环境中直接调用 `trace`
- 如何跟踪 JNI 相关调用
- 如何将日志写入应用自己的文件目录

更适合：

- 验证 JNI-heavy 函数在 `trace` 下是否正常
- 演示这套 API 在 Android 项目中的接入方式
- 快速获取 JNI 调用链日志

### 原子测试示例

这组示例重点演示：

- VM 对常见 ARM64 原子指令的执行结果是否正确
- `std::atomic` 相关逻辑在 VM 下是否正常

更适合：

- 进行兼容性验证
- 做回归测试
- 确认底层指令行为在 VM 中是否稳定

### 普通 trace 示例

这组示例重点演示：

- 如何以最小成本将函数接入 `trace`
- 如何快速拿到日志并及时释放包装句柄

更适合：

- 第一次接入
- 先跑通再细化
- 快速确认 `trace` 能力是否正常工作

## 运行后会生成什么

运行成功后，会在应用自身的 `filesDir` 下生成以下日志文件：

- `prybar-jni-trace.log`
- `prybar-atomic-trace.log`
- `prybar-plain-trace.log`

界面输出中也会直接显示完整路径，方便你第一时间查看结果。

## 对外头文件

目前对外统一使用一个头文件：

`app/src/main/cpp/include/ARM64Emulator.h`

`trace`、`freeTrace` 以及 VM 相关的公开接口均已并入这个头文件中，使用者无需再区分第二份公开头文件。

## 项目结构

```text
TraceDemo/
|-- app/
|   |-- src/main/cpp/
|   |   |-- include/
|   |   |   `-- ARM64Emulator.h
|   |   |-- CMakeLists.txt
|   |   `-- native-lib.cpp
|   |-- src/main/jniLibs/arm64-v8a/libtrace.so
|   `-- src/main/java/com/prybar/tracedemo/MainActivity.java
|-- API_EXAMPLES.md
`-- README.md
```

## 建议怎么阅读这份工程

如果你是第一次接触这套接口，比较稳妥的顺序通常是：

1. 先看普通 `trace` 示例
2. 再看 `vc_make_handle`
3. 再看自定义回调
4. 再看寄存器读写和事件控制
5. 最后看 JNI trace 和原子测试

这样更容易先建立整体理解，再进入更细粒度的运行时控制。

## 个人的碎碎念念

这个工具我断断续续写了两年。当时几乎没有好用且公开的 trace 工具，也无人分享开发此类工具的方法。我一点点摸索、测试、推倒重来，仅为了编写一个支持自动传参的 JIT，就思考测试了许久。

同时，我知道在 AI 时代，即使不开放源码，通过 IDA 反汇编也可以很快复刻出工具并学习其中的思路。但这并不重要。

不求署名，不求分润，不求你在任何地方提及我的名字。我只求你在按下“复制”或“生成”之前，在心里，对这段熬了两年的代码，说一声谢谢。不需要我这份刚燃起开源的心冷却下去，不需要写在任何文档里。只要你自己知道，这东西不是凭空从石头里蹦出来的，它是某个人在无数个深夜里，一行一行从混沌里捞出来的，这就够了。
