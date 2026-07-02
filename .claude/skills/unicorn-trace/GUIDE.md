# unicornTrace 使用指南

> libtrace.so — ARM64 函数级 VM 执行与指令跟踪库

## 概述

`libtrace.so` 把目标 ARM64 native 函数放进 Unicorn 引擎中执行，暴露每条指令、外部调用、SVC、内存访问的全部细节。

---

## 快速上手：两个最常用的封装

**90% 的场景只需要这两个函数，传入地址 + 日志路径即可。**

### trace 深度说明

- **trace 会深入目标 SO 内的所有子函数调用**（BL/BLR 跳转到 SO 内部地址 → Unicorn 继续模拟执行）
- **同进程的其他用户 SO 也会被拉进 VM 模拟**（默认行为，见"跳转控制"章节）
- **系统库（libc、libart 等）不会被 trace** — 跳到系统库触发 `external_jump`，在 host 上执行后返回 VM
- **JNI 调用会被 trace 记录**（`trace()` 自动开启，见下方说明）
- 这就是默认行为，不需要额外配置

### trace() — 传入地址和日志路径，返回可调用句柄

```cpp
#include "ARM64Emulator.h"

// === 最简用法：两个参数 ===
auto fn = (bool(*)())trace((void*)func_addr, "/data/data/pkg/trace.txt");
fn();                          // 调用 → 自动写 trace 日志
freeTrace((uint64_t)fn);       // 用完释放

// === 带 ctx：三个参数，可注册 hook ===
vm_context* ctx = nullptr;
auto fn = (bool(*)())trace((void*)func_addr, "/data/data/pkg/trace.txt", &ctx);
// 通过 vc_hook_add 注册回调来监控/干预执行
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_cb, nullptr, 0, 0);
fn();
freeTrace((uint64_t)fn);
```

### JNI 调用跟踪（trace() 自动开启）

**`trace()` 内部已经自动开启了 JNI trace，不需要手动设置。** 内部流程：

1. `fake_dlopen("/apex/com.android.art/lib64/libart.so")` 获取 ART 句柄
2. `JNI_GetCreatedJavaVMs()` 拿到当前进程的 `JavaVM*`
3. `JavaVM->GetEnv()` 获取 `JNIEnv*`
4. 拿到 JNIEnv 后调 `addJniTrace()` 注册 JNI 调用拦截 hook

所以只要是在有 ART 的 Android 进程中（正常 App 环境），trace 输出会自动包含所有 JNI 调用（FindClass、GetMethodID、CallObjectMethod 等）。

**如果 trace 输出没有 JNI 日志，可能的原因：**
- 目标函数本身没有 JNI 调用（纯计算函数）
- 进程中没有 JavaVM（纯 native 进程、独立测试程序）
- `libart.so` 路径不对（不同 Android 版本路径可能不同）

**手动开启（用 vc_make_handle 时）：**

`vc_make_handle` 默认不开启 JNI trace。需要通过 `trace()` 封装来自动开启，
或者在内部调用 `addJniTrace()` 注册 hook。

**函数签名：**
```cpp
void* trace(void* funcAddr, char* logPath);                    // 简版
void* trace(void* funcAddr, char* logPath, vm_context** ctx);  // 带 ctx
void  freeTrace(uint64_t wrapper_func);                        // 释放
```

### replace_trace() — 传入地址和日志路径，全局替换

不需要改调用方，直接 hook 原函数入口。**之后所有对该函数的调用都自动走 VM trace。**

```cpp
// 一行搞定：所有调用者自动被 trace
replace_trace((void*)func_addr, "/data/data/pkg/trace.txt");

// ... app 正常运行，所有调用 func_addr 的地方都会被记录 ...

restore_function((void*)func_addr);  // 恢复原函数
```

**函数签名：**
```cpp
void* replace_trace(void* target_func, char* logPath);
void  restore_function(void* target_func);
```

### 实战：trace 某个 SO 的检测函数

```cpp
void* base = /* SO 基址 (dlopen 或 /proc/self/maps) */;

// 快速检测函数 (偏移 +0x1234)
auto check = (bool(*)())trace(
    (void*)((uint64_t)base + 0x1234),
    "/data/data/com.example.app/check_trace.txt");
bool result = check();
LOGD("check() = %d", result);
freeTrace((uint64_t)check);

// 完整检测引擎 (偏移 +0x5678) — 全局替换式
replace_trace(
    (void*)((uint64_t)base + 0x5678),
    "/data/data/com.example.app/engine_trace.txt");
// app 自己调用该函数时自动 trace
```

### trace 输出格式

每行一条指令：
```
so+0xOFFSET 0xPC: mnemonic operands reg_reads => reg_writes  mem_r[0xADDR] / mem_w[0xADDR]=VAL
```

- **so+0xOFFSET**: 模块名 + 模块内偏移（可直接对应 IDA 地址）
- **0xPC**: 绝对虚拟地址（用于混淆代码中 LR 跳转目标分析）
- **reg_reads**: 指令读取的寄存器当前值（只显示变化的寄存器）
- **=> reg_writes**: 指令写入的寄存器新值
- **mem_r[0xADDR]**: 内存读取地址
- **mem_w[0xADDR]=VAL**: 内存写入地址和值
- **str:"..."**: 访问地址本身是 C 字符串时，完整输出（`ldr x`/`str x` 级别触发，`ldrb` 等不触发）
- **->"..."**: 加载的值是字符串指针时，完整输出指向的字符串

特殊标签：
- `[CALL] func(args)` — 外部函数调用（regex: `\[CALL\]\s+(\w+)\(`）
- `>>> JNIEnv->Method() => result` — JNI 调用（regex: `>>>\s+JNIEnv->`）

示例：
```
libtarget.so+0x35a6e0 0x727a35a6e0: stp x29, x30, [sp, #-0x30]! x29=0x764a233e00 x30=0x727a290130 sp=0x764a233df0
libtarget.so+0x35a710 0x727a35a710: blr x8 x8=0x70c04759f0
  [CALL] getuid()
libtarget.so+0x35a714 0x727a35a714: cmp w0, #0x15f8f => nzcv=0x60000000
libtarget.so+0x73634 0x727a273634: str q0, [x8, #0x20] q0=0x0a000000140000001e00000028000000 x8=0x764a233da0  mem_w[0x764a233dc0]=0x0a000000140000001e00000028000000
libtarget.so+0x86fd8 0x727a286fd8: ldr w8, [sp, #0x1c] sp=0x764a233d70 => w8=0x7  mem_r[0x764a233d8c]
libtarget.so+0x86fe0 0x727a286fe0: ldr x1, [x8] x8=0x764a233da0 => x1=0x727a2f0100  mem_r[0x764a233da0] ->"/data/data/com.example.app/files/config.json"
libtarget.so+0x86fe8 0x727a286fe8: ldr x2, [sp, #0x30] sp=0x764a233d70  mem_r[0x764a233da0] str:"ro.build.fingerprint"
    >>> JNIEnv->FindClass("com/example/MyClass") => 0x1 @ [libtarget.so]0x26f46c
```

---

## 进阶：带回调的 VM 执行

当你需要**实时监控和干预执行**（拦截外部调用、伪造返回值）时，用 `vc_make_handle` + `vc_hook_add`。

### vc_make_handle — 创建 VM 托管句柄

```cpp
vm_context* ctx = nullptr;
auto vm_fn = (int(*)(int, int))vc_make_handle((void*)original_func, &ctx);

// 注册你关心的 hook
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);

int result = vm_fn(1, 2);  // 在 VM 中执行
vc_free(ctx);               // 释放
```

### Hook 类型与回调

每种 hook 类型有对应的回调签名，按需注册：

```cpp
// --- 拦截外部函数调用（最常用）---
void my_jump_cb(vm_context* ctx, uint64_t address,
                const char* symbol_name, vc_event_action* action, void* ud) {
    LOGD("call: %s @ 0x%lx", symbol_name, address);

    // 伪造 getuid 返回值
    if (symbol_name && strcmp(symbol_name, "getuid") == 0) {
        uint64_t fake_uid = 0;
        vc_reg_write(ctx, VC_REG_X0, &fake_uid);
        *action = VC_ACTION_SKIP;  // 跳过真实调用
    }
}

// --- SVC 系统调用监控 ---
void my_svc_cb(vm_context* ctx, uint64_t address, uint32_t syscall_nr, void* ud) {
    LOGD("SVC #%u @ 0x%lx", syscall_nr, address);
}

// --- 基本块 / 逐指令 ---
void my_block_cb(vm_context* ctx, uint64_t address, uint32_t size, void* ud) {
    // 每个基本块入口触发
}

// --- 内存读写 ---
void my_mem_cb(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    if (type == VC_MEM_WRITE) {
        LOGD("MEM WRITE @ 0x%lx size=%d val=0x%lx", address, size, value);
    }
}
```

### 注册与删除 hook

```cpp
vc_hook_h hh1, hh2, hh3;

// 外部跳转
vc_hook_add(ctx, &hh1, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);

// SVC
vc_hook_add(ctx, &hh2, VC_HOOK_SVC, (void*)my_svc_cb, nullptr, 0, 0);

// 基本块（begin/end 可限定地址范围，0,0 = 全范围）
vc_hook_add(ctx, &hh3, VC_HOOK_BLOCK, (void*)my_block_cb, nullptr, 0, 0);

// 逐指令（性能影响大，按需开启）
// vc_hook_add(ctx, &hh, VC_HOOK_CODE, (void*)my_code_cb, nullptr, 0, 0);

// 内存读写
// vc_hook_add(ctx, &hh, VC_HOOK_MEM_WRITE, (void*)my_mem_cb, nullptr, 0, 0);

// 不需要的 hook 可以删除
vc_hook_del(ctx, hh3);
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
// 单个读写
uint64_t pc, x0;
vc_reg_read(ctx, VC_REG_PC, &pc);
vc_reg_read(ctx, VC_REG_X0, &x0);

uint64_t fake_ret = 0;
vc_reg_write(ctx, VC_REG_X0, &fake_ret);

// 批量读（一次读多个寄存器，减少调用次数）
uint64_t x0_val, x1_val, sp_val;
vc_reg regs[] = { VC_REG_X0, VC_REG_X1, VC_REG_SP };
void* vals[]  = { &x0_val, &x1_val, &sp_val };
vc_reg_read_batch(ctx, regs, vals, 3);

// 批量写
uint64_t new_x0 = 0, new_x1 = 1;
vc_reg regs_w[] = { VC_REG_X0, VC_REG_X1 };
void* vals_w[]  = { &new_x0, &new_x1 };
vc_reg_write_batch(ctx, regs_w, (void* const*)vals_w, 2);

// 读 SIMD/FP 寄存器（128 位）
__uint128_t q0;
vc_reg_read(ctx, VC_REG_Q0, &q0);

// 读 64 位浮点寄存器
uint64_t d0;
vc_reg_read(ctx, VC_REG_D0, &d0);

// 读条件标志
uint64_t nzcv;
vc_reg_read(ctx, VC_REG_NZCV, &nzcv);
// nzcv 高 4 位: N(负) Z(零) C(进位) V(溢出)
```

---

## 跳转控制

### 默认行为

| SO 类型 | 默认行为 | 说明 |
|---------|---------|------|
| 目标 SO（vc_make_handle 传入的函数所在 SO）| VM 内执行 | 自动注册到 vm_resident 表 |
| 其他用户 SO（非系统库）| **VM 内执行** | 首次跳入时 isJumpExternal 判定后加入 vm_resident |
| 系统库（libc、libart、linker 等）| 跳出到 host | external_jump 桥接执行 |

### vc_set_jump_blacklist — 强制指定 SO 跳出到 host

默认情况下，用户 SO 之间的调用也会在 VM 内模拟。如果某个 SO 不需要被 trace（比如纯工具库），
可以把它加入黑名单，强制跳出到 host 执行：

```cpp
// 场景：目标 SO 依赖 libutils.so 和 libcrypto.so，
// 这两个库不需要 trace，让它们在 host 上跑更快
const char* blacklist[] = { "libutils.so", "libcrypto.so", nullptr };
vc_set_jump_blacklist(blacklist, nullptr, 0);

// 之后创建句柄，blacklist 全局生效
vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target_func, &ctx);
int result = fn();
vc_free(ctx);
```

```cpp
// 也可以用地址范围指定黑名单（不依赖 SO 名）
void* libcrypto = dlopen("libcrypto.so", RTLD_NOW);
Dl_info info;
dladdr(dlsym(libcrypto, "EVP_DigestInit"), &info);
uint64_t ranges[][2] = {
    { (uint64_t)info.dli_fbase, (uint64_t)info.dli_fbase + 0x200000 }
};
vc_set_jump_blacklist(nullptr, ranges, 1);
```

```cpp
// 混合：名称 + 地址范围
const char* names[] = { "libssl.so", nullptr };
uint64_t ranges[][2] = {
    { 0x7200000000, 0x7200100000 }  // 某匿名 JIT 区域
};
vc_set_jump_blacklist(names, ranges, 1);
```

```cpp
// 清除所有黑名单（恢复默认行为）
vc_clear_jump_blacklist();
```

### vc_set_external_jump_enabled — 全局开关

```cpp
// 禁止所有用户 SO 留在 VM 内 → 只模拟目标 SO 本身，
// 其他所有用户库都跳出到 host
vc_set_external_jump_enabled(false);

// 再配合 blacklist 做精细控制：
// false + 无 blacklist = 只跑目标 SO，其他全跳出
// true（默认）+ blacklist = 全部留在 VM 内，blacklist 里的跳出
```

### 实战场景

```cpp
// 场景 1：只 trace 目标 SO，其他库全部跳出（最快）
vc_set_external_jump_enabled(false);
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);

// 场景 2：目标 SO + 依赖的 libpayment.so 一起 trace，其他跳出
vc_set_external_jump_enabled(false);  // 先全禁
// libpayment.so 不在 blacklist → 但 enabled=false 所以也跳出？
// 不对！enabled=false 让所有非目标 SO 跳出，包括 libpayment.so
// 目前没有 whitelist API，这个场景需要用 enabled=true（默认）+ blacklist 排除不想要的

// 正确做法：保持默认 enabled=true，把不想 trace 的库加 blacklist
const char* bl[] = { "libc++_shared.so", "liblog.so", nullptr };
vc_set_jump_blacklist(bl, nullptr, 0);
// 目标 SO + libpayment.so 都在 VM 内，libc++/liblog 跳出

// 场景 3：trace 时排除慢速的加密库
vm_context* ctx = nullptr;
const char* bl[] = { "libcrypto.so", "libssl.so", nullptr };
vc_set_jump_blacklist(bl, nullptr, 0);
auto fn = (int(*)())trace((void*)target,
    "/data/data/pkg/trace.txt", &ctx);
fn();  // 加密操作在 host 上跑，不 trace，速度快很多
freeTrace((uint64_t)fn);
```

---

## VM 控制 API

### 停止执行

```cpp
// 在任意回调中调用，VM 会在当前 TB 执行完后停止
vc_emu_stop(ctx);

// 常见用途：检测到危险调用时紧急停机
void my_jump_cb(vm_context* ctx, uint64_t addr,
                const char* sym, vc_event_action* action, void* ud) {
    if (sym && strcmp(sym, "abort") == 0) {
        LOGD("检测到 abort，停止 VM");
        vc_emu_stop(ctx);
    }
}
```

### CPU 状态快照

```cpp
// 保存当前 CPU 状态 → 执行一段逻辑 → 恢复到保存点（类似存档/读档）
void my_block_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);

    if (pc == interesting_addr) {
        // 保存快照
        vc_cpu_context* snap = nullptr;
        vc_context_save(ctx, &snap);

        // ... 让函数继续执行到某个结果 ...

        // 不满意？恢复到快照点重新来
        vc_context_restore(ctx, snap);
        // 修改参数再试
        uint64_t new_arg = 42;
        vc_reg_write(ctx, VC_REG_X0, &new_arg);

        vc_context_free(snap);  // 用完释放
    }
}
```

```cpp
// 实战：fuzz 某个函数的不同输入
vm_context* ctx = nullptr;
auto fn = (int(*)(int))vc_make_handle((void*)target, &ctx);

vc_hook_h hh;
vc_cpu_context* entry_snap = nullptr;
bool first = true;
int test_inputs[] = { 0, 1, -1, 0x7fffffff, 0x80000000 };
int input_idx = 0;

auto block_cb = [](vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    // 在函数入口保存快照，后续通过 restore 反复执行
};
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)(vc_cb_hookcode_t)block_cb, nullptr, 0, 0);
fn(test_inputs[0]);
vc_free(ctx);
```

### 符号查询

```cpp
// 地址 → 函数名（如果有符号表）
const char* sym = vc_lookup_symbol(ctx, address);
if (sym) {
    LOGD("0x%lx = %s", address, sym);
} else {
    LOGD("0x%lx (no symbol)", address);
}
```

---

## 单步与受控执行

所有单步/断点 API 都在**回调中调用**，控制 VM 在回调返回后的执行行为。

### vc_single_step — 执行 N 条指令后暂停

```cpp
// 在回调中调用。回调返回后 VM 执行 N 条指令，然后再次触发回调链。
// 回调中不调用此函数 → 恢复正常（无限制）执行。

// 最简单的单步调试器
void step_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    LOGD("STEP @ 0x%lx", pc);

    // 继续单步（不调用则恢复正常执行）
    vc_single_step(ctx, 1);
}

vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)step_cb, nullptr, 0, 0);
// 第一次触发需要在调用前设置
vc_single_step(ctx, 1);
fn();
vc_free(ctx);
```

```cpp
// 跑 100 条指令看一次状态，类似 stepi 100
void batch_step_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc, x0, x1;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    vc_reg_read(ctx, VC_REG_X0, &x0);
    vc_reg_read(ctx, VC_REG_X1, &x1);
    LOGD("PC=0x%lx X0=0x%lx X1=0x%lx", pc, x0, x1);

    vc_single_step(ctx, 100);  // 再跑 100 条
}
```

### vc_set_until — 设置停止地址（临时断点）

```cpp
// 执行到 addr 时停止，等价于 GDB 的 "until"
// 到达后自动清除，传 0 手动清除

// 跑到函数末尾的 ret 指令停下来
void my_block_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);

    if (pc == func_entry) {
        // 设置断点在函数返回处
        vc_set_until(ctx, func_entry + 0x120);  // ret 指令地址
    }

    if (pc == func_entry + 0x120) {
        // 到达断点！读返回值
        uint64_t ret;
        vc_reg_read(ctx, VC_REG_X0, &ret);
        LOGD("函数返回值: 0x%lx", ret);

        // 继续执行（until 到达后已自动清除）
    }
}
```

```cpp
// 组合使用：先跑到某个地址，再单步分析关键代码段
void debug_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);

    if (pc == crypto_start) {
        LOGD("进入加密函数，开始单步");
        vc_single_step(ctx, 1);
    } else if (pc > crypto_start && pc < crypto_end) {
        // 在加密函数内单步
        vc_insn insn;
        vc_disasm(pc, 1, &insn);
        LOGD("  0x%lx: %s %s", pc, insn.mnemonic, insn.op_str);
        vc_single_step(ctx, 1);
    } else if (pc == crypto_end) {
        LOGD("加密函数执行完毕，恢复正常执行");
        // 不调用 vc_single_step → 自动恢复正常执行
    }
}

vm_context* ctx = nullptr;
auto fn = (void(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)debug_cb, nullptr, 0, 0);
vc_set_until(ctx, crypto_start);  // 先跑到加密函数入口
fn();
vc_free(ctx);
```

---

## 反汇编 API

```cpp
// 反汇编指定地址的 ARM64 指令（不需要 vm_context，identity mapping）
vc_insn insns[10];
int count = vc_disasm(address, 10, insns);
for (int i = 0; i < count; i++) {
    printf("0x%lx: [%08x] %s %s\n",
        insns[i].address, insns[i].bytes,
        insns[i].mnemonic, insns[i].op_str);
}
// 输出:
// 0x727a35a6e0: [a9ba7bfd] stp x29, x30, [sp, #-0x60]!
// 0x727a35a6e4: [910003fd] mov x29, sp
// 0x727a35a6e8: [f9000bf3] str x19, [sp, #0x10]
```

`vc_insn` 结构：
| 字段 | 类型 | 说明 |
|------|------|------|
| `address` | `uint64_t` | 指令地址 |
| `size` | `uint8_t` | 指令字节数（ARM64 固定 4）|
| `bytes` | `uint32_t` | 原始机器码 |
| `mnemonic` | `char[32]` | 助记符（如 "ldr", "mov"）|
| `op_str` | `char[160]` | 操作数（如 "x0, [sp, #0x10]"）|

```cpp
// 实战：在 BLOCK 回调中反汇编整个基本块
void disasm_block(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    int insn_count = size / 4;
    vc_insn* insns = (vc_insn*)alloca(insn_count * sizeof(vc_insn));
    int got = vc_disasm(addr, insn_count, insns);
    LOGD("=== BLOCK @ 0x%lx (%d insns) ===", addr, got);
    for (int i = 0; i < got; i++) {
        LOGD("  0x%lx: %s %s", insns[i].address, insns[i].mnemonic, insns[i].op_str);
    }
}
```

```cpp
// 搜索特定指令模式（比如找 SVC）
void find_svc(uint64_t start, size_t length) {
    for (uint64_t pc = start; pc < start + length; pc += 4) {
        vc_insn insn;
        if (vc_disasm(pc, 1, &insn) > 0 && strcmp(insn.mnemonic, "svc") == 0) {
            LOGD("SVC @ 0x%lx: %s %s", pc, insn.mnemonic, insn.op_str);
        }
    }
}
```

---

## 内存监控

### 内置 trace_read / trace_write

```cpp
// 监控指定地址范围的内存读写（每个 ctx 各只能添加一次）
vm_context* ctx = nullptr;
auto fn = (void(*)())vc_make_handle((void*)target, &ctx);

// 监控 [0x7200000000, 0x7200001000) 范围的读写
trace_read(ctx, 0x7200000000, 0x7200001000);
trace_write(ctx, 0x7200000000, 0x7200001000);

// 全范围监控（0, 0 = 不限地址）
trace_read(ctx, 0, 0);
trace_write(ctx, 0, 0);

// 带 FILE* 输出（裸 vc_make_handle 时使用）
FILE* f = fopen("/data/data/pkg/mem.txt", "w");
trace_read(ctx, 0, 0, f);
trace_write(ctx, 0, 0, f);

fn();
vc_free(ctx);
```

### 自定义内存 hook（更灵活）

```cpp
// 用 VC_HOOK_MEM_WRITE 监控特定变量被谁写了
uint64_t watch_addr = /* 全局变量地址 */;

void mem_watch(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    if (address == *(uint64_t*)ud) {
        uint64_t pc;
        vc_reg_read(ctx, VC_REG_PC, &pc);
        LOGD("WATCHPOINT: [0x%lx] 被 PC=0x%lx 写入 0x%lx (size=%d)",
             address, pc, value, size);
    }
}

vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_MEM_WRITE, (void*)mem_watch,
            &watch_addr, watch_addr, watch_addr + 8);
```

```cpp
// 用 VC_HOOK_MEM_READ_AFTER 追踪从哪里读了什么值
void mem_read_after(vm_context* ctx, vc_mem_type type,
                    uint64_t address, int size, int64_t value, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    LOGD("READ [0x%lx] = 0x%lx (size=%d) @ PC=0x%lx",
         address, value, size, pc);
}

vc_hook_h hh;
// 监控整个 SO 的 .bss 段
vc_hook_add(ctx, &hh, VC_HOOK_MEM_READ_AFTER, (void*)mem_read_after,
            nullptr, bss_start, bss_end);
```

### identity mapping 直接读写

```cpp
// guest 地址 == host 地址，可以直接用 C 指针
uint64_t val = *(uint64_t*)0x764a233dc0;  // 读

*(uint64_t*)0x764a233dc0 = 0x42;          // 写数据页

// 写代码页需要改权限
uint64_t page = addr & ~0xFFFULL;
mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
*(uint32_t*)addr = 0xD503201F;  // NOP
mprotect((void*)page, 0x1000, PROT_READ | PROT_EXEC);
```

---

## 综合实战示例

### 示例 1：监控所有文件操作

```cpp
void file_monitor(vm_context* ctx, uint64_t addr,
                  const char* sym, vc_event_action* action, void* ud) {
    if (!sym) return;

    if (strcmp(sym, "open") == 0 || strcmp(sym, "openat") == 0) {
        uint64_t path_ptr;
        vc_reg_read(ctx, VC_REG_X0, &path_ptr);
        // identity mapping → 直接当 char* 用
        LOGD("[FILE] open(\"%s\")", (const char*)path_ptr);
    }
    else if (strcmp(sym, "read") == 0) {
        uint64_t fd, buf, count;
        vc_reg_read(ctx, VC_REG_X0, &fd);
        vc_reg_read(ctx, VC_REG_X2, &count);
        LOGD("[FILE] read(fd=%lu, count=%lu)", fd, count);
    }
    else if (strcmp(sym, "write") == 0) {
        uint64_t fd, buf, count;
        vc_reg_read(ctx, VC_REG_X0, &fd);
        vc_reg_read(ctx, VC_REG_X1, &buf);
        vc_reg_read(ctx, VC_REG_X2, &count);
        LOGD("[FILE] write(fd=%lu, \"%.*s\")", fd, (int)count, (char*)buf);
    }
}

vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)file_monitor, nullptr, 0, 0);
fn();
vc_free(ctx);
```

### 示例 2：全面绕过环境检测

```cpp
void anti_detect(vm_context* ctx, uint64_t addr,
                 const char* sym, vc_event_action* action, void* ud) {
    if (!sym) return;

    // 隐藏 root
    if (strcmp(sym, "access") == 0) {
        uint64_t path_ptr;
        vc_reg_read(ctx, VC_REG_X0, &path_ptr);
        const char* path = (const char*)path_ptr;
        if (strstr(path, "su") || strstr(path, "magisk") ||
            strstr(path, "superuser")) {
            uint64_t ret = (uint64_t)-1;
            vc_reg_write(ctx, VC_REG_X0, &ret);
            *action = VC_ACTION_SKIP;
            LOGD("[BYPASS] access(\"%s\") → -1", path);
        }
    }
    // 伪造 UID（非 root）
    else if (strcmp(sym, "getuid") == 0) {
        uint64_t ret = 10000;
        vc_reg_write(ctx, VC_REG_X0, &ret);
        *action = VC_ACTION_SKIP;
    }
    // 伪造系统属性
    else if (strcmp(sym, "__system_property_get") == 0) {
        uint64_t name_ptr;
        vc_reg_read(ctx, VC_REG_X0, &name_ptr);
        const char* name = (const char*)name_ptr;
        if (strstr(name, "ro.debuggable")) {
            uint64_t buf_ptr;
            vc_reg_read(ctx, VC_REG_X1, &buf_ptr);
            strcpy((char*)buf_ptr, "0");
            uint64_t ret = 1;
            vc_reg_write(ctx, VC_REG_X0, &ret);
            *action = VC_ACTION_SKIP;
            LOGD("[BYPASS] prop(\"%s\") → \"0\"", name);
        }
    }
    // 阻止自杀
    else if (strcmp(sym, "exit") == 0 || strcmp(sym, "abort") == 0 ||
             strcmp(sym, "killProcess") == 0) {
        *action = VC_ACTION_SKIP;
        LOGD("[BYPASS] blocked %s()", sym);
    }
}
```

### 示例 3：类 LLDB 交互式调试器

```cpp
struct Debugger {
    uint64_t breakpoints[16];
    int bp_count = 0;
    bool stepping = false;
};

void debugger_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    auto* dbg = (Debugger*)ud;
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);

    // 检查是否命中断点
    bool hit_bp = false;
    for (int i = 0; i < dbg->bp_count; i++) {
        if (pc == dbg->breakpoints[i]) { hit_bp = true; break; }
    }

    if (hit_bp || dbg->stepping) {
        // 反汇编当前指令
        vc_insn insn;
        vc_disasm(pc, 1, &insn);

        // 打印寄存器
        uint64_t x0, x1, x2, sp, lr;
        vc_reg_read(ctx, VC_REG_X0, &x0);
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_SP, &sp);
        vc_reg_read(ctx, VC_REG_LR, &lr);

        LOGD(">>> 0x%lx: %s %s", pc, insn.mnemonic, insn.op_str);
        LOGD("    X0=0x%lx X1=0x%lx X2=0x%lx SP=0x%lx LR=0x%lx",
             x0, x1, x2, sp, lr);

        if (dbg->stepping) {
            vc_single_step(ctx, 1);
        }
    }
}

// 使用
Debugger dbg;
dbg.breakpoints[0] = base + 0x1234;  // 断点 1
dbg.breakpoints[1] = base + 0x5678;  // 断点 2
dbg.bp_count = 2;
dbg.stepping = false;

vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)debugger_cb, &dbg, 0, 0);
fn();
vc_free(ctx);
```

### 示例 4：SVC 系统调用过滤

```cpp
void svc_filter(vm_context* ctx, uint64_t addr, uint32_t syscall_nr, void* ud) {
    switch (syscall_nr) {
        case __NR_openat: {
            uint64_t path_ptr;
            vc_reg_read(ctx, VC_REG_X1, &path_ptr);
            LOGD("[SVC] openat(\"%s\")", (const char*)path_ptr);
            break;
        }
        case __NR_connect: {
            uint64_t sockaddr_ptr;
            vc_reg_read(ctx, VC_REG_X1, &sockaddr_ptr);
            auto* sa = (struct sockaddr_in*)sockaddr_ptr;
            if (sa->sin_family == AF_INET) {
                LOGD("[SVC] connect(%s:%d)",
                     inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
            }
            break;
        }
        case __NR_kill:
        case __NR_tgkill: {
            uint64_t pid, sig;
            vc_reg_read(ctx, VC_REG_X0, &pid);
            vc_reg_read(ctx, VC_REG_X1, &sig);
            LOGD("[SVC] kill(pid=%lu, sig=%lu)", pid, sig);
            break;
        }
    }
}

vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_SVC, (void*)svc_filter, nullptr, 0, 0);
```

---

## 典型场景：拦截并伪造检测结果

```cpp
void* target_func = (void*)((uint64_t)base + 0x1234);

vm_context* ctx = nullptr;
auto fn = (void(*)(void*))vc_make_handle(target_func, &ctx);

// 用 lambda 拦截外部调用
auto intercept = [](vm_context* ctx, uint64_t addr,
                    const char* sym, vc_event_action* action, void* ud) {
    if (!sym) return;

    // 伪造 root 检测
    if (strstr(sym, "access")) {
        uint64_t ret = (uint64_t)-1;  // 文件不存在
        vc_reg_write(ctx, VC_REG_X0, &ret);
        *action = VC_ACTION_SKIP;
    }
    // 伪造 UID
    if (strstr(sym, "getuid")) {
        uint64_t ret = 10000;
        vc_reg_write(ctx, VC_REG_X0, &ret);
        *action = VC_ACTION_SKIP;
    }
    // 阻止自杀 / 强制退出
    if (strstr(sym, "killProcess") || strstr(sym, "exit")) {
        *action = VC_ACTION_SKIP;
    }
};

vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)(vc_cb_hookjump_t)intercept, nullptr, 0, 0);
fn(nullptr);
vc_free(ctx);
```

---

## 集成方式

### 方式一：APK 内集成

```
app/src/main/jniLibs/arm64-v8a/libtrace.so
```

```java
static { System.loadLibrary("trace"); }
```

### 方式二：Frida 注入

```js
const trace = Module.load("/data/local/tmp/libtrace.so");
```

### 头文件

```cpp
#include "ARM64Emulator.h"
```

## API 速查表

| API | 用途 |
|-----|------|
| `trace(func, path)` | 快速 trace，返回可调用句柄 |
| `trace(func, path, &ctx)` | 带 ctx 的 trace |
| `freeTrace(wrapper)` | 释放 trace 句柄 |
| `replace_trace(func, path)` | 全局替换式 trace |
| `restore_function(func)` | 恢复被替换的函数 |
| `vc_make_handle(func, &ctx)` | 创建裸 VM 句柄（不带 trace 输出）|
| `vc_free(ctx)` | 释放 VM 上下文 |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | 注册 hook |
| `vc_hook_del(ctx, hh)` | 删除 hook |
| `vc_reg_read(ctx, reg, &val)` | 读寄存器 |
| `vc_reg_write(ctx, reg, &val)` | 写寄存器 |
| `vc_reg_read_batch(ctx, regs, vals, n)` | 批量读 |
| `vc_reg_write_batch(ctx, regs, vals, n)` | 批量写 |
| `vc_emu_stop(ctx)` | 停止 VM |
| `vc_single_step(ctx, count)` | 执行 N 条后暂停 |
| `vc_set_until(ctx, addr)` | 设置临时断点 |
| `vc_disasm(addr, count, out)` | 反汇编 |
| `vc_context_save(ctx, &snap)` | 保存 CPU 快照 |
| `vc_context_restore(ctx, snap)` | 恢复 CPU 快照 |
| `vc_context_free(snap)` | 释放快照 |
| `vc_lookup_symbol(ctx, addr)` | 地址查符号 |
| `vc_set_jump_blacklist(names, ranges, n)` | 设置跳转黑名单 |
| `vc_clear_jump_blacklist()` | 清除黑名单 |
| `vc_set_external_jump_enabled(enabled)` | 全局跳转开关 |
| `trace_read(ctx, begin, end)` | 内存读监控 |
| `trace_write(ctx, begin, end)` | 内存写监控 |

## 性能参考

| 模式 | 速度 | 适用场景 |
|------|------|---------|
| vc_make_handle (默认 mask) | ~2-5x 慢 | 功能验证、外部调用监控 |
| trace() 指令级 | ~50-100x 慢 | 详细分析、逆向工程 |
| vc_make_handle + CODE | ~10-20x 慢 | 自定义逐指令监控 |
| vc_single_step(ctx, 1) | ~100-200x 慢 | 精确调试 |

## 文件清单

| 文件 | 说明 |
|------|------|
| `libtrace.so` | ARM64 共享库 (7.2MB) |
| `ARM64Emulator.h` | API 头文件（vm_context 为不透明类型）|
| `app-debug.apk` | 测试 APK |
| `UNICORN_TRACE_GUIDE.md` | 本文档 |

## 已知限制

- 仅 ARM64 (aarch64)，Android 10+
- 不能 trace Java 方法（只能 trace native）
- 不支持 VM 嵌套（handle 内再创建 handle 会死锁）
- `vc_single_step(ctx, 1)` 单步模式比正常执行慢（Unicorn icount 模式开销）
- 跳转控制只有 blacklist（强制跳出），没有 whitelist（强制留在 VM 内）
