# AndroidPrybar

ARM64 函数级 VM 执行与指令跟踪框架。把任意 native 函数放进 Unicorn 引擎中执行，暴露每条指令、外部调用、SVC、内存访问的全部细节。

本工程已包含预编译的 `libtrace.so`、头文件和 JNI 示例。克隆后直接编译运行即可查看结果。

## `libtrace.so` 在哪里

预编译动态库：`app/src/main/jniLibs/arm64-v8a/libtrace.so`

对外头文件：`app/src/main/cpp/include/ARM64Emulator.h`

---

## 快速上手

### trace() — 最简路径，一行出日志

```cpp
#include "ARM64Emulator.h"

auto fn = (int(*)(int))trace((void*)target_func, "/data/data/pkg/trace.txt");
fn(123);                       // 调用 → 自动写 trace 日志
freeTrace((uint64_t)fn);       // 用完释放
```

### vc_make_handle — 带回调的 VM 执行

```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(int, int))vc_make_handle((void*)target_func, &ctx);

// 注册 hook
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);

int result = fn(1, 2);  // 在 VM 中执行
vc_free(ctx);
```

### replace_trace() — 全局替换式 trace

```cpp
replace_trace((void*)func_addr, "/data/data/pkg/trace.txt");
// 之后所有对 func_addr 的调用都自动走 VM trace
restore_function((void*)func_addr);  // 恢复原函数
```

---

## Hook 类型与回调

每种 hook 类型有对应的回调签名：

```cpp
// 拦截外部函数调用（最常用）
void my_jump_cb(vm_context* ctx, uint64_t address,
                const char* symbol_name, vc_event_action* action, void* ud) {
    if (symbol_name && strcmp(symbol_name, "getuid") == 0) {
        uint64_t fake_uid = 0;
        vc_reg_write(ctx, VC_REG_X0, &fake_uid);
        *action = VC_ACTION_SKIP;  // 跳过真实调用
    }
}

// SVC 系统调用监控
void my_svc_cb(vm_context* ctx, uint64_t address, uint32_t syscall_nr, void* ud) {
    LOGD("SVC #%u @ 0x%lx", syscall_nr, address);
}

// 基本块
void my_block_cb(vm_context* ctx, uint64_t address, uint32_t size, void* ud) {
    // 每个基本块入口触发
}

// 内存读写
void my_mem_cb(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    if (type == VC_MEM_WRITE) {
        LOGD("MEM WRITE @ 0x%lx size=%d val=0x%lx", address, size, value);
    }
}
```

### Hook 类型速查

| 类型 | 回调签名 | 用途 |
|------|---------|------|
| `VC_HOOK_BLOCK` | `vc_cb_hookcode_t` | 每个基本块入口 |
| `VC_HOOK_CODE` | `vc_cb_hookcode_t` | 每条指令（慢 ~10x） |
| `VC_HOOK_INTR` | `vc_cb_hookintr_t` | 中断（含 SVC） |
| `VC_HOOK_MEM_READ` | `vc_cb_hookmem_t` | 内存读 |
| `VC_HOOK_MEM_WRITE` | `vc_cb_hookmem_t` | 内存写 |
| `VC_HOOK_MEM_READ_AFTER` | `vc_cb_hookmem_t` | 内存读后（有值） |
| `VC_HOOK_SVC` | `vc_cb_hooksvc_t` | SVC 系统调用 |
| `VC_HOOK_EXTERNAL_JUMP` | `vc_cb_hookjump_t` | 外部函数调用 |

---

## 寄存器读写

```cpp
uint64_t pc, x0;
vc_reg_read(ctx, VC_REG_PC, &pc);
vc_reg_read(ctx, VC_REG_X0, &x0);

uint64_t fake_ret = 0;
vc_reg_write(ctx, VC_REG_X0, &fake_ret);

// 批量读
uint64_t x0_val, x1_val, sp_val;
vc_reg regs[] = { VC_REG_X0, VC_REG_X1, VC_REG_SP };
void* vals[]  = { &x0_val, &x1_val, &sp_val };
vc_reg_read_batch(ctx, regs, vals, 3);

// SIMD/FP 寄存器（128 位）
__uint128_t q0;
vc_reg_read(ctx, VC_REG_Q0, &q0);
```

---

## 跳转控制

### 默认行为

| SO 类型 | 默认行为 |
|---------|---------|
| 目标 SO | VM 内执行 |
| 其他用户 SO | VM 内执行 |
| 系统库（libc 等） | 跳出到 host |

### blacklist — 强制指定 SO 跳出到 host

```cpp
// 某些 SO 不需要 trace，让它们在 host 上跑更快
const char* blacklist[] = { "libutils.so", "libcrypto.so", nullptr };
vc_set_jump_blacklist(blacklist, nullptr, 0);

// 也可以用地址范围
uint64_t ranges[][2] = { { base, base + 0x200000 } };
vc_set_jump_blacklist(nullptr, ranges, 1);

// 清除
vc_clear_jump_blacklist();
```

### 全局开关

```cpp
// 只模拟目标 SO 本身，其他所有用户库都跳出到 host
vc_set_external_jump_enabled(false);
```

---

## 单步与受控执行

所有单步/断点 API 在**回调中调用**。

```cpp
// 单步 — 执行 N 条指令后暂停，再次触发回调链
vc_single_step(ctx, 1);      // 执行 1 条后暂停
vc_single_step(ctx, 100);    // 执行 100 条后暂停
// 回调中不调用 → 恢复正常执行

// 设置停止地址（临时断点），到达后自动清除
vc_set_until(ctx, target_addr);
vc_set_until(ctx, 0);  // 手动清除
```

### 类 LLDB 调试器示例

```cpp
void debugger_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);

    // 反汇编当前指令
    vc_insn insn;
    if (vc_disasm(pc, 1, &insn) > 0) {
        LOGD("0x%lx: %s %s", insn.address, insn.mnemonic, insn.op_str);
    }

    // 打印寄存器
    uint64_t x0, x1, sp;
    vc_reg_read(ctx, VC_REG_X0, &x0);
    vc_reg_read(ctx, VC_REG_X1, &x1);
    vc_reg_read(ctx, VC_REG_SP, &sp);
    LOGD("  X0=0x%lx X1=0x%lx SP=0x%lx", x0, x1, sp);

    vc_single_step(ctx, 1);  // 继续单步
}
```

---

## 反汇编 API

```cpp
// 不需要 vm_context（identity mapping）
vc_insn insns[10];
int count = vc_disasm(address, 10, insns);
for (int i = 0; i < count; i++) {
    LOGD("0x%lx: [%08x] %s %s",
        insns[i].address, insns[i].bytes,
        insns[i].mnemonic, insns[i].op_str);
}
```

---

## VM 控制

```cpp
// 停止 VM
vc_emu_stop(ctx);

// CPU 状态快照
vc_cpu_context* snap = nullptr;
vc_context_save(ctx, &snap);
// ... 执行一些操作 ...
vc_context_restore(ctx, snap);
vc_context_free(snap);

// 符号查询
const char* sym = vc_lookup_symbol(ctx, address);
```

---

## 内存监控

```cpp
// 内置监控（每个 ctx 各只能添加一次）
trace_read(ctx, 0, 0);   // 全范围读监控
trace_write(ctx, 0, 0);  // 全范围写监控

// 自定义 watchpoint
void mem_watch(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    LOGD("WATCHPOINT: [0x%lx] written by PC=0x%lx val=0x%lx", address, pc, value);
}
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_MEM_WRITE, (void*)mem_watch, nullptr,
            watch_addr, watch_addr + 8);
```

---

## 实战示例：绕过环境检测

```cpp
void anti_detect(vm_context* ctx, uint64_t addr,
                 const char* sym, vc_event_action* action, void* ud) {
    if (!sym) return;

    if (strcmp(sym, "access") == 0) {
        uint64_t path_ptr;
        vc_reg_read(ctx, VC_REG_X0, &path_ptr);
        const char* path = (const char*)path_ptr;
        if (strstr(path, "su") || strstr(path, "magisk")) {
            uint64_t ret = (uint64_t)-1;
            vc_reg_write(ctx, VC_REG_X0, &ret);
            *action = VC_ACTION_SKIP;
        }
    }
    else if (strcmp(sym, "getuid") == 0) {
        uint64_t ret = 10000;
        vc_reg_write(ctx, VC_REG_X0, &ret);
        *action = VC_ACTION_SKIP;
    }
    else if (strcmp(sym, "exit") == 0 || strcmp(sym, "abort") == 0) {
        *action = VC_ACTION_SKIP;
    }
}

vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)anti_detect, nullptr, 0, 0);
fn();
vc_free(ctx);
```

---

## trace 输出格式

每行一条指令：
```
so+0xOFFSET 0xPC: mnemonic operands reg_reads => reg_writes  mem_r[0xADDR] / mem_w[0xADDR]=VAL
```

- **str:"..."** — 访问地址是 C 字符串时完整输出
- **->"..."** — 加载的值是字符串指针时输出指向的字符串
- **[CALL] func(args)** — 外部函数调用
- **>>> JNIEnv->Method()** — JNI 调用

---

## API 速查表

| API | 用途 |
|-----|------|
| `trace(func, path)` | 快速 trace |
| `trace(func, path, &ctx)` | 带 ctx 的 trace |
| `freeTrace(wrapper)` | 释放 trace 句柄 |
| `replace_trace(func, path)` | 全局替换式 trace |
| `restore_function(func)` | 恢复被替换的函数 |
| `vc_make_handle(func, &ctx)` | 创建裸 VM 句柄 |
| `vc_free(ctx)` | 释放 VM 上下文 |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | 注册 hook |
| `vc_hook_del(ctx, hh)` | 删除 hook |
| `vc_reg_read / vc_reg_write` | 寄存器读写 |
| `vc_reg_read_batch / vc_reg_write_batch` | 批量读写 |
| `vc_emu_stop(ctx)` | 停止 VM |
| `vc_single_step(ctx, count)` | 执行 N 条后暂停 |
| `vc_set_until(ctx, addr)` | 设置临时断点 |
| `vc_disasm(addr, count, out)` | 反汇编 |
| `vc_context_save / restore / free` | CPU 快照 |
| `vc_lookup_symbol(ctx, addr)` | 地址查符号 |
| `vc_set_jump_blacklist(names, ranges, n)` | 设置跳转黑名单 |
| `vc_clear_jump_blacklist()` | 清除黑名单 |
| `vc_set_external_jump_enabled(enabled)` | 全局跳转开关 |
| `trace_read / trace_write` | 内存监控 |

## 性能参考

| 模式 | 速度 | 适用场景 |
|------|------|---------|
| vc_make_handle（默认） | ~2-5x 慢 | 功能验证、外部调用监控 |
| trace() 指令级 | ~50-100x 慢 | 详细分析、逆向工程 |
| vc_make_handle + CODE | ~10-20x 慢 | 自定义逐指令监控 |
| vc_single_step(ctx, 1) | ~100-200x 慢 | 精确调试 |

---

## Demo 工程说明

点击界面上的 `Run Native Demo` 后会按顺序运行：

1. **自定义回调示例** — `vc_make_handle` + 事件回调 + 伪造返回值
2. **JNI trace 示例** — `trace()` 在 Android 环境中跟踪 JNI 调用
3. **原子指令测试** — LDXR/STXR/CAS/SWP/LDADD + std::atomic 正确性验证
4. **普通 trace 示例** — 最小接入路径

运行后在 `filesDir` 下生成日志文件，界面输出中显示完整路径。

## 项目结构

```text
AndroidPrybar/
|-- TraceDemo/                       ← Android 接入示例工程
|   `-- app/src/main/
|       |-- cpp/include/ARM64Emulator.h  ← API 头文件
|       |-- cpp/native-lib.cpp           ← Demo 示例代码
|       `-- jniLibs/arm64-v8a/libtrace.so ← 预编译库
|-- tools/
|   `-- build_calltree.py            ← trace → 函数调用树/调用图
|-- .claude/skills/unicorn-trace/    ← 附带的 Claude Code 用法 skill
`-- README.md
```

## 附带工具

### `tools/build_calltree.py` — trace → 函数调用树

从 trace 重建函数调用树/调用图（谁调了谁、各函数调用次数）。脚本侧建树，部分/被打断的 trace 也能建；新引擎的多线程输出（每线程一文件的目录）传目录即自动每线程一棵树。

```bash
python tools/build_calltree.py <trace文件或目录> [so名] [入口偏移hex] [最大行数]
# 输出: <base>_calltree.txt(调用树) + <base>_callgraph.txt(调用图/计数)
```

### `.claude/skills/unicorn-trace/` — Claude Code 用法 skill

本仓库内置一个 **Claude Code skill：`unicorn-trace`**。用 Claude Code 打开本仓库干活时，它会自动带上 libtrace 的完整用法，AI 直接知道怎么调 `trace()` / `vc_make_handle` / 各类 hook，不用每次解释。
- `SKILL.md`：精简速查（两个入口、API 速查表、Hook 类型、实战示例索引、trace 格式、性能、限制）。
- `GUIDE.md`：完整指南（完整 API + 实战示例 + trace 格式）。
- 想在别的项目用：把 `.claude/skills/unicorn-trace/` 复制到那个项目的 `.claude/skills/` 或用户级 `~/.claude/skills/`。

## 交流群 / 联系方式

欢迎扫码进群，一起学习和交流 Android Native / Trace / VM 相关内容。

![交流群二维码](./TraceDemo/59fb9c9ea8fcff2c0c8ae12d2064b25e.jpg)

## 个人的碎碎念念

这个工具我断断续续写了两年。当时几乎没有好用且公开的 trace 工具，也无人分享开发此类工具的方法。我一点点摸索、测试、推倒重来，仅为了编写一个支持自动传参的 JIT，就思考测试了许久。

同时，我知道在 AI 时代，即使不开放源码，通过 IDA 反汇编也可以很快复刻出工具并学习其中的思路。但这并不重要。

不求署名，不求分润，不求你在任何地方提及我的名字。我只求你在按下"复制"或"生成"之前，在心里，对这段熬了两年的代码，说一声谢谢。不需要我这份刚燃起开源的心冷却下去，不需要写在任何文档里。只要你自己知道，这东西不是凭空从石头里蹦出来的，它是某个人在无数个深夜里，一行一行从混沌里捞出来的，这就够了。
