# VCPU —— 可编程 ARM64 虚拟 CPU（底层 API）

对应头文件 `include/vcpu.h`,实现在二进制资产 `libvcpu.a` / `libvcpu.so`(VCPU + Unicorn 引擎合并、符号隐藏)。
把任意 native 函数包成"VM 托管的可调用句柄",你在回调里读写寄存器、下断、单步、拍快照、反汇编,完全掌控它的执行。

> 开箱即用的 `trace()`、`trace_unidbg_dump()` 等都是拿这层 API 包出来的成品,用法见 **[TRACE.md](TRACE.md)**。

---

## vc_make_handle — 底层 VCPU（`trace()` 就是基于它包的）

这是整个框架的核心：把目标函数变成一个"VM 托管的可调用句柄"，你注册 hook、读写寄存器、单步执行，
完全掌控它的运行。上面所有 `trace*` 包装都是在它之上加了默认 hook 而已。

```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(int, int))vc_make_handle((void*)target_func, &ctx);

// 注册 hook
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);

int result = fn(1, 2);  // 在 VM 中执行
vc_free(ctx); //不调用也行，释放资源而已
```


## 不满意自带的 trace？直接用底层 Hook 自己包

这层 Hook 是对 Unicorn hook 的薄封装(类型换成 `vm_context*` + `vc_*`,不必碰任何引擎头);`trace()`、`trace_unidbg_dump()` 都是拿它拼的。套路一句话:**`vc_make_handle` 拿句柄 → `vc_hook_add` 挂回调 → 回调里写你自己的逻辑 → 调用**。记录、改寄存器、篡改返回值、下断、单步、按地址过滤,随你,不用动我们的代码。

**完整流程**(创建 → 挂回调 → 调用 → 释放):

```cpp
#include "trace.h"

// ① 创建 VCPU 句柄：把目标函数包成一个同签名、可调用的指针
vm_context* ctx = nullptr;
auto fn = (int(*)(int))vc_make_handle((void*)target_func, &ctx);

// ② 往这个句柄上挂回调（可挂多个、不同类型；begin/end 传 0 表示全程生效，
//    传地址段则只在该范围触发。回调实现见下面）
vc_hook_h h1, h2;
vc_hook_add(ctx, &h1, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);
vc_hook_add(ctx, &h2, VC_HOOK_MEM_WRITE,     (void*)my_mem_cb,  nullptr, 0, 0);

// ③ 调用 → 目标函数在 VCPU 里跑，期间触发上面注册的回调
int r = fn(123);

// ④ 释放（不调也行，仅回收资源）
vc_free(ctx);
```

上面第 ② 步挂的回调，按 hook 类型各有对应签名，实现长这样：

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

### Hook 类型速查，详细的签名请看头文件


| 类型                     | 回调签名           | 用途                   |
| ------------------------ | ------------------ | ---------------------- |
| `VC_HOOK_BLOCK`          | `vc_cb_hookcode_t` | 每个基本块入口         |
| `VC_HOOK_CODE`           | `vc_cb_hookcode_t` | 每条指令执行触发一次。 |
| `VC_HOOK_INTR`           | `vc_cb_hookintr_t` | 中断（含 SVC）         |
| `VC_HOOK_MEM_READ`       | `vc_cb_hookmem_t`  | 内存读                 |
| `VC_HOOK_MEM_WRITE`      | `vc_cb_hookmem_t`  | 内存写                 |
| `VC_HOOK_MEM_READ_AFTER` | `vc_cb_hookmem_t`  | 内存读后（有值）       |
| `VC_HOOK_SVC`            | `vc_cb_hooksvc_t`  | SVC 系统调用           |
| `VC_HOOK_EXTERNAL_JUMP`  | `vc_cb_hookjump_t` | 外部函数调用           |

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


| SO 类型           | 默认行为    |
| ----------------- | ----------- |
| 目标 SO           | VM 内执行   |
| 其他用户 SO       | VM 内执行   |
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

## 实战示例：运行时修改寄存器 / 返回值

VCPU 的典型用法是**在函数执行途中读改它的 CPU 状态**——篡改返回值、改入参、强行改走某条分支。
因为整个函数在我们引擎里跑，任意时刻的寄存器都可读可写。（注：单纯"绕检测"用 inline hook 往往更直接，
这里展示的是 VCPU 独有的"运行时精确改状态"能力。）

**① 改某个被调用函数的返回值** —— 拦外部调用 → 写 X0 → `SKIP` 跳过真实调用

```cpp
void patch_ret(vm_context* ctx, uint64_t addr, const char* sym,
               vc_event_action* action, void* ud) {
    if (sym && strcmp(sym, "check_license") == 0) {
        uint64_t ok = 1;
        vc_reg_write(ctx, VC_REG_X0, &ok);   // 让 check_license() 返回 1
        *action = VC_ACTION_SKIP;            // 不真正执行它，直接用我们写的返回值
    }
}
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)patch_ret, nullptr, 0, 0);
```

**② 在指定指令处篡改寄存器，强改分支走向** —— BLOCK/CODE hook 限定地址段

```cpp
// 在 base+0x1240（某个 cmp 之前）把 x0 强设为 0，让后面的 cbnz 不跳转
void force_branch(vm_context* ctx, uint64_t pc, uint32_t size, void* ud) {
    uint64_t zero = 0;
    vc_reg_write(ctx, VC_REG_X0, &zero);     // 直接改运行中的寄存器
}
uint64_t at = base + 0x1240;
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)force_branch, nullptr, at, at + 4);
```

**③ 调用前篡改入参** —— 改传给某函数的参数寄存器，但仍真实调用

```cpp
void patch_arg(vm_context* ctx, uint64_t addr, const char* sym,
               vc_event_action* action, void* ud) {
    if (sym && strcmp(sym, "memcmp") == 0) {
        uint64_t n = 0;
        vc_reg_write(ctx, VC_REG_X2, &n);    // 把 memcmp 的长度 x2 改成 0 → 恒返回相等
        // 不设 SKIP：仍真实执行 memcmp，只是用我们改过的参数
    }
}
```

组装照常：`vc_make_handle` 拿句柄 → `vc_hook_add` 注册上面的回调 → 调用 → `vc_free`。

---

