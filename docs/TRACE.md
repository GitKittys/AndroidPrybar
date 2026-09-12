# trace —— VCPU 之上的开箱即用工具

对应头文件 `include/trace.h`(顶部已 `#include "vcpu.h"`),成品在预编译 `libtrace.so`。
这些都是在底层 VCPU API 之上包出来的:一键逐指令 trace、Unidbg「中段执行」dump、替换式 trace。

> **底层 API(`vc_make_handle` / 各类 hook / 寄存器读写 / 单步断点 / 快照 / 反汇编)见 [VCPU.md](VCPU.md)。**
> 觉得自带 trace 不够用,照 VCPU.md 的 Hook 自己包即可,不必改本仓库代码。

---

## trace() — 最简路径，一键出日志,一般用这个就够了,第二个参数是指定一个路径,不要指定名字,它支持多线程调用的,你用的时候,trace这个包装函数会返回一个同等功能的函数指针,你直接用inlinehook或者无痕hook等手段替换到原来地址,等app自己调用,或者你来传参调用都可以

```cpp
#include "trace.h"

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

## 自动追踪被 trace 函数内部创建的线程（可选，默认关）

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

## 崩溃 / 退出前保住 trace（可选）

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
## replace_trace() — inline hook 式 trace（一般用不到）

```cpp
replace_trace((void*)func_addr, "/data/data/pkg/trace_dir");
// 之后所有对 func_addr 的调用都自动走 VM trace
restore_function((void*)func_addr);  // 恢复原函数
```

## trace_unidbg_dump() — 一键导出 Unidbg「中段执行」dump 包

配合 dump 法补 Unidbg 调试：真机上把目标函数放进 VM 真实跑一遍、运行时完整采样，**跑完自动**把
Unidbg 从该函数「中段」跑起来所需的全部要素落盘成一个 dump 目录（内存段、寄存器、参数、符号、
JNI 表、maps、目标读过的文件）。省去 Frida+dd+adb pull 反复回填那套。用法和 `trace()` 一样：拿指针 → 调 → 释放。

```cpp
#include "trace.h"

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


| 文件                 | 内容                                                                                            |
| -------------------- | ----------------------------------------------------------------------------------------------- |
| `<start>_<end>.bin`  | 运行时访问到的内存段（按段落盘，灌进 Unidbg）                                                   |
| `regs.txt`           | 入口时的寄存器（X0-X30/SP/PC/NZCV/FPCR/FPSR/Q0-Q31）                                            |
| `args.txt`           | 入口参数（X0-X7 + SP）                                                                          |
| `symbols.log`        | 这次调用路径用到的外部函数（地址 + 名字）                                                       |
| `jni_table.log`      | JNI 函数表（`env->functions->FindClass -> 0xXXXX`，用来认出 symbols.log 里哪些地址是 JNI 调用） |
| `maps.log`           | `/proc/self/maps`                                                                               |
| `info.txt`           | 模块基址 / 目标偏移 / SO 路径                                                                   |
| `rootfs/`            | 目标运行时打开过的文件，按原路径镜像成目录树，直接当 Unidbg 的 rootfs 用                        |
| `files_manifest.txt` | rootfs 里放了哪些文件（含跳过记录）                                                             |

> 把 dump 目录 `adb pull` 回来：`.bin` 按 `info.txt` 的基址/偏移在 Unidbg 里映射，`rootfs/` 指给
> Unidbg 的 rootfs，目标读的配置/数据/密钥文件就位，即可从该函数入口在 Unidbg 里跑起来。

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

