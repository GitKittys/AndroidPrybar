---
name: unicorn-trace
description: unicornTrace (libtrace.so) 用法速查 — trace() 一键逐指令 trace、vc_make_handle + typed hook 精细控制、寄存器读写、单步/断点、CPU 快照、反汇编。逆向 ARM64 native 时用。完整 API + 实战代码见 GUIDE.md。
---

# unicornTrace

把 ARM64 native 函数放进 Unicorn VM 跑：逐指令 trace、hook 外部调用、伪造返回值、单步调试。用于算法还原、加密分析、绕过检测。仅 native（非 Java）、仅 ARM64、Android 10+。单函数级 VM，不是全进程模拟器。

完整 API + 实战代码见 [GUIDE.md](GUIDE.md)。
相关：`build-trace` command（编译部署）· `native-trace-capture`（Frida 端用法）· `trace-analysis-toolkit`（trace 解析）

### 附带文件

| 文件 | 说明 |
|------|------|
| `libtrace.so` | 生产版（~8MB）。20MB LZ4 输出缓冲，高吞吐，崩溃时可能丢尾部数据 |
| `libtrace_debug.so` | Debug 版。256B 输出缓冲（最多攒 1-2 条 trace 就刷盘），崩溃前数据不丢。另含内存映射额外日志。性能较低，排查崩溃/丢数据时用 |
| `ARM64Emulator.h` | C ABI 头文件（vm_context 为不透明类型）|
| `trace_receiver.py` | LZ4 trace 解码 + TCP 接收脚本（`decode` / `receive`） |
| `GUIDE.md` | 完整 API 说明 + 回调签名 + 实战代码 |

---

## 两个入口

**① `trace()` — 一键全量 trace（最常用）**
```cpp
auto fn = (int(*)(int))trace((void*)target, (char*)"/data/local/tmp/out", &ctx);
int r = fn(42);           // VM 里跑，逐指令+JNI+SVC 全记录
freeTrace((uint64_t)fn);  // 释放
```
- 第 2 参是**输出目录**（不能指定文件名；多线程每线程一个 `.lz4` 文件）
- JNI 调用自动跟踪（有 ART 环境时）
- `replace_trace(target, path)` / `restore_function(target)`：全局替换式 trace

**② `vc_make_handle()` — 裸 VM 句柄 + 自定义 hook**
```cpp
auto fn = (int(*)(int,int))vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)on_jump, nullptr, 0, 0);
int r = fn(1,2);
vc_free(ctx);   // 注意：vc_make_handle 用 vc_free，trace() 用 freeTrace
```

---

## API 速查表

| API | 用途 |
|-----|------|
| `trace(func, path[, &ctx])` / `freeTrace(wrapper)` | 一键 trace / 释放 |
| `replace_trace(func, path)` / `restore_function(func)` | 全局替换式 trace / 恢复 |
| `vc_make_handle(func, &ctx)` / `vc_free(ctx)` | 裸 VM 句柄 / 释放 |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` / `vc_hook_del(ctx, hh)` | 注册 / 删除 hook |
| `vc_reg_read/write(ctx, reg, &val)` / `..._batch(ctx, regs, vals, n)` | 寄存器读写 / 批量 |
| `vc_emu_stop(ctx)` | 停止 VM |
| `vc_single_step(ctx, n)` / `vc_set_until(ctx, addr)` | 单步 N 条 / 临时断点 |
| `vc_context_save/restore/free(...)` | CPU 快照 |
| `vc_disasm(addr, count, out)` / `vc_lookup_symbol(ctx, addr)` | 反汇编 / 符号查询 |
| `vc_set_jump_blacklist(names, ranges, n)` / `vc_clear_jump_blacklist()` | 跳转黑名单 |
| `vc_set_external_jump_enabled(bool)` | 全局跳转开关 |
| `trace_read/write(ctx, begin, end)` | 内存读写监控 |

## Hook 类型

| type | 回调签名 | 触发时机 |
|------|---------|---------|
| `VC_HOOK_BLOCK` | `vc_cb_hookcode_t` | 基本块入口 |
| `VC_HOOK_CODE` | `vc_cb_hookcode_t` | 每条指令（慢 ~10x） |
| `VC_HOOK_EXTERNAL_JUMP` | `vc_cb_hookjump_t` | 外部函数调用（可 SKIP + 伪造返回） |
| `VC_HOOK_SVC` | `vc_cb_hooksvc_t` | SVC 系统调用 |
| `VC_HOOK_MEM_READ/WRITE/ALL` | `vc_cb_hookmem_t` | 内存读写 |
| `VC_HOOK_INTR` | `vc_cb_hookintr_t` | 中断 |

begin/end 限地址范围（0 = 不限）。

---

## Trace 输出

### 输出格式
所有输出为 LZ4 压缩（`.lz4` 文件），需用 `trace_receiver.py decode` 还原为文本：
```bash
python trace_receiver.py decode *.lz4      # 每个 .lz4 → 对应 .log
```

每行一条指令：
```
[HH:MM:SS.mmm][so名 0xOFFSET] [机器码] 地址: "助记符" 读寄存器 => 写寄存器  mem_r/w  ->"字符串"
```
特殊标签：`[CALL] func(args)` · `>>> JNIEnv->Method()` · `[SVC #N]`

### 两种输出目标

| logPath | 输出方式 | 用法 |
|---------|---------|------|
| `"/data/.../dir"` | 本地 LZ4 文件 | 跑完 `adb pull` 回来，`trace_receiver.py decode` |
| `"tcp:9876"` 或 `"tcp:"` | TCP 实时流到 PC | PC 先 `trace_receiver.py receive`，app 再调 trace |

TCP 适合大 trace（不落设备盘）和解释器主循环等长驻场景。

### 多线程
同一 trace 函数被多线程并发调用时，每线程自动分独立文件（thread_local 隔离），无需加锁。

---

## Frida 用法
```js
Module.load('/data/local/tmp/libtrace.so');
var trace = new NativeFunction(
    Module.findExportByName('libtrace.so','_Z5tracePvPcPP10vm_context'),
    'pointer', ['pointer','pointer','pointer']);
var target  = Module.findBaseAddress('libfoo.so').add(0x9abc);
var wrapper = trace(target, Memory.allocUtf8String('/data/local/tmp/out'), Memory.alloc(8));
Interceptor.replace(target, wrapper);  // app 调 target 时自动进 VM trace
```
目标自校验 `.text` 时，把 `Interceptor.replace` 换成无痕方式（硬件断点 / 影子页 hook）。

---

## 性能

| 模式 | 速度 |
|------|------|
| `vc_make_handle`（裸 VM） | ~2-5x |
| `trace()`（指令级） | ~50-100x |
| `vc_make_handle` + CODE | ~10-20x |
| `vc_single_step(ctx, 1)` | ~100-200x |

## 已知限制
- 仅 ARM64，Android 10+；只能 trace native，不能 trace Java 方法
- 不支持 VM 嵌套（handle 内再 `vc_make_handle` 会死锁）
- 跳转控制只有 blacklist，无 whitelist
- 崩点前数据可能丢尾（LZ4 缓冲区未 flush）

回调签名、完整代码示例、集成方式 → 见 [GUIDE.md](GUIDE.md)。
