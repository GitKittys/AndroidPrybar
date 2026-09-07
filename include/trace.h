#ifndef TRACE_H
#define TRACE_H

#include "vcpu.h"
// ============================================================
//  Trace 快捷 API
// ============================================================

void* trace(void* funcAddr, char* logPath, vm_context** ctx);
void* trace(void* funcAddr, char* logPath);
void freeTrace(uint64_t wrapper_func);

/*
 * vc_set_trace_crash_flush — trace 的可选项：拦崩溃/退出信号，强制把 trace 缓冲刷到盘
 *
 * 开启后，进程崩溃(SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE)、被终止(SIGTERM/SIGINT)、或正常 exit 前，
 * 自动把还在缓冲里没落盘的 trace（**文件模式** .lz4）强制刷出去——目标一崩就丢最后一段 trace 的
 * 问题没了，逆向"跑到哪崩的"特别有用。刷完链回原信号处理，不吞崩溃(tombstone 照出)。
 *
 * 幂等，装一次即可（建议 trace() 之前调）。TCP 模式本就实时流，不受此影响。
 */
void vc_set_trace_crash_flush(bool enable);

/*
 * trace_unidbg_dump — 运行时完整采样 dump出运行时需要的数据段，符号，jni地址表，帮助用dump法来补unidbg调试
 * dump法请参阅"白龙"的知识星球，为什么要用trace来采集？当然是方便可以随意输入参数来观察分支情况辅助调试。
 *@ funcAddr 要trace的地址
 *@ dumpDir 要保存的地址，会自动导出一下文件
 *
 * 用法（和 trace 一样：拿指针 → 调 → 释放）：
 *   auto h = (原函数签名)trace_unidbg_dump((void*)func, "/data/.../dump");
 *   h(args...);                          // 跑一遍；返回前已通过 emu_stop 回调把文件全落好盘
 *   trace_unidbg_dump_finish((void*)h);  // 只负责释放（和 freeTrace 一样），不再写文件
 * 多次 h(args...) 会增量累积：内部记「已 dump 页表」，已 dump 过的页下次跳过。
 *
 * 【支持多线程】wrapper 替换到原地址后被 app 多线程并发调用时，**每条线程各出一份完整 dump**，
 *   落到 dumpDir/tid{N}/（每线程一个子目录，互不干扰）。单线程调用也是 dumpDir/tid{N}/ 一个子目录。
 *
 * 每条线程的子目录 dumpDir/tid{N}/ 里是这些文件：
 *   <start>_<end>.bin  运行时访问的段内存
 *   regs.txt           入口时的寄存器值
 *   args.txt           入口时的参数
 *   symbols.log        这次调到的外部函数（地址+名字）
 *   jni_table.log      JNI 函数表（env->functions->GetVersion -> 0x71e6c2b000，这样，就可以很清楚知道哪些地址是jni地址）
 *   maps.log           /proc/self/maps
 *   info.txt           模块基址/偏移/SO 路径
 *   rootfs/            目标打开过的文件（按原路径放好，直接当 Unidbg 的 rootfs）
 *   files_manifest.txt rootfs 里都放了哪些文件（含跳过的记录）
 *
 * withTrace=true：同一次运行**额外**产出完整逐指令 trace（落到 dumpDir/trace/*.lz4）。这样你既有
 *   「中段执行快照」（dump 包），又有「这次真实运行的完整流水」——灌进 Unidbg 从入口重放时，一旦
 *   行为和 trace 对不上，就知道是哪一步、哪个环境没补对（类似有一份完整录像可回看）。代价：trace
 *   拖慢执行（逐指令），所以做成可选，默认关。用 trace_receiver.py decode dumpDir/trace/*.lz4 还原。
 *
 * trace 层实现，落盘由 VC_HOOK_EMU_STOP 回调驱动（内部用 vc_hook_add 注册，无需额外 VCPU 接口）。
 */
void* trace_unidbg_dump(void* funcAddr, const char* dumpDir, bool withTrace);
void* trace_unidbg_dump(void* funcAddr, const char* dumpDir);   // = withTrace=false（向后兼容）
void  trace_unidbg_dump_finish(void* wrapper_func);




/*
 封装了dobby，一键hook要trace的地方
 @target_func 要trace的地址，
 @logPath 要保存的地址/path/ 无需指定文件名字
 */
void* replace_trace(void* target_func, char* logPath);
void restore_function(void* target_func);

// ============================================================
//  内置内存监控 Hook（每种只能添加一次，重复调用会被忽略）
// ============================================================

/*
 * trace_read — 添加内存读监控 hook
 *
 * 在 [begin, end) 地址范围内的每次内存读取都会被记录。
 * 每个 vm_context 只能添加一次，重复调用无效。
 *
 * 输出优先级：trace 模式下写入 trace 文件 → 有 FILE* 时写 FILE* → 都没有则静默跳过。
 *
 * @param ctx    VM 上下文
 * @param begin  监控起始地址（0 = 不限）
 * @param end    监控结束地址（0 = 不限）
 * @param file   输出文件（裸 vc_make_handle 时使用，trace 模式下可传 NULL）
 */
void trace_read(vm_context* ctx, uint64_t begin, uint64_t end);
void trace_read(vm_context* ctx, uint64_t begin, uint64_t end, FILE* file);

/*
 * trace_write — 添加内存写监控 hook
 *
 * 参数和行为同 trace_read。
 */
void trace_write(vm_context* ctx, uint64_t begin, uint64_t end);
void trace_write(vm_context* ctx, uint64_t begin, uint64_t end, FILE* file);

// 带过滤器的内存监控重载：listener 返回 true 才输出（过滤器），false 抑制该条记录。
typedef bool (*traceWriteListener)(vm_context* ctx, uint64_t address, int size, long value);
typedef bool (*traceReadListener)(vm_context* ctx, uint64_t address, int size);
void trace_read(vm_context* ctx, uint64_t begin, uint64_t end, traceReadListener* onRead);
void trace_write(vm_context* ctx, uint64_t begin, uint64_t end, traceWriteListener* onWrite);

// hook 句柄（延迟/运行时 hook 注册用）。
typedef size_t TraceHook;
void stopTrace(vm_context* ctx, TraceHook* hook_session);

/*
 * add_hook — 底层 hook 注册（延迟初始化 + 运行时注册）
 *
 * 引擎起来前登记到 deferred_hooks（每个新 bundle 在 init() 中回放），
 * 引擎运行中则直接把原生 uc 回调装到当前 bundle。type 取 Unicorn 的
 * UC_HOOK_* 值，callback 为对应的原生 uc 回调。按 *ctx 指定的目标 vm 路由。
 */
void add_hook(vm_context** ctx, TraceHook* hh, int type, void* callback,
              void* user_data, uint64_t begin, uint64_t end, ...);


#endif // TRACE_H
