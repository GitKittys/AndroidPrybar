# AndroidPrybar

作者:发财哥

ARM64 函数级 VM 执行与指令跟踪框架。把任意 native 函数放进 Unicorn 引擎中执行，暴露每条指令、外部调用、SVC、内存访问的全部细节。

本工程已包含预编译的 `libtrace.so`、头文件和 JNI 示例。克隆后直接编译运行即可查看结果。
支持多线程trace，有时候样本会在多个线程调用同一个函数，我里面会自动命名文件tid_index.log

## `libtrace.so` 在哪里

预编译动态库：`app/src/main/jniLibs/arm64-v8a/libtrace.so`

对外头文件：`app/src/main/cpp/include/ARM64Emulator.h`

---

## 快速上手

### trace() — 最简路径，一行出日志,一般用这个就够了,第二个参数是指定一个路径,不要指定名字,它支持多线程调用的,你用的时候,trace这个包装函数会返回一个同等功能的函数指针,你直接用inlinehook或者无痕hook等手段替换到原来地址,等app自己调用,或者你来传参调用都可以

```cpp
#include "ARM64Emulator.h"

auto fn = (int(*)(int))trace((void*)target_func, "/data/data/pkg/");
fn(123);                       // 调用 → 自动写 trace 日志
freeTrace((uint64_t)fn);       // 用完释放空间,其实也没必要,毕竟总不能有几百个函数要被trace吧
```

### vc_make_handle — 带回调的 VM 执行,没有封装任何trace,使用者可以自己封装自己的trace日志

```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(int, int))vc_make_handle((void*)target_func, &ctx);

// 注册 hook
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);

int result = fn(1, 2);  // 在 VM 中执行
vc_free(ctx);
```

### replace_trace() — 这个是我用bobyhook简单的封装了一下trace()而已,是inlinehook.

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


| 类型                       | 回调签名               | 用途           |
| ------------------------ | ------------------ | ------------ |
| `VC_HOOK_BLOCK`          | `vc_cb_hookcode_t` | 每个基本块入口      |
| `VC_HOOK_CODE`           | `vc_cb_hookcode_t` | 每条指令（慢 ~10x） |
| `VC_HOOK_INTR`           | `vc_cb_hookintr_t` | 中断（含 SVC）    |
| `VC_HOOK_MEM_READ`       | `vc_cb_hookmem_t`  | 内存读          |
| `VC_HOOK_MEM_WRITE`      | `vc_cb_hookmem_t`  | 内存写          |
| `VC_HOOK_MEM_READ_AFTER` | `vc_cb_hookmem_t`  | 内存读后（有值）     |
| `VC_HOOK_SVC`            | `vc_cb_hooksvc_t`  | SVC 系统调用     |
| `VC_HOOK_EXTERNAL_JUMP`  | `vc_cb_hookjump_t` | 外部函数调用       |


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


| SO 类型       | 默认行为     |
| ----------- | -------- |
| 目标 SO       | VM 内执行   |
| 其他用户 SO     | VM 内执行   |
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

每行一条指令，格式：

```
模块名+0x模块内偏移 0x绝对PC: 助记符 操作数 读寄存器=值 => 写寄存器=值  mem_r[地址] / mem_w[地址]=值
```

### 每个字段说明

| 字段 | 含义 | 示例 |
|------|------|------|
| `模块名+0xOFFSET` | SO 名 + 模块内偏移，直接对应 IDA 地址 | `libtarget.so+0x35a6e0` |
| `0xPC` | 绝对虚拟地址 | `0x727a35a6e0` |
| `助记符 操作数` | 反汇编结果 | `stp x29, x30, [sp, #-0x30]!` |
| `寄存器=值` | 指令**读取**的寄存器当前值（只显示本条指令用到的） | `x29=0x764a233e00 x30=0x727a290130` |
| `=> 寄存器=值` | 指令**写入**的寄存器新值 | `=> w8=0x7` |
| `mem_r[地址]` | 内存读地址 | `mem_r[0x764a233d8c]` |
| `mem_w[地址]=值` | 内存写地址和写入的值 | `mem_w[0x764a233dc0]=0x42` |
| `str:"..."` | 访问的地址本身是 C 字符串，完整输出内容 | `str:"ro.build.fingerprint"` |
| `->"..."` | 加载的值是一个字符串指针，输出它指向的字符串 | `->"/data/data/com.example/config.json"` |

### 特殊标签行

除了普通指令行之外，trace 中还会插入以下特殊标签行：

| 标签 | 触发条件 | 输出内容 | 正则匹配 |
|------|---------|---------|---------|
| `[CALL]` | VM 跳转到外部函数时 | 函数名 + 参数（已知签名显示参数名，未知显示前 4 个 hex） | `\[CALL\]\s+(\w+)\(` |
| `>>> JNIEnv->` | JNI 函数被调用时 | JNI 方法名 + 参数 + 返回值 | `>>>\s+JNIEnv->` |

### 示例片段

```
libtarget.so+0x35a6e0 0x727a35a6e0: stp x29, x30, [sp, #-0x30]! x29=0x764a233e00 x30=0x727a290130 sp=0x764a233df0
libtarget.so+0x35a710 0x727a35a710: blr x8 x8=0x70c04759f0
  [CALL] getuid()
libtarget.so+0x35a714 0x727a35a714: cmp w0, #0x15f8f => nzcv=0x60000000
    >>> JNIEnv->FindClass("com/example/MyClass") => 0x1 @ [libtarget.so]0x26f46c
libtarget.so+0x86fe0 0x727a286fe0: ldr x1, [x8] x8=0x764a233da0 => x1=0x727a2f0100  mem_r[0x764a233da0] ->"/data/data/com.example/files/config.json"
libtarget.so+0x86fe8 0x727a286fe8: ldr x2, [sp, #0x30] sp=0x764a233d70  mem_r[0x764a233da0] str:"ro.build.fingerprint"
```

### 字符串自动检测规则

- 仅 `ldr x`/`str x` 等 **>= 8 字节**的内存访问触发，`ldrb`/`ldrh` 不触发（避免循环拷贝噪音）
- **str:"..."** — 访问的地址本身是一个 C 字符串（至少前 4 字节是可打印字符）
- **->"..."** — 访问的地址不是字符串，但加载出来的 8 字节值是一个有效的字符串指针

---

## API 速查表


| API                                               | 用途            |
| ------------------------------------------------- | ------------- |
| `trace(func, path)`                               | 快速 trace      |
| `trace(func, path, &ctx)`                         | 带 ctx 的 trace |
| `freeTrace(wrapper)`                              | 释放 trace 句柄   |
| `replace_trace(func, path)`                       | 全局替换式 trace   |
| `restore_function(func)`                          | 恢复被替换的函数      |
| `vc_make_handle(func, &ctx)`                      | 创建裸 VM 句柄     |
| `vc_free(ctx)`                                    | 释放 VM 上下文     |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | 注册 hook       |
| `vc_hook_del(ctx, hh)`                            | 删除 hook       |
| `vc_reg_read / vc_reg_write`                      | 寄存器读写         |
| `vc_reg_read_batch / vc_reg_write_batch`          | 批量读写          |
| `vc_emu_stop(ctx)`                                | 停止 VM         |
| `vc_single_step(ctx, count)`                      | 执行 N 条后暂停     |
| `vc_set_until(ctx, addr)`                         | 设置临时断点        |
| `vc_disasm(addr, count, out)`                     | 反汇编           |
| `vc_context_save / restore / free`                | CPU 快照        |
| `vc_lookup_symbol(ctx, addr)`                     | 地址查符号         |
| `vc_set_jump_blacklist(names, ranges, n)`         | 设置跳转黑名单       |
| `vc_clear_jump_blacklist()`                       | 清除黑名单         |
| `vc_set_external_jump_enabled(enabled)`           | 全局跳转开关        |
| `trace_read / trace_write`                        | 内存监控          |


## 性能参考


| 模式                     | 速度          | 适用场景        |
| ---------------------- | ----------- | ----------- |
| vc_make_handle（默认）     | ~2-5x 慢     | 功能验证、外部调用监控 |
| trace() 指令级            | ~50-100x 慢  | 详细分析、逆向工程   |
| vc_make_handle + CODE  | ~10-20x 慢   | 自定义逐指令监控    |
| vc_single_step(ctx, 1) | ~100-200x 慢 | 精确调试        |


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
TraceDemo/
|-- app/
|   |-- src/main/cpp/
|   |   |-- include/
|   |   |   `-- ARM64Emulator.h      ← API 头文件
|   |   |-- CMakeLists.txt
|   |   `-- native-lib.cpp           ← Demo 示例代码
|   |-- src/main/jniLibs/arm64-v8a/
|   |   `-- libtrace.so              ← 预编译库
|   `-- src/main/java/.../MainActivity.java
`-- README.md
```

## 交流群 / 联系方式

欢迎扫码进群，一起学习和交流 Android Native / Trace / VM 相关内容。

交流群二维码

## 个人的碎碎念念

这个工具我断断续续写了两年。当时几乎没有好用且公开的 trace 工具，也无人分享开发此类工具的方法。我一点点摸索、测试、推倒重来，仅为了编写一个支持自动传参的 JIT，就思考测试了许久。

同时，我知道在 AI 时代，即使不开放源码，通过 IDA 反汇编也可以很快复刻出工具并学习其中的思路。但这并不重要。

不求署名，不求分润，不求你在任何地方提及我的名字。我只求你在按下"复制"或"生成"之前，在心里，对这段熬了两年的代码，说一声谢谢。不需要我这份刚燃起开源的心冷却下去，不需要写在任何文档里。只要你自己知道，这东西不是凭空从石头里蹦出来的，它是某个人在无数个深夜里，一行一行从混沌里捞出来的，这就够了。