# AndroidPrybar

ARM64 **函数级 VCPU（可编程虚拟 CPU）** + 指令跟踪框架。把任意 native 函数放进 Unicorn 引擎里执行，你能像调试器一样完全掌控它：逐指令 / 基本块 / 内存 / SVC / 外部调用 hook、读写寄存器、单步、断点、CPU 快照、反汇编。

**重点：**`trace()` **只是我们在这套 VCPU API 之上包出来的开箱即用工具之一，不是全部。** 不想写代码 → 直接 `trace()` 一键出日志；想精细控制 → 直接拿底层 VCPU（`vc_make_handle` + 各类 hook + 寄存器读写 + 单步/断点/快照）自己驱动。`trace()` / `trace_unidbg_dump()` / `replace_trace()` 本质都是这层 VCPU 的封装——比如 `trace()` 就是「`vc_make_handle` + 注册逐指令 hook + 把每条指令格式化写盘」。
支持多进程，多线程，多并发，多个函数同时追踪，测试抖音，美团，等大型app均无崩溃情况。  

本工程已包含预编译的 `libtrace.so`、头文件和 JNI 示例。克隆后直接编译运行即可查看结果。

## 更新记录

- 新增 `vc_set_trace_crash_flush()`：app 崩溃/终止/exit 前强制把 trace 缓冲刷到磁盘，不丢崩溃点之前那段
- `trace_receiver.py` 解压改用 lz4 C 引擎（`pip install lz4`），decode 大幅加速；未安装则自动回退纯 Python
- 新增 `trace_unidbg_dump()`：一键导出 Unidbg「中段执行」dump 包（内存段/寄存器/参数/符号/JNI 表/maps/rootfs）
- README 增加 Frida 调用 `trace()` 的示例
- 修复了部分 bug

## `libtrace.so` 在哪里

预编译动态库：`app/src/main/jniLibs/arm64-v8a/libtrace.so`

对外头文件：`app/src/main/cpp/include/ARM64Emulator.h`

---

## 快速上手

> **两层用法，按需选：**
>
> - **开箱即用的包装**（不写逻辑、一键出结果）：`trace()`、`trace_unidbg_dump()`、`replace_trace()`。
> - **底层 VCPU**（自己写回调、掌控执行）：`vc_make_handle()` + `vc_hook_add()` + 寄存器读写 + 单步 / 断点 / CPU 快照 / 反汇编。
>
> 上面的包装**全部基于**下面的 VCPU API 实现。所以你既能当"一键 trace 工具"用，也能当"可编程 ARM64 VCPU"用——同一套东西。

### trace() — 最简路径，一键出日志,一般用这个就够了,第二个参数是指定一个路径,不要指定名字,它支持多线程调用的,你用的时候,trace这个包装函数会返回一个同等功能的函数指针,你直接用inlinehook或者无痕hook等手段替换到原来地址,等app自己调用,或者你来传参调用都可以

```cpp
#include "ARM64Emulator.h"

// 本地文件输出（LZ4 压缩，per-thread 自动分 .lz4 文件）
auto fn = (int(*)(int))trace((void*)target_func, "/data/data/pkg/trace_dir");
fn(123);                       // 调用 → 自动写 trace 日志
freeTrace((uint64_t)fn);       // 用完释放，也可以不用释放，没必要
// 输出文件在 trace_dir/ 下，用 trace_receiver.py decode *.lz4 还原为文本

// TCP 远程输出（LZ4 压缩，实时传到 PC，适合超大 trace）,在使用的时候adb forward该端口即可
auto fn2 = (int(*)(int))trace((void*)target_func, "tcp:9876");
// ↑ 阻塞等待 PC 连接，PC 端运行: python trace_receiver.py receive，这个的具体使用写在最末尾
fn2(123);
freeTrace((uint64_t)fn2); //不调用也行，释放资源而已
```

#### Frida 调用示例（两参版）

不写 C++、直接用 Frida 调 `libtrace.so` 导出的 `trace(func, path)`，包装目标函数后替换到原地址，等 app 自己调用即产出 trace：

```javascript
// 目标 SO 已加载后运行。libtrace.so 需先 load 进目标进程（如放到 app 目录再 dlopen / Module.load）
const trace = new NativeFunction(
    Module.getExportByName("libtrace.so", "_Z5tracePvPc"), // trace(void*, char*) 的 mangled 名
    'pointer', ['pointer', 'pointer']);

const target = Module.findBaseAddress("libtarget.so").add(0x1234); // 目标函数偏移
const path   = Memory.allocUtf8String("/data/data/com.xxx/trace_dir"); // 目录需存在且可写

const wrapper = trace(target, path);      // 返回同签名的包装函数指针
Interceptor.replace(target, wrapper);     // 替换到原地址，之后 app 调用自动走 VM trace
```

### 自动追踪被 trace 函数内部创建的线程（可选，默认关）

开启后：被 trace 的函数内部 `pthread_create` 创建的线程，其入口函数自动也进 VM 执行、产生独立 trace，
不用手动为每个线程函数单独 `trace()`。**默认关**，用 3 参拿 `ctx` 后 `vc_set_auto_trace_threads(ctx, true)` 开。

```cpp
// 3 参拿 ctx，开启自动线程追踪（默认是关的）
vm_context* ctx = nullptr;
auto fn = (int(*)(int))trace((void*)target_func, "/data/data/pkg/trace_dir", &ctx);
vc_set_auto_trace_threads(ctx, true);    // 开启
fn(123);                                 // 内部 pthread_create 的线程自动被追踪
```

**怎么做到的**：在 `pthread_create` 处下一个地址锁定的 block hook，把它的 `start_routine`（x2）换成
`trace()` 包装过的版本 —— 新线程一启动就在 VM 里跑、自动出 trace。地址锁定 = 只在命中该点才进回调，
其它块零开销。

- **递归成线程树**：子线程内部再 `pthread_create` 开的孙线程，一样被自动追踪，整棵线程树都覆盖。
- **文件模式**：子线程文件名带父函数，`父+0xOFF__sub__子+0xOFF_tidN_0.lz4`，`ls` 一眼看出谁开的线程、同父排一起。
- **TCP 模式**：子线程复用同一连接，接收端按 tid 分文件（无父前缀，帧只带 tid）。
- **只追这些**：入口落在**目标 SO 内**、且经 `pthread_create` 新建的线程。**不追**：trace() 之前就已存在的线程、入口在 libc/ART 等系统库的线程（不会把系统内部线程全拖进 VM）。

### 崩溃 / 退出前保住 trace（可选）

> **Q：用 trace 找检测点，但 app 一检测到环境异常就崩，trace 不完整怎么办？**
> **A：**`vc_set_trace_crash_flush(true)` —— 崩溃前自动把 trace 刷到盘，崩溃点之前的全保住。

逆向常遇到"目标跑着跑着崩了"，默认崩溃时缓冲里没落盘的那段 trace 就丢了、正好看不到"崩在哪"。
开这个开关：进程**崩溃**（SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE）、被**终止**（SIGTERM/SIGINT）、
或正常 **exit** 前，自动把还在缓冲里的 trace 抢救到盘（补最后一行 → 刷缓冲 → 关文件），刷完再走原来的
崩溃流程（tombstone 照出，不吞崩溃）。

```cpp
vc_set_trace_crash_flush(true);   // 装一次即可，建议 trace(),这里会安装信号处理器，让trace日志强制刷新到磁盘。
auto fn = (int(*)(int))trace((void*)target_func, "/data/data/pkg/trace_dir");
fn(123);                          // 就算 fn 里崩了，崩溃点之前的 trace 也已落盘
```

- 仅**文件模式**（`.lz4`）；TCP 模式本就实时流、不受影响。
- 信号处理器里只做「刷 + 关」，**不做释放**（崩溃时 malloc/锁可能被占，释放会死锁），尽力而为抢数据。

### vc_make_handle — 底层 VCPU（`trace()` 就是基于它包的）

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

### replace_trace() — inline hook 式 trace（一般用不到）

```cpp
replace_trace((void*)func_addr, "/data/data/pkg/trace_dir");
// 之后所有对 func_addr 的调用都自动走 VM trace
restore_function((void*)func_addr);  // 恢复原函数
```

### trace_unidbg_dump() — 一键导出 Unidbg「中段执行」dump 包

配合 dump 法补 Unidbg 调试：真机上把目标函数放进 VM 真实跑一遍、运行时完整采样，**跑完自动**把
Unidbg 从该函数「中段」跑起来所需的全部要素落盘成一个 dump 目录（内存段、寄存器、参数、符号、
JNI 表、maps、目标读过的文件）。省去 Frida+dd+adb pull 反复回填那套。用法和 `trace()` 一样：拿指针 → 调 → 释放。

```cpp
#include "ARM64Emulator.h"

// 1. 包装目标函数，指定 dump 输出目录（返回同签名的可调用指针）
auto h = (int(*)(JNIEnv*, jbyteArray))trace_unidbg_dump(
             (void*)target_func, "/data/data/spkg/unidbg_dump");

// 2. 正常调用（app 自己调 / inline hook 替换到原地址 / 你手动传参都行）
h(env, input);                        // 跑一遍；执行完毕会被监听到，返回时文件已【自动落好盘】

// 3. 这个可以忽略，释放资源而已，调不调用无所谓，s这步只回收 VM 句柄（和 freeTrace 一样）；
//    不调也行、只是漏掉一份一次性句柄，不影响 dump 结果。
trace_unidbg_dump_finish((void*)h);
```

> 落盘是靠「执行完毕」监听（目标函数一返回就自动落盘），不依赖 finish。finish 只负责释放，
> 且不能自动做——释放必须在调用返回之后，不能在还在 VM 里跑的时候拆自己。

多次调用 `h(...)`（换不同参数走不同分支）会**增量累积**到同一目录：已 dump 过的内存页下次跳过，
适合多输入多跑把覆盖补全。

**可选：同时出完整指令 trace**（第 3 参 `withTrace=true`）

```cpp
auto h = (原签名)trace_unidbg_dump((void*)func, "/data/.../dump", true);
h(args...);   // 同一次运行：dump 落 dump/ 下，完整逐指令 trace 落 dump/trace/*.lz4
```

这样你既有「中段执行快照」（dump 包），又有「这次真实运行的完整流水」。灌进 Unidbg 从入口重放时，
一旦行为和 trace 对不上，直接看 trace 就知道是哪一步、哪个环境没补对——相当于有份完整录像可回看。
代价：全程逐指令 trace 会**慢很多**（trace 本就 ~50-100x），所以默认关。trace 用
`trace_receiver.py decode dump/trace/*.lz4` 还原。

**支持多线程**：wrapper 替换到原地址后被 app 多线程并发调用时，**每条线程各出一份完整 dump**，
落到 `dumpDir/tid{N}/`（每线程一个子目录，互不干扰；单线程调用也是一个 `tid{N}/` 子目录）。喂 Unidbg
时选你要分析的那条线程的 `tid{N}/` 即可。

**每个** `dumpDir/tid{N}/` **里导出这些文件（直接喂给 Unidbg）：**


| 文件                   | 内容                                                                            |
| -------------------- | ----------------------------------------------------------------------------- |
| `<start>_<end>.bin`  | 运行时访问到的内存段（按段落盘，灌进 Unidbg）                                                    |
| `regs.txt`           | 入口时的寄存器（X0-X30/SP/PC/NZCV/FPCR/FPSR/Q0-Q31）                                   |
| `args.txt`           | 入口参数（X0-X7 + SP）                                                              |
| `symbols.log`        | 这次调用路径用到的外部函数（地址 + 名字）                                                        |
| `jni_table.log`      | JNI 函数表（`env->functions->FindClass -> 0xXXXX`，用来认出 symbols.log 里哪些地址是 JNI 调用） |
| `maps.log`           | `/proc/self/maps`                                                             |
| `info.txt`           | 模块基址 / 目标偏移 / SO 路径                                                           |
| `rootfs/`            | 目标运行时打开过的文件，按原路径镜像成目录树，直接当 Unidbg 的 rootfs 用                                  |
| `files_manifest.txt` | rootfs 里放了哪些文件（含跳过记录）                                                         |


> 把 dump 目录 `adb pull` 回来：`.bin` 按 `info.txt` 的基址/偏移在 Unidbg 里映射，`rootfs/` 指给
> Unidbg 的 rootfs，目标读的配置/数据/密钥文件就位，即可从该函数入口在 Unidbg 里跑起来。

---

## 不满意自带的 trace？直接用底层 Hook 自己包

前面那些 `trace*` 都是成品；**真正的乐高积木是这一层 Hook**——它就是对 Unicorn hook API 的一层薄封装
（只是把类型换成 `vm_context*` + `vc_*`，你无需引入任何引擎头文件）。

**上面所有开箱即用的 trace，全是拿这些 Hook 拼出来的**，套路完全一样：

> `vc_make_handle` 拿句柄 → `vc_hook_add` 注册你要的 hook → **回调里写你自己的逻辑** → 调用函数。

- `trace()` 本质 = `vc_make_handle` + 一个 `VC_HOOK_CODE`（逐指令）回调，回调里把「反汇编 + 寄存器 + 访存」格式化写盘；
- `trace_unidbg_dump()` = `vc_make_handle` + `VC_HOOK_BLOCK` / `MEM` / `SVC` / `EMU_STOP` 几个 hook 采样落盘。

所以自带的不够用时**不用改我们的代码**——直接注册下面这些 Hook，在回调里干你想干的：记录、改寄存器、
篡改返回值、下断、单步、按地址过滤……随你。

**完整流程**（创建句柄 → 加回调 → 调用 → 释放）：

```cpp
#include "ARM64Emulator.h"

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


| 类型                       | 回调签名               | 用途          |
| ------------------------ | ------------------ | ----------- |
| `VC_HOOK_BLOCK`          | `vc_cb_hookcode_t` | 每个基本块入口     |
| `VC_HOOK_CODE`           | `vc_cb_hookcode_t` | 每条指令执行触发一次。 |
| `VC_HOOK_INTR`           | `vc_cb_hookintr_t` | 中断（含 SVC）   |
| `VC_HOOK_MEM_READ`       | `vc_cb_hookmem_t`  | 内存读         |
| `VC_HOOK_MEM_WRITE`      | `vc_cb_hookmem_t`  | 内存写         |
| `VC_HOOK_MEM_READ_AFTER` | `vc_cb_hookmem_t`  | 内存读后（有值）    |
| `VC_HOOK_SVC`            | `vc_cb_hooksvc_t`  | SVC 系统调用    |
| `VC_HOOK_EXTERNAL_JUMP`  | `vc_cb_hookjump_t` | 外部函数调用      |


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

## trace 格式摘要

`.lz4` 用 `trace_receiver.py decode` 还原后每行一条指令。看几行真实例子就懂有哪些格式：

```text
# 普通指令：so+偏移 PC: 指令  读寄存器 => 写回寄存器(新值)
libtest.so+0x4e40 0x76981c4e40: add x0, x1, #0x10  x1=0x20 => x0=0x30

# 访存 mem_r/mem_w[地址 归属]=值 —— 归属直接标出是哪块内存：
libz.so+0xa1d4 ...: ldr x14, [x10] => x14=0x726f..  mem_r[0x707167a760 libtest.so+0x1a760:code] str:"o UnicornTrace VM!"   # SO段+偏移(对IDA)，地址是串就打出来
libssl.so+0x3b0 ...: ldr x0, [x8]                    mem_r[0x72aa5d3000 libssl.so+0x1a300:ro AES_Td+0x40]                  # 命中导出符号
libtest.so+0x9a70 ...: stp x29,x30,[sp,#-0x30]! => sp=0x70..  mem_w[0x70719f6ff0 [anon:stack_and_tls:9059]]=0x70..        # 栈
libfoo.so+0x1200 ...: str w0, [x9]                   mem_w[0x71b0640010 [anon:scudo:primary]]=0x1                          # 堆(scudo)
libfoo.so+0x1300 ...: ldr w0, [x9]                   mem_r[0x72d0aa1000 [anon:.bss]]                                       # BSS

# 外部调用 / JNI / 系统调用 / 返回值
libtest.so+0x..: bl  ...   [CALL] memcpy(dst=0x.., src=0x.., n=0x40)
libtest.so+0x..: blr x8    >>> JNIEnv->FindClass("java/lang/String") => 0x1
libtest.so+0x..: svc #0    [SVC #56] openat(AT_FDCWD, "/data/.../config", O_RDONLY)
libtest.so+0x..: ret       x0=0x0
```

- 归属：命名 SO 标 `so+0x偏移:ro/code/rw`(+ 导出符号)，直接对 IDA；匿名标 `[stack]`/`[anon:scudo:*]`(堆)/`[anon:.bss]`(BSS)/`[anon:stack_and_tls:TID]` —— **栈 / 堆 / BSS 一眼分清**。
- 其它：SIMD `qN=0x<32hex>`、标志 `nzcv: N=.,Z=.,C=.,V=.`、`str:"…"`(地址是串) / `->"…"`(值是串指针)。
- 多线程每线程一个 `.lz4`(名字带父函数)；TCP 模式按 tid 自动分文件。

---

## API 速查表


| API                                               | 用途                                                     |
| ------------------------------------------------- | ------------------------------------------------------ |
| `trace(func, path)`                               | 快速 trace（path 为目录 → 本地 .lz4，`"tcp:PORT"` → 远程）         |
| `trace(func, path, &ctx)`                         | 带 ctx 的 trace                                          |
| `freeTrace(wrapper)`                              | 释放 trace 句柄                                            |
| `replace_trace(func, path)`                       | 全局替换式 trace                                            |
| `restore_function(func)`                          | 恢复被替换的函数                                               |
| `trace_unidbg_dump(func, dumpDir)`                | 导出 Unidbg 中段执行 dump 包（跑完自动落盘，返回可调用指针）                  |
| `trace_unidbg_dump_finish(wrapper)`               | 释放 dump 句柄（文件已自动落盘，仅回收资源）                              |
| `vc_make_handle(func, &ctx)`                      | 创建裸 VM 句柄                                              |
| `vc_free(ctx)`                                    | 释放 VM 上下文                                              |
| `vc_hook_add(ctx, &hh, type, cb, ud, begin, end)` | 注册 hook                                                |
| `vc_hook_del(ctx, hh)`                            | 删除 hook                                                |
| `vc_reg_read / vc_reg_write`                      | 寄存器读写                                                  |
| `vc_reg_read_batch / vc_reg_write_batch`          | 批量读写                                                   |
| `vc_emu_stop(ctx)`                                | 停止 VM                                                  |
| `vc_single_step(ctx, count)`                      | 执行 N 条后暂停                                              |
| `vc_set_until(ctx, addr)`                         | 设置临时断点                                                 |
| `vc_disasm(addr, count, out)`                     | 反汇编                                                    |
| `vc_context_save / restore / free`                | CPU 快照                                                 |
| `vc_lookup_symbol(ctx, addr)`                     | 地址查符号                                                  |
| `vc_set_jump_blacklist(names, ranges, n)`         | 设置跳转黑名单                                                |
| `vc_clear_jump_blacklist()`                       | 清除黑名单                                                  |
| `vc_set_external_jump_enabled(enabled)`           | 全局跳转开关                                                 |
| `vc_set_auto_trace_threads(ctx, enable)`          | 自动追踪被 trace 函数内部创建的线程（**默认关**；3 参拿 ctx 传 true 开启） |
| `vc_set_trace_crash_flush(enable)`                | 崩溃/终止/exit 前自动把 trace 缓冲刷到盘（文件模式），不丢崩溃前那段              |
| `trace_read / trace_write`                        | 内存监控                                                   |


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

运行后在 `filesDir` 下生成 LZ4 日志文件，界面输出中显示完整路径。用 `python tools/trace_receiver.py decode *.lz4` 还原为纯文本。

## 项目结构

```text
AndroidPrybar/
|-- TraceDemo/                       ← Android 接入示例工程
|   `-- app/src/main/
|       |-- cpp/include/ARM64Emulator.h  ← API 头文件
|       |-- cpp/native-lib.cpp           ← Demo 示例代码
|       `-- jniLibs/arm64-v8a/libtrace.so ← 预编译库
|-- tools/
|   |-- trace_receiver.py            ← TCP 接收 + LZ4 解码工具
|   `-- build_calltree.py            ← trace → 函数调用树/调用图
|-- .claude/skills/unicorn-trace/    ← 附带的 Claude Code 用法 skill
`-- README.md
```

## 附带工具

### `tools/trace_receiver.py` — LZ4 解码 + TCP 接收

trace 输出统一为 LZ4 压缩格式（`.lz4` 文件），压缩比约 7-8x。本工具负责解码和 TCP 接收。

> **提速：**装依赖后解压走 C 引擎，decode 大文件快很多；不装也能用（自动回退纯 Python 实现，只是慢）。
>
> ```bash
> pip install lz4                        # 或
> pip install -r tools/requirements.txt
> ```

#### 解码本地 .lz4 文件

手机上 trace 产生的文件是 `.lz4` 格式，先 `adb pull` 到 PC，再用 `decode` 还原为可读文本：

```bash
# 从手机拉取 trace 目录
adb pull /data/data/com.xxx/trace_dir/ .

# 解码
python tools/trace_receiver.py decode trace.lz4              # → trace.log
python tools/trace_receiver.py decode *.lz4                  # 批量解码所有 .lz4
python tools/trace_receiver.py decode trace.lz4 --stdout     # 输出到 stdout（可 pipe 给 grep 等）
python tools/trace_receiver.py decode trace.lz4 -o out.log   # 指定输出文件名
```

#### TCP 远程接收（适合超大 trace）

trace 量很大时（几百 MB ~ 几 GB），不适合写手机存储，用 TCP 实时传到 PC：

```bash
# 1. PC 端先启动接收（自动 adb forward，等待设备连接）
python tools/trace_receiver.py receive

# 2. App 中调用 trace(func, "tcp:9876") → 设备阻塞等待 PC 连接
#    连接建立后 trace 数据实时流式传输到 PC
#    freeTrace() 后自动结束

# 自定义选项
python tools/trace_receiver.py receive -p 12345 -o my.log    # 自定义端口和输出前缀
python tools/trace_receiver.py receive --no-adb --host 192.168.1.x  # WiFi 直连（不走 USB）
python tools/trace_receiver.py receive --stdout              # 输出到 stdout
```

#### 多线程输出

trace 支持多线程并发调用，每个线程的 trace 数据带有 tid 标识：

- **本地文件模式**：每个线程自动生成独立文件，如 `func_tid1234_0.lz4`、`func_tid1234_1.lz4`（同一线程多次调用自动递增序号）
- **TCP 模式**：所有线程数据通过同一 socket 传输，接收端自动按 tid 分文件，如 `trace_tid1234_0.log`、`trace_tid5678_0.log`。同一 tid 多次接收时序号自动递增，不会覆盖已有文件
- **decode 多线程 .lz4**：如果一个 `.lz4` 文件中包含多个 tid 的数据，decode 会自动拆分为 `_tid{N}.log`

接收/解码时终端会实时显示各线程数据量：

```
[*] frames=42  total=3.2MB  ratio=7.6x  speed=12.5MB/s  [t1234:2.1M t5678:1.1M]
```

### `tools/build_calltree.py` — trace → 函数调用树

从 trace 重建函数调用树/调用图（谁调了谁、各函数调用次数）。脚本侧建树，部分/被打断的 trace 也能建；新引擎的多线程输出（每线程一文件的目录）传目录即自动每线程一棵树。

```bash
python tools/build_calltree.py <trace文件或目录> [so名] [入口偏移hex] [最大行数]
# 输出: <base>_calltree.txt(调用树) + <base>_callgraph.txt(调用图/计数)
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