# unicornTrace 完整指南

> libtrace.so — ARM64 函数级 VM 执行与指令跟踪库

---

## 快速上手

### trace() — 一键全量 trace

```cpp
#include "ARM64Emulator.h"

// 最简：两个参数
auto fn = (bool(*)())trace((void*)func_addr, (char*)"/data/data/pkg/trace_dir");
fn();
freeTrace((uint64_t)fn);

// 带 ctx：可注册 hook
vm_context* ctx = nullptr;
auto fn = (bool(*)())trace((void*)func_addr, (char*)"/data/data/pkg/trace_dir", &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_cb, nullptr, 0, 0);
fn();
freeTrace((uint64_t)fn);
```

**签名：**
```cpp
void* trace(void* funcAddr, char* logPath);
void* trace(void* funcAddr, char* logPath, vm_context** ctx);
void  freeTrace(uint64_t wrapper_func);
```

**logPath 是输出目录**，不能指定文件名。trace 在目录内自动命名（前缀 `soName+0xOFFSET`），多线程时每线程一个 `.lz4` 文件。有扩展名会被去掉（`.../trace.txt` → 目录 `.../trace/`）。

**trace 深度：**
- 深入目标 SO 内所有子函数（BL/BLR 到 SO 内 → 继续模拟）
- 同进程其他用户 SO 也会拉进 VM（默认行为，可用跳转黑名单控制）
- 系统库（libc、libart 等）不 trace — 跳出到 host 执行后返回
- JNI 调用自动跟踪（有 ART 环境时）

### replace_trace() — 全局替换式 trace

所有对该函数的调用自动走 VM trace，不用改调用方：
```cpp
replace_trace((void*)func_addr, (char*)"/data/data/pkg/trace_dir");
// app 正常运行，所有调 func_addr 的地方自动被记录
restore_function((void*)func_addr);  // 恢复原函数
```

内部实现：`trace()` 拿包装指针 → Dobby inline hook 把 target 重定向到它。你也可以自己做 inline hook（Frida Interceptor / 影子页 / 手写跳转补丁）。

---

## Trace 输出

### LZ4 压缩格式

所有 trace 输出为 LZ4 帧格式（`.lz4` 文件），帧结构：`[comp_size:4][orig_size:4][tid:4][compressed_data]`。
用 `trace_receiver.py` 还原为纯文本。

### 两种输出目标

**本地文件（默认）：**
```cpp
trace(func, (char*)"/data/data/pkg/trace_dir");
```
```bash
adb pull /data/data/pkg/trace_dir/ .
python trace_receiver.py decode *.lz4        # 每个 .lz4 → 对应 .log
python trace_receiver.py decode trace.lz4 --stdout  # 输出到 stdout
```

**TCP 远程（大 trace / 不落设备盘）：**
```cpp
trace(func, (char*)"tcp:9876");   // 或 "tcp:" 用默认端口
```
```bash
# PC 端先启动接收（自动 adb forward，等待连接）
python trace_receiver.py receive
python trace_receiver.py receive -p 12345 -o my.log   # 自定义端口和输出
python trace_receiver.py receive --no-adb --host 192.168.1.x  # WiFi 直连
```
流程：PC 端 `receive` → app 调 `trace("tcp:9876")` 阻塞等连接 → 连接后实时流 → `freeTrace()` 发结束帧 → receiver 退出。

引擎侧：20MB mmap 缓冲，TCP 用双 buffer + 后台线程 LZ4 压缩发送（trace 线程不阻塞 I/O）。

### 多线程输出
被 trace 的函数被多线程并发调用时，每线程拿 master 的 per-thread 克隆、各写自己的文件（thread_local 隔离、无锁）。
- 本地：每线程独立 `.lz4` 文件，同线程多次调用自动递增序号（`_0.lz4`, `_1.lz4`）
- TCP：所有线程帧混在同一 socket，接收端按 tid 自动分文件

### trace 文本格式

每行一条指令：
```
[HH:MM:SS.mmm][so名 0xOFFSET] [机器码] 地址: "助记符 操作数" 读寄存器 => 写寄存器  mem_r/w  ->"字符串"
```

字段说明：
| 部分 | 含义 |
|------|------|
| `so+0xOFFSET` | 模块名 + 模块内偏移（对应 IDA 地址）|
| `0xPC` | 运行时绝对虚拟地址 |
| 寄存器 | 只显示被指令使用/修改的寄存器及其值 |
| `=> reg=val` | 指令写回的寄存器新值 |
| `mem_r[addr]` / `mem_w[addr]=val` | 内存读写地址和值 |
| `str:"..."` | 访问地址本身是 C 字符串 |
| `->"..."` | 加载的值是字符串指针，显示指向的字符串 |

特殊标签（可用正则匹配）：
- `[CALL] func(args)` — 外部函数调用（`\[CALL\]\s+(\w+)\(`）
- `>>> JNIEnv->Method() => result` — JNI 调用（`>>>\s+JNIEnv->`）
- `[SVC #N] syscall_name(args)` — 系统调用
- `[MEM READ/WRITE @ addr, size=N]` — 内存访问 hexdump（开启 MEM 事件时）

示例：
```
[14:23:01.456][libtarget.so 0x35a6e0] [a9ba7bfd] 0x727a35a6e0: "stp x29, x30, [sp, #-0x30]!" x29=0x764a233e00 x30=0x727a290130 sp=0x764a233df0
[14:23:01.456][libtarget.so 0x35a710] [d63f0100] 0x727a35a710: "blr x8" x8=0x70c04759f0
  [CALL] getuid()
[14:23:01.456][libtarget.so 0x86fe0] [f9400101] 0x727a286fe0: "ldr x1, [x8]" x8=0x764a233da0 => x1=0x727a2f0100  mem_r[0x764a233da0] ->"/data/data/com.example.app/files/config.json"
    >>> JNIEnv->FindClass("com/example/MyClass") => 0x1 @ [libtarget.so]0x26f46c
```

---

## vc_make_handle — 裸 VM 句柄 + 自定义 hook

不带 trace 输出，通过 hook 回调自己监控/干预执行：

```cpp
vm_context* ctx = nullptr;
auto fn = (int(*)(int, int))vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);
int result = fn(1, 2);
vc_free(ctx);
```

---

## Hook 类型与回调签名

### EXTERNAL_JUMP — 拦截外部函数调用（最常用）

```cpp
void my_jump_cb(vm_context* ctx, uint64_t address,
                const char* symbol_name, vc_event_action* action, void* ud) {
    LOGD("call: %s @ 0x%lx", symbol_name, address);
    if (symbol_name && strcmp(symbol_name, "getuid") == 0) {
        uint64_t fake_uid = 0;
        vc_reg_write(ctx, VC_REG_X0, &fake_uid);
        *action = VC_ACTION_SKIP;
    }
}
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump_cb, nullptr, 0, 0);
```

### SVC — 系统调用

```cpp
void my_svc_cb(vm_context* ctx, uint64_t address, uint32_t syscall_nr, void* ud) {
    LOGD("SVC #%u @ 0x%lx", syscall_nr, address);
}
vc_hook_add(ctx, &hh, VC_HOOK_SVC, (void*)my_svc_cb, nullptr, 0, 0);
```

### BLOCK / CODE — 基本块 / 逐指令

```cpp
void my_block_cb(vm_context* ctx, uint64_t address, uint32_t size, void* ud) {
    // 每个基本块入口触发；size = 块大小（字节）
}
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)my_block_cb, nullptr, 0, 0);
// VC_HOOK_CODE 同签名，逐指令触发（慢 ~10x），按需开启
```

### MEM — 内存读写

```cpp
void my_mem_cb(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    // type: VC_MEM_READ(16) / VC_MEM_WRITE(17) / VC_MEM_READ_AFTER(25)
    // WRITE 时 value = 写入值；READ_AFTER 时 value = 读到的值
}
vc_hook_add(ctx, &hh, VC_HOOK_MEM_WRITE, (void*)my_mem_cb, nullptr, base, base + 0x1000);
// VC_HOOK_MEM_ALL = MEM_READ | MEM_WRITE
```

### INTR — 中断

```cpp
void my_intr_cb(vm_context* ctx, uint32_t intno, void* ud) {
    // intno==2 为 SVC（但一般直接用 VC_HOOK_SVC 更方便）
}
vc_hook_add(ctx, &hh, VC_HOOK_INTR, (void*)my_intr_cb, nullptr, 0, 0);
```

### 注册与删除

```cpp
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_cb, nullptr, 0, 0);
// begin/end 限地址范围（0, 0 = 全范围），可只 hook 某段代码
vc_hook_del(ctx, hh);  // 不再需要时删除
```

---

## 寄存器读写

```cpp
// 单个读写
uint64_t pc, x0;
vc_reg_read(ctx, VC_REG_PC, &pc);
vc_reg_read(ctx, VC_REG_X0, &x0);

uint64_t fake = 42;
vc_reg_write(ctx, VC_REG_X0, &fake);

// 批量
uint64_t x0_val, x1_val, sp_val;
vc_reg regs[] = { VC_REG_X0, VC_REG_X1, VC_REG_SP };
void*  vals[] = { &x0_val, &x1_val, &sp_val };
vc_reg_read_batch(ctx, regs, vals, 3);

// SIMD/FP（128 位）
__uint128_t q0;
vc_reg_read(ctx, VC_REG_Q0, &q0);

// 64 位浮点
uint64_t d0;
vc_reg_read(ctx, VC_REG_D0, &d0);

// 条件标志
uint64_t nzcv;
vc_reg_read(ctx, VC_REG_NZCV, &nzcv);
```

寄存器号：`VC_REG_X0..X30`、`SP/PC/NZCV`、`Q0..Q31`、`D0..D15`、`FPCR/FPSR`；别名 `VC_REG_FP`(=X29) / `VC_REG_LR`(=X30)。X0-X30 连续，可 `(vc_reg)(VC_REG_X0+n)` 遍历。CODE 回调内自动用 `uc_reg_read_fast`。

---

## 跳转控制

### 默认行为

| SO 类型 | 行为 |
|---------|------|
| 目标 SO（函数所在） | VM 内执行 |
| 其他用户 SO | VM 内执行 |
| 系统库（libc/libart/linker） | 跳出到 host |

### 黑名单 — 强制指定 SO 跳出

```cpp
const char* bl[] = { "libcrypto.so", "libssl.so", nullptr };
vc_set_jump_blacklist(bl, nullptr, 0);

// 地址范围
uint64_t ranges[][2] = { { base, base + 0x200000 } };
vc_set_jump_blacklist(nullptr, ranges, 1);

// 混合
vc_set_jump_blacklist(bl, ranges, 1);

// 清除
vc_clear_jump_blacklist();
```

### 全局开关

```cpp
vc_set_external_jump_enabled(false);  // 只跑目标 SO，其他全跳出
// true（默认）= 用户 SO 都留在 VM，配合 blacklist 排除不想要的
```

---

## VM 控制

### 停止执行

```cpp
vc_emu_stop(ctx);  // 在回调中调用，当前 TB 执行完后停止
```

### 单步执行

```cpp
// 在回调中调用，回调返回后执行 N 条指令再触发回调
vc_single_step(ctx, 1);   // 单步
vc_single_step(ctx, 100); // 跑 100 条再停
// 不调用则恢复正常执行
```

### 临时断点

```cpp
vc_set_until(ctx, addr);  // 执行到 addr 停（到达后自动清除）
vc_set_until(ctx, 0);     // 手动清除
```

### CPU 快照

```cpp
vc_cpu_context* snap = nullptr;
vc_context_save(ctx, &snap);     // 存当前全部寄存器
// ... 执行/改寄存器 ...
vc_context_restore(ctx, snap);   // 恢复到快照点（PC 也回去）
vc_context_free(snap);           // 传 NULL 安全
```

---

## 反汇编

```cpp
vc_insn insns[10];
int n = vc_disasm(addr, 10, insns);
for (int i = 0; i < n; i++)
    printf("0x%lx: %s %s\n", insns[i].address, insns[i].mnemonic, insns[i].op_str);
```

`vc_insn`：`.address` `.size`(=4) `.bytes`(机器码) `.mnemonic[32]` `.op_str[160]`。
不需要 vm_context（identity mapping，addr 就是 host 指针）。

```cpp
const char* sym = vc_lookup_symbol(ctx, addr);  // 地址→符号名（可能 NULL）
```

---

## 内存监控

### 内置 trace_read / trace_write

```cpp
trace_read(ctx, begin, end);   // 监控 [begin, end) 范围的读
trace_write(ctx, begin, end);  // 监控写（0,0 = 全范围）
// 每个 ctx 每种只能加一次，重复忽略
// 裸 vc_make_handle 时用带 FILE* 的重载：trace_read(ctx, begin, end, fp);
```

### 自定义 MEM hook

```cpp
void mem_watch(vm_context* ctx, vc_mem_type type,
               uint64_t address, int size, int64_t value, void* ud) {
    uint64_t pc;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    LOGD("[0x%lx] written by PC=0x%lx, val=0x%lx", address, pc, value);
}
vc_hook_add(ctx, &hh, VC_HOOK_MEM_WRITE, (void*)mem_watch, nullptr, watch_addr, watch_addr + 8);
```

### identity mapping 直接读写

guest 地址 == host 地址，可直接用 C 指针：
```cpp
uint64_t val = *(uint64_t*)0x764a233dc0;      // 读
*(uint64_t*)0x764a233dc0 = 0x42;              // 写数据页
// 写代码页需要先 mprotect
```

---

## 实战示例

### 1. 监控所有文件操作

```cpp
void file_monitor(vm_context* ctx, uint64_t addr,
                  const char* sym, vc_event_action* action, void* ud) {
    if (!sym) return;
    if (strcmp(sym, "open") == 0 || strcmp(sym, "openat") == 0) {
        uint64_t path_ptr;
        vc_reg_read(ctx, VC_REG_X0, &path_ptr);
        LOGD("[FILE] open(\"%s\")", (const char*)path_ptr);
    }
    else if (strcmp(sym, "read") == 0) {
        uint64_t fd, count;
        vc_reg_read(ctx, VC_REG_X0, &fd);
        vc_reg_read(ctx, VC_REG_X2, &count);
        LOGD("[FILE] read(fd=%lu, count=%lu)", fd, count);
    }
}
vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)file_monitor, nullptr, 0, 0);
```

### 2. 绕过环境检测

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
    else if (strcmp(sym, "__system_property_get") == 0) {
        uint64_t name_ptr;
        vc_reg_read(ctx, VC_REG_X0, &name_ptr);
        if (strstr((const char*)name_ptr, "ro.debuggable")) {
            uint64_t buf_ptr;
            vc_reg_read(ctx, VC_REG_X1, &buf_ptr);
            strcpy((char*)buf_ptr, "0");
            uint64_t ret = 1;
            vc_reg_write(ctx, VC_REG_X0, &ret);
            *action = VC_ACTION_SKIP;
        }
    }
    else if (strcmp(sym, "exit") == 0 || strcmp(sym, "abort") == 0) {
        *action = VC_ACTION_SKIP;
    }
}
```

### 3. 单步调试器

```cpp
void step_cb(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
    uint64_t pc, x0, x1, sp, lr;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    vc_reg_read(ctx, VC_REG_X0, &x0);
    vc_reg_read(ctx, VC_REG_X1, &x1);
    vc_reg_read(ctx, VC_REG_SP, &sp);
    vc_reg_read(ctx, VC_REG_LR, &lr);

    vc_insn insn;
    vc_disasm(pc, 1, &insn);
    LOGD("0x%lx: %s %s  X0=%lx X1=%lx SP=%lx LR=%lx",
         pc, insn.mnemonic, insn.op_str, x0, x1, sp, lr);

    vc_single_step(ctx, 1);
}

vm_context* ctx = nullptr;
auto fn = (int(*)())vc_make_handle((void*)target, &ctx);
vc_hook_h hh;
vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)step_cb, nullptr, 0, 0);
vc_single_step(ctx, 1);
fn();
vc_free(ctx);
```

### 4. SVC 系统调用过滤

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
            uint64_t sa_ptr;
            vc_reg_read(ctx, VC_REG_X1, &sa_ptr);
            auto* sa = (struct sockaddr_in*)sa_ptr;
            if (sa->sin_family == AF_INET)
                LOGD("[SVC] connect(%s:%d)", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
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
vc_hook_add(ctx, &hh, VC_HOOK_SVC, (void*)svc_filter, nullptr, 0, 0);
```

---

## 集成方式

### APK 内集成
```
app/src/main/jniLibs/arm64-v8a/libtrace.so
```
```java
static { System.loadLibrary("trace"); }
```
```cpp
#include "ARM64Emulator.h"
```

### Frida 注入
```js
Module.load('/data/local/tmp/libtrace.so');
var trace = new NativeFunction(
    Module.findExportByName('libtrace.so', '_Z5tracePvPcPP10vm_context'),
    'pointer', ['pointer', 'pointer', 'pointer']);

var target  = Module.findBaseAddress('libfoo.so').add(0x9abc);
var ctxPP   = Memory.alloc(Process.pointerSize);
var wrapper = trace(target, Memory.allocUtf8String('/data/local/tmp/foo_trace'), ctxPP);

// 方式 1：自己调
var fn = new NativeFunction(wrapper, 'int', ['int']);
fn(42);

// 方式 2：全局替换（app 调 target 时自动进 VM）
Interceptor.replace(target, wrapper);
```

### C 语言用法
API 就是 C ABI，纯 C 声明原型即可调：
```c
extern void* trace(void* f, char* logDir, struct vm_context** ctx);
extern void  freeTrace(unsigned long long wrapper);
extern void* vc_make_handle(void* f, struct vm_context** ctx);
extern void  vc_free(struct vm_context*);
extern int   vc_hook_add(struct vm_context*, size_t* hh, int type, void* cb, void* ud,
                         unsigned long long begin, unsigned long long end);
```

---

## 性能参考

| 模式 | 速度 | 适用场景 |
|------|------|---------|
| `vc_make_handle`（默认 mask） | ~2-5x 慢 | 功能验证、外部调用监控 |
| `trace()`（指令级） | ~50-100x 慢 | 详细分析、逆向工程 |
| `vc_make_handle` + CODE | ~10-20x 慢 | 自定义逐指令监控 |
| `vc_single_step(ctx, 1)` | ~100-200x 慢 | 精确调试 |

## 已知限制

- 仅 ARM64 (aarch64)，Android 10+
- 只能 trace native，不能 trace Java 方法
- 不支持 VM 嵌套（handle 内再 `vc_make_handle` 会死锁）
- 跳转控制只有 blacklist，没有 whitelist
- `vc_single_step(ctx, 1)` 单步模式比正常执行慢（icount 模式开销）
- 崩点前数据可能丢尾（LZ4 20MB 缓冲区未 flush），实质函数崩时用 debug 模式编译（`-DTRACE_DEBUG`）缩小缓冲
