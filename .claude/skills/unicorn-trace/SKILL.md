---
name: unicorn-trace
description: unicornTrace(libtrace.so / ARM64Emulator.h)官方完整用法 —— 把 native 函数放进 unicorn VM 跑，抓逐指令 trace、挂 CODE/MEM/SVC/外部跳转 hook、读写寄存器、单步/断点、CPU 快照、反汇编、内存监控、伪造检测结果。逆向 ARM64 native 算法/调试/绕检测时用。完整 API 与实战代码在本 skill 的 GUIDE.md。
---

# unicornTrace 使用（Prybar API）

把任意 ARM64 native 函数放进 unicorn VM 执行，输出逐指令 trace / 挂 hook 监控 / 改写执行 / 调试。头文件 `ARM64Emulator.h`，库 `libtrace.so`（7.2MB，ARM64、Android10+）。**所有导出是 C ABI，C/C++ 都能调。**

> 📖 **完整 API 说明 + 4 个实战示例 + trace 格式在本 skill 的 [GUIDE.md](GUIDE.md)** —— 需要具体代码/细节时读它。本页是地图与速查。

> 📁 **原始文档/源头**（本 skill 的 GUIDE.md 是它的副本，权威原文/更新看这里）：
> - 原始指南：`unicornTrace/UNICORN_TRACE_GUIDE.md`
> - 头文件 `ARM64Emulator.h` + 库 `libtrace.so` + 测试 `app-debug.apk`：unicornTrace 工程 `D:\AppData\project\Andorid\UnicornVm\unicornTrace\`（export demo：`_export/TraceDemo/app/src/main/cpp/`）
> - 版本更新后：重新从原文档拷 GUIDE.md、从工程取新的 `ARM64Emulator.h`/`libtrace.so`。

## 两个入口

**① 高层 `trace()`：一键全量逐指令 trace（最常用）**
```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(const char*))trace((void*)target, (char*)"/data/local/tmp/out", &ctx);
int r = fn("hi");              // 在 VM 里跑, 逐指令+JNI+SVC+内存访问全落盘
freeTrace((uint64_t)fn);
// replace_trace(target, "/path/out") + restore_function(target): 让 app 自己调时即 trace
```
> 第 2 参是**输出目录**（不能命名文件；多线程并发调用同一函数 → 每线程一个文件）。JNI 调用自动跟踪。

**② 低层 `vc_make_handle` + `vc_hook_add`：自己挂 hook 精细控制**
```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(int,int))vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)on_jump, nullptr, 0, 0);
int r = fn(1,2);
vc_free(ctx);
```

## API 速查表
| API | 用途 |
|-----|------|
| `trace(func,path[,&ctx])` / `freeTrace(wrapper)` | 快速 trace / 释放 |
| `replace_trace(func,path)` / `restore_function(func)` | 全局替换式 trace / 恢复（内部 Dobby inline hook 到 trace 返回的包装指针）|
| `vc_make_handle(func,&ctx)` / `vc_free(ctx)` | 裸 VM 句柄（不带 trace 输出）/ 释放 |
| `vc_hook_add(ctx,&hh,type,cb,ud,begin,end)` / `vc_hook_del(ctx,hh)` | 注册 / 删除 hook |
| `vc_reg_read/write(ctx,reg,&val)` `..._batch(ctx,regs,vals,n)` | 寄存器读写 / 批量 |
| `vc_emu_stop(ctx)` `vc_single_step(ctx,n)` `vc_set_until(ctx,addr)` | 停止 / 单步 N 条 / 临时断点 |
| `vc_context_save/restore/free(...)` | CPU 状态快照 存/恢复/释放 |
| `vc_disasm(addr,count,out)` `vc_lookup_symbol(ctx,addr)` | 反汇编 / 地址查符号 |
| `vc_set_jump_blacklist(names,ranges,n)` `vc_clear_jump_blacklist()` `vc_set_external_jump_enabled(bool)` | 外部跳转控制 |
| `trace_read/write(ctx,begin,end)` | 内存读/写监控 |

## Hook 类型（vc_hook_add 的 type → 回调签名，详见 GUIDE.md）
`VC_HOOK_CODE`/`BLOCK`(逐指令/块) · `MEM_READ`/`MEM_WRITE`/`MEM_ALL`(内存) · `SVC`(系统调用) · `EXTERNAL_JUMP`(外部跳转，可 `*action=VC_ACTION_SKIP` 跳过+`vc_reg_write` 伪造返回) · `INTR`(中断)。begin/end 限地址范围（0=不限）。

## 实战示例（完整代码见 GUIDE.md 对应节）
1. **监控所有文件操作**（SVC hook 过滤 openat/read/write）
2. **全面绕过环境检测**（EXTERNAL_JUMP 拦截 + 伪造返回，绕 root/调试/模拟器检测）
3. **类 LLDB 交互式调试器**（single_step + set_until + 寄存器/反汇编）
4. **SVC 系统调用过滤**
+ trace 某 SO 检测函数、跳转黑名单实战、拦截并伪造检测结果。

## trace 格式（拉回后）
```
libtarget.so+0x1a2b3c 0x7abc..: add w7,w7,w24  w7=0x3 w24=0x5 => w7=0x8  mem_r[0x7f..] ->"str"
```
so+偏移 / 运行时VA / 反汇编 / 寄存器实值 / =>写回 / mem_r|mem_w / 字符串；外部调 `[CALL]`、JNI(符号+签名+X0-X7)、`[SVC]`。

## 性能（越详细越慢）
裸句柄 ~2-5x · trace 指令级 ~50-100x · CODE 逐指令 ~10-20x · 单步 ~100-200x。

## ⚠️ 已知限制
- **仅 ARM64、Android 10+**；**只能 trace native，不能 trace Java 方法**。
- **不支持 VM 嵌套**（handle 内再 `vc_make_handle` 会死锁）。
- 跳转控制只有 blacklist（强制跳出），无 whitelist。
- 崩点前数据可能丢尾（flush 间隔），实质函数崩时调小 flush。

## Frida 里用 trace（用 frida 的 hook 重定向到 trace 返回的指针）
核心：`trace(target, outDir)` 返回一个**包装指针**——调它就在 VM 里跑并 trace。要让 app 自己调 `target` 时进 trace，**直接用 frida 的 hook 把 `target` 重定向到这个包装指针**：
```js
Module.load('/data/local/tmp/libtrace.so');
var trace = new NativeFunction(
    Module.findExportByName('libtrace.so','_Z5tracePvPcPP10vm_context'),
    'pointer', ['pointer','pointer','pointer']);

var target  = Module.findBaseAddress('libfoo.so').add(0x9abc);
var ctxPP   = Memory.alloc(Process.pointerSize);
var wrapper = trace(target, Memory.allocUtf8String('/data/local/tmp/foo_trace'), ctxPP);  // ★ 拿包装指针

// ★ 用 frida 的 hook 把 target 重定向到包装指针
Interceptor.replace(target, wrapper);   // 之后 app 每次调 target → 进 VM trace(落 foo_trace/ 目录)
```
自己能调这个函数时更简单：`var fn = new NativeFunction(wrapper, retType, argTypes); fn(args);`。
> **目标自校验 .text**：`Interceptor.replace` 会改目标 `.text` 被检测 → 把这步换成不改 `.text` 的方式（硬件断点 / 影子页无痕 hook）重定向到包装指针。

## APK 内集成
`ARM64Emulator.h` + link `libtrace.so`，纯 C/C++ 直接调（见 GUIDE.md）。
