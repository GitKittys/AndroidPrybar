ARM64 **函数级 VCPU（可编程虚拟 CPU）** + 指令跟踪框架。把任意 native 函数放进 Unicorn 引擎里执行，你能像调试器一样完全掌控它：逐指令 / 基本块 / 内存 / SVC / 外部调用 hook、读写寄存器、单步、断点、CPU 快照、反汇编。

`trace()` **只是这套 libcVCPU 的封装。** 你可以直接调用直接 `trace()` 一键出日志;
如果你对trace日志格式不满意，可以自己修改，本质上都是对 VCPU（`vc_make_handle` + vcpu内部的hook回调接口的封装
自己驱动——`trace()` / `trace_unidbg_dump()` / `replace_trace()` 都是这层 VCPU 的封装。

VCPU 是闭源的引擎内核,`trace/` 则是它上面一层**开源**的示范应用(与 VCPU 的解耦由 AI 完成,只通过公开 `vc_`* 接口调用)。所以 trace 源码本身就是「这套 VCPU 怎么用」的最佳范例;想定制就拿它当模板改,`trace/build_trace.sh` 重编 `libtrace.so` 即可,全程不需要 VCPU 源码。

支持多进程、多线程、多并发,多个函数同时追踪;抖音、美团等大型 app 实测无崩溃。工程已含预编译 `libtrace.so`、分离好的头文件,以及一个最简命令行示例(`demo/`,纯 C 用法、无 app 壳)。

## 更新记录

- 新增 `vc_set_trace_crash_flush()`：app 崩溃/终止/exit 前强制把 trace 缓冲刷到磁盘，不丢崩溃点之前那段
- `trace_receiver.py` 解压改用 lz4 C 引擎（`pip install lz4`），decode 大幅加速；未安装则自动回退纯 Python
- 新增 `trace_unidbg_dump()`：一键导出 Unidbg「中段执行」dump 包（内存段/寄存器/参数/符号/JNI 表/maps/rootfs）
- README 增加 Frida 调用 `trace()` 的示例
- 修复了部分 bug

## `libtrace.so` 你拿来就用，如果你想修改trace日志格式，你可以自己改

预编译动态库：`libs/prebuilt/arm64-v8a/libtrace.so`（demo 运行时把它和可执行文件一起 `adb push` 到设备的 `/data/local/tmp/`）

对外头文件（**已分离**）：`include/vcpu.h`（VCPU 核心 API）+ `include/trace.h`（trace 工具,顶部已 `#include "vcpu.h"`）。用 trace 直接 `#include "trace.h"` 即可。

### 开源分层(重要)

- `trace/` —— 开源的 trace 层源码,链接闭源的 `libvcpu.a` 编出 `libtrace.so`。
- `libs/arm64-v8a/libvcpu.a` **/** `libvcpu.so` —— 闭源二进制资产:**VCPU + Unicorn 引擎合并、已去除全部内部符号,仅暴露** `vc_`* **等接口**。`.a` 供 trace 静态嵌入、`.so` 供直接用裸 VCPU API 者动态加载。
- **两种用法**:①克隆即用——直接用 `libs/prebuilt/arm64-v8a/libtrace.so` + `include/*.h`;②改 trace 源码后 `NDK=/path bash trace/build_trace.sh` 重编 `libtrace.so`(只需开源 trace 源 + 现成 `libvcpu.a`)。

---

## 快速上手

> **两层用法，按需选：**
>
> - **开箱即用的包装**（不写逻辑、一键出结果）：`trace()`、`trace_unidbg_dump()`、`replace_trace()`。
> - **底层 VCPU**（自己写回调、掌控执行）：`vc_make_handle()` + `vc_hook_add()` + 寄存器读写 + 单步 / 断点 / CPU 快照 / 反汇编。
>
> 上面的包装**全部基于**下面的 VCPU API 实现。所以你既能当"一键 trace 工具"用，也能当"可编程 ARM64 VCPU"用——同一套东西。


## 文档

详细用法拆成两份,按需看:

- **[docs/VCPU.md](docs/VCPU.md)** —— 底层可编程 VCPU:`vc_make_handle` 拿句柄、各类 Hook、寄存器读写、单步 / 断点、CPU 快照、反汇编、跳转控制、内存监控,以及「运行时改寄存器 / 返回值」实战。**想自己包 trace 也看这份。**
- **[docs/TRACE.md](docs/TRACE.md)** —— 开箱 trace 工具:`trace()`、Frida 调用、自动线程追踪、崩溃保盘、`replace_trace()`、`trace_unidbg_dump()`、trace 输出格式,以及配套解码 / 调用树工具。

## API 速查表


| API                                               | 用途                                                                       |
| ------------------------------------------------- | -------------------------------------------------------------------------- |
| `trace(func, path)`                               | 快速 trace（path 为目录 → 本地 .lz4，`"tcp:PORT"` → 远程）               |
| `trace(func, path, &ctx)`                         | 带 ctx 的 trace                                                            |
| `freeTrace(wrapper)`                              | 释放 trace 句柄                                                            |
| `replace_trace(func, path)`                       | 全局替换式 trace                                                           |
| `restore_function(func)`                          | 恢复被替换的函数                                                           |
| `trace_unidbg_dump(func, dumpDir)`                | 导出 Unidbg 中段执行 dump 包（跑完自动落盘，返回可调用指针）               |
| `trace_unidbg_dump_finish(wrapper)`               | 释放 dump 句柄（文件已自动落盘，仅回收资源）                               |
| `vc_make_handle(func, &ctx)`                      | 创建裸 VM 句柄                                                             |
| `vc_free(ctx)`                                    | 释放 VM 上下文                                                             |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | 注册 hook                                                                  |
| `vc_hook_del(ctx, hh)`                            | 删除 hook                                                                  |
| `vc_reg_read / vc_reg_write`                      | 寄存器读写                                                                 |
| `vc_reg_read_batch / vc_reg_write_batch`          | 批量读写                                                                   |
| `vc_emu_stop(ctx)`                                | 停止 VM                                                                    |
| `vc_single_step(ctx, count)`                      | 执行 N 条后暂停                                                            |
| `vc_set_until(ctx, addr)`                         | 设置临时断点                                                               |
| `vc_disasm(addr, count, out)`                     | 反汇编                                                                     |
| `vc_context_save / restore / free`                | CPU 快照                                                                   |
| `vc_lookup_symbol(ctx, addr)`                     | 地址查符号                                                                 |
| `vc_set_jump_blacklist(names, ranges, n)`         | 设置跳转黑名单                                                             |
| `vc_clear_jump_blacklist()`                       | 清除黑名单                                                                 |
| `vc_set_external_jump_enabled(enabled)`           | 全局跳转开关                                                               |
| `vc_set_auto_trace_threads(ctx, enable)`          | 自动追踪被 trace 函数内部创建的线程（**默认关**；3 参拿 ctx 传 true 开启） |
| `vc_set_trace_crash_flush(enable)`                | 崩溃/终止/exit 前自动把 trace 缓冲刷到盘（文件模式），不丢崩溃前那段       |
| `trace_read / trace_write`                        | 内存监控                                                                   |

## 性能参考


| 模式                   | 速度         | 适用场景               |
| ---------------------- | ------------ | ---------------------- |
| vc_make_handle（默认） | ~2-5x 慢     | 功能验证、外部调用监控 |
| trace() 指令级         | ~50-100x 慢  | 详细分析、逆向工程     |
| vc_make_handle + CODE  | ~10-20x 慢   | 自定义逐指令监控       |
| vc_single_step(ctx, 1) | ~100-200x 慢 | 精确调试               |

---

## Demo

`demo/` 是一个最简命令行示例(**无 Android app 壳**):`main.cpp` 里定义一个自己的 C 函数,用 `trace()`
包装 → 进 VCPU 执行 → 落 trace 日志,核心就三步(`trace()` 拿指针 → 调它 → `freeTrace()`)。

编译、`adb push` 到设备运行、`decode` 还原的完整步骤见 **[demo/README.md](demo/README.md)**。

## 项目结构

```text
AndroidPrybar/
|-- include/                         ← 对外公开头(已分离)
|   |-- vcpu.h                       ←   VCPU 核心 API(对应 libvcpu.a)
|   `-- trace.h                      ←   trace 工具 API(#include "vcpu.h")
|-- libs/
|   |-- arm64-v8a/
|   |   |-- libvcpu.a                ←   闭源资产:VCPU+引擎合并、符号隐藏(静态)
|   |   |-- libvcpu.so               ←   同上(动态,裸 VCPU 用)
|   |   |-- libcapstone.a            ←   trace 依赖(反汇编)
|   |   `-- libdobby.a               ←   trace 依赖(inline hook)
|   `-- prebuilt/arm64-v8a/
|       `-- libtrace.so              ←   预编译成品(克隆即用)
|-- trace/                           ← 开源的 trace 层
|   |-- src/                         ←   trace 源码(EastTrace/JniTrace/…)
|   |-- include/                     ←   trace 自己的头(含 ARM64Emulator.h 兼容垫片)
|   |-- Utils/                       ←   通用工具头(符号在 libvcpu.a 中)
|   |-- thirdparty/include/          ←   编译期用的 unicorn/capstone/dobby 头
|   |-- trace.exports                ←   导出符号版本脚本
|   |-- CMakeLists.txt / build_trace.sh  ← 两种重编方式
|-- demo/                            ← 最简命令行示例(无 app 壳)
|   |-- main.cpp                     ←   trace 自己的一个 C 函数 → 出日志
|   `-- build.sh                     ←   NDK 编成 arm64 可执行、链 libtrace.so
|-- tools/
|   |-- trace_receiver.py            ← TCP 接收 + LZ4 解码工具
|   `-- build_calltree.py            ← trace → 函数调用树/调用图
`-- README.md
```

### `.claude/skills/unicorn-trace/` — Claude Code 用法 skill

本仓库内置一个 **Claude Code skill：**`unicorn-trace`。用 Claude Code 打开本仓库干活时，它会自动带上 libtrace 的完整用法，AI 直接知道怎么调 `trace()` / `vc_make_handle` / 各类 hook，不用每次解释。

- `SKILL.md`：精简速查（两个入口、API 速查表、Hook 类型、实战示例索引、trace 格式、性能、限制）。
- `GUIDE.md`：完整指南（完整 API + 实战示例 + trace 格式）。
- 想在别的项目用：把 `.claude/skills/unicorn-trace/` 复制到那个项目的 `.claude/skills/` 或用户级 `~/.claude/skills/`。

## 交流群 / 联系方式

欢迎大家扫码进群，一起学习和交流 Android Native / Trace / VM 相关内容。

作者微信：`klovemh3344`

群聊：import FacaiTrace

## 个人的碎碎念念

这个工具断断续续写了两年。当时公开好用的 trace 工具不多，相关思路也少有人分享，很多东西只能自己一点点摸索、测试、推倒重来，光是一个支持自动传参的 JIT 就折腾了很久。

现在把它分享出来，用得上就拿去用，随便改、随便抄，不用署名。如果它帮到了你，欢迎进群交流，也算是给我一点继续维护的动力。
