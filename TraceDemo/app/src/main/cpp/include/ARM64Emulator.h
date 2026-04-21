//
// Created by ASUS on 2023-07-14.
//

#ifndef UNICRONTEST_ARM64EMULATOR_H
#define UNICRONTEST_ARM64EMULATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
/**
本文件由OpenAi生成
*/

// ============================================================
//  vc_err — API 返回的错误码
// ============================================================
typedef enum {
    VC_ERR_OK       = 0,  // 操作成功
    VC_ERR_ARG      = 1,  // 传入了无效的参数（如 NULL 指针、越界寄存器编号等）
    VC_ERR_HANDLE   = 2,  // vm_context 无效（未绑定到 VM 或已被 vc_free 释放）
    VC_ERR_NOMEM    = 3,  // 内存分配失败
    VC_ERR_INTERNAL = 4,  // 内部引擎出错（通常不会遇到）
} vc_err;

// ============================================================
//  vc_reg — ARM64 寄存器编号
//
//  用于 vc_reg_read / vc_reg_write 等函数的第二个参数，
//  代替 Unicorn 的 UC_ARM64_REG_* 枚举，用户无需引入任何引擎头文件。
//
//  X0-X30 连续编号，所以可以用 (vc_reg)(VC_REG_X0 + n) 做循环遍历。
// ============================================================
typedef enum {
    // --- 通用寄存器 X0-X30（64 位）---
    VC_REG_X0 = 0,  VC_REG_X1,  VC_REG_X2,  VC_REG_X3,
    VC_REG_X4,       VC_REG_X5,  VC_REG_X6,  VC_REG_X7,
    VC_REG_X8,       VC_REG_X9,  VC_REG_X10, VC_REG_X11,
    VC_REG_X12,      VC_REG_X13, VC_REG_X14, VC_REG_X15,
    VC_REG_X16,      VC_REG_X17, VC_REG_X18, VC_REG_X19,
    VC_REG_X20,      VC_REG_X21, VC_REG_X22, VC_REG_X23,
    VC_REG_X24,      VC_REG_X25, VC_REG_X26, VC_REG_X27,
    VC_REG_X28,      VC_REG_X29, VC_REG_X30,

    // --- 特殊寄存器 ---
    VC_REG_SP,        // 栈指针 (Stack Pointer)
    VC_REG_PC,        // 程序计数器 (Program Counter)
    VC_REG_NZCV,      // 条件标志寄存器 (N/Z/C/V flags)

    // --- 常用别名 ---
    VC_REG_FP = VC_REG_X29,   // 帧指针，等价于 X29
    VC_REG_LR = VC_REG_X30,   // 链接寄存器，等价于 X30

    // --- 128 位 SIMD/FP 寄存器 Q0-Q31 ---
    VC_REG_Q0,  VC_REG_Q1,  VC_REG_Q2,  VC_REG_Q3,
    VC_REG_Q4,  VC_REG_Q5,  VC_REG_Q6,  VC_REG_Q7,
    VC_REG_Q8,  VC_REG_Q9,  VC_REG_Q10, VC_REG_Q11,
    VC_REG_Q12, VC_REG_Q13, VC_REG_Q14, VC_REG_Q15,
    VC_REG_Q16, VC_REG_Q17, VC_REG_Q18, VC_REG_Q19,
    VC_REG_Q20, VC_REG_Q21, VC_REG_Q22, VC_REG_Q23,
    VC_REG_Q24, VC_REG_Q25, VC_REG_Q26, VC_REG_Q27,
    VC_REG_Q28, VC_REG_Q29, VC_REG_Q30, VC_REG_Q31,

    // --- 64 位浮点寄存器 D0-D15 ---
    VC_REG_D0,  VC_REG_D1,  VC_REG_D2,  VC_REG_D3,
    VC_REG_D4,  VC_REG_D5,  VC_REG_D6,  VC_REG_D7,
    VC_REG_D8,  VC_REG_D9,  VC_REG_D10, VC_REG_D11,
    VC_REG_D12, VC_REG_D13, VC_REG_D14, VC_REG_D15,

    // --- 浮点控制/状态 ---
    VC_REG_FPCR,     // 浮点控制寄存器
    VC_REG_FPSR,     // 浮点状态寄存器

    _VC_REG_COUNT    // （内部使用）枚举总数
} vc_reg;

// ============================================================
//  事件系统
// ============================================================

struct vm_context;

/*
 * vc_cpu_event_type — 回调中收到的事件类型
 *
 * 通过 vc_make_handle() 注册回调后，VM 执行过程中会在不同时机
 * 触发对应类型的事件，用户可以在回调中读写寄存器、修改内存、
 * 甚至决定是否跳过某次外部函数调用。
 */
typedef enum {
    VC_CPU_EVENT_CODE  = 0,  // 每执行一条指令时触发（需要手动开启，默认关闭，开启后有性能开销）
    VC_CPU_EVENT_BLOCK = 1,  // 每进入一个基本块（一段连续指令）时触发
    VC_CPU_EVENT_INTR  = 2,  // 发生中断时触发（不含 SVC）
    VC_CPU_EVENT_SVC   = 3,  // 执行 SVC（系统调用）时触发，event->syscall_nr 为调用号
    VC_CPU_EVENT_MEM_READ  = 4,  // 读取内存时触发（需要手动开启，默认关闭，开启后有性能开销）
    VC_CPU_EVENT_MEM_WRITE = 5,  // 写入内存时触发（需要手动开启，默认关闭，开启后有性能开销）
    VC_CPU_EVENT_EXTERNAL_JUMP = 6,  // VM 即将跳转到目标 SO 外部的函数时触发
                                     // 用户可以在回调中设置 event->action = VC_ACTION_SKIP 来阻止跳转
} vc_cpu_event_type;

/*
 * vc_event_action — 用户在回调中设置的动作
 *
 * 目前仅对 VC_CPU_EVENT_EXTERNAL_JUMP 有效。
 * 默认值为 VC_ACTION_CONTINUE，即正常跳出 VM 执行外部函数。
 *
 * 使用场景示例：某个外部函数不想让它真正执行，可以在回调中：
 *   1. 用 vc_reg_write(ctx, VC_REG_X0, &fake_ret) 设置伪造的返回值
 *   2. 设置 event->action = VC_ACTION_SKIP
 *   这样 VM 会跳过外部调用，直接用你设置的返回值继续执行。
 */
typedef enum {
    VC_ACTION_CONTINUE = 0,  // 正常执行（默认）
    VC_ACTION_SKIP     = 1,  // 跳过这次外部函数调用，VM 直接从 LR 地址继续执行
} vc_event_action;

/*
 * vc_event_mask — 控制哪些事件类型会触发回调
 *
 * 通过 vc_set_event_mask() 设置，用按位或（|）组合你需要的事件。
 * 默认值为 VC_EVENT_DEFAULT_MASK，包含 BLOCK + INTR + SVC + EXTERNAL_JUMP。
 *
 * CODE 和 MEM 默认关闭，因为它们会在每条指令 / 每次内存访问时触发回调，
 * 对性能影响很大，只在需要精细监控时才开启。
 *
 * 示例:
 *   // 在默认基础上额外开启逐指令监控
 *   vc_set_event_mask(ctx, ctx->eventMask | VC_EVENT_ENABLE_CODE);
 *
 *   // 只监听外部跳转，其他全关
 *   vc_set_event_mask(ctx, VC_EVENT_ENABLE_EXTERNAL_JUMP);
 *
 *   // 关闭 BLOCK 事件
 *   vc_set_event_mask(ctx, ctx->eventMask & ~VC_EVENT_ENABLE_BLOCK);
 */
typedef enum {
    VC_EVENT_ENABLE_CODE  = 1 << 0,  // 逐指令回调（默认关，性能开销大）
    VC_EVENT_ENABLE_BLOCK = 1 << 1,  // 基本块回调（默认开）
    VC_EVENT_ENABLE_INTR  = 1 << 2,  // 中断回调（默认开）
    VC_EVENT_ENABLE_SVC   = 1 << 3,  // SVC/系统调用回调（默认开）
    VC_EVENT_ENABLE_MEM   = 1 << 4,  // 内存读写回调（默认关，性能开销大）
    VC_EVENT_ENABLE_EXTERNAL_JUMP = 1 << 5, // 外部跳转回调（默认开）
} vc_event_mask;

// 默认开启的事件掩码
#define VC_EVENT_DEFAULT_MASK \
    (VC_EVENT_ENABLE_BLOCK | VC_EVENT_ENABLE_INTR | \
     VC_EVENT_ENABLE_SVC | VC_EVENT_ENABLE_EXTERNAL_JUMP)

/*
 * vc_cpu_event_info — 回调收到的事件详情
 *
 * 不同事件类型下，各字段含义如下：
 *
 *   CODE:           address=当前PC, size=指令字节数, insn_bytes=指令编码
 *   BLOCK:          address=块起始PC, size=块大小(字节)
 *   INTR:           address=当前PC, intno=中断号
 *   SVC:            address=当前PC, intno=中断号, syscall_nr=系统调用号(X8)
 *   MEM_READ:       address=读取的内存地址, size=读取字节数, mem_value=读到的值
 *   MEM_WRITE:      address=写入的内存地址, size=写入字节数, mem_value=写入的值
 *   EXTERNAL_JUMP:  address=跳转目标地址, symbol_name=目标函数名(可能为NULL)
 *                   可设置 action=VC_ACTION_SKIP 来阻止跳转
 */
typedef struct {
    vc_cpu_event_type type;       // 事件类型
    uint64_t address;             // 地址（含义随 type 不同，见上方说明）
    uint32_t size;                // 大小（含义随 type 不同，见上方说明）
    uint32_t intno;               // 中断号（仅 INTR/SVC 有效）
    uint32_t insn_bytes;          // 原始指令编码（仅 CODE 有效）
    uint64_t syscall_nr;          // 系统调用号，即 X8 的值（仅 SVC 有效）
    const char* symbol_name;      // 目标函数的符号名（仅 EXTERNAL_JUMP 有效，可能为 NULL）
    uint64_t mem_value;           // 内存读写的值（仅 MEM_READ/MEM_WRITE 有效）
    vc_event_action action;       // 用户可修改的动作（仅 EXTERNAL_JUMP 有效，默认 CONTINUE）
} vc_cpu_event_info;

/*
 * vc_cpu_event_callback — 事件回调函数签名
 *
 * @param ctx        当前 VM 上下文，可传给 vc_reg_read / vc_reg_write 等函数
 * @param event      事件详情（非 const 指针，允许你修改 event->action 来控制行为）
 * @param user_data  你在 vc_make_handle() 时传入的自定义数据指针
 *
 * 示例:
 *   void my_callback(vm_context* ctx, vc_cpu_event_info* event, void* user_data) {
 *       if (event->type == VC_CPU_EVENT_EXTERNAL_JUMP) {
 *           printf("即将调用外部函数: %s\n", event->symbol_name);
 *           if (strcmp(event->symbol_name, "dangerous_func") == 0) {
 *               uint64_t fake = 0;
 *               vc_reg_write(ctx, VC_REG_X0, &fake);  // 伪造返回值
 *               event->action = VC_ACTION_SKIP;         // 阻止真正调用
 *           }
 *       }
 *   }
 */
typedef void (*vc_cpu_event_callback)(vm_context* ctx,
                                      vc_cpu_event_info* event,
                                      void* user_data);

// ============================================================
//  vm_context — VM 上下文配置
// ============================================================

struct vm_context {
    bool traceJni;         // 是否 trace JNI 调用
    bool traceString;      // 是否打印所有字符串访问
    bool traceCode;        // 是否 trace 所有指令（内部 trace 输出）
    bool traceSystemLib;   // 是否 trace 系统库中的指令
    bool traceIO;          // 是否监听文件 I/O 操作
    bool traceSvc;         // 是否打印 SVC 系统调用的参数
    bool enableTaint;      // 是否启用污点追踪

    // 信号立即投递模式（默认 false）。
    // 开启后，非法 MRS（如 TPIDR_EL1、PMU）触发的 SIGILL 会立即投递，
    // MRS 后面同一个基本块内的指令不会被执行，与真机行为完全一致。
    // 代价：启用 Unicorn icount 模式，整体执行速度约降低 40%。
    // 关闭时信号在下一个基本块边界投递，同一块内 MRS 后面的指令仍会执行。
    bool immediateSignal;
    FILE *file;            // trace 输出文件，如果为 NULL 则不输出任何 trace 结果
    struct {
        uint64_t begin;    // trace 起始地址（0 表示从头开始）
        uint64_t end;      // trace 结束地址（0 表示直到结束）
    } traceRange;
    vc_cpu_event_callback cpuEventCallback;  // 用户注册的事件回调函数
    void* cpuEventUserData;                  // 传给回调的自定义数据指针
    uint32_t eventMask;                      // 当前启用的事件掩码，见 vc_event_mask
};

// ============================================================
//  核心 API
// ============================================================

/*
 * vc_make_handle — 创建一个由 VM 托管的可调用函数句柄
 *
 * 把 target_func 包装成一个新的函数指针，调用这个新指针时，
 * target_func 会在 VM（模拟器）中执行，而不是直接在 CPU 上跑。
 *
 * @param target_func  要放入 VM 执行的原始函数指针
 * @param ctx          输出参数，返回创建好的 vm_context（用于后续 vc_reg_read 等操作）
 * @param callback     可选，事件回调函数（传 NULL 表示不需要回调）
 * @param user_data    可选，传给回调的自定义数据
 * @return             包装后的函数指针，直接强转为对应签名调用即可；失败返回 NULL
 *
 * 示例:
 *   vm_context* ctx = NULL;
 *   auto fn = (int(*)(int,int))vc_make_handle((void*)my_add, &ctx, my_callback, NULL);
 *   int result = fn(1, 2);  // my_add(1,2) 在 VM 中执行
 *   vc_free(ctx);           // 用完释放
 */
void* vc_make_handle(void* target_func,
                     vm_context** ctx,
                     vc_cpu_event_callback callback = nullptr,
                     void* user_data = nullptr);

// 释放 vm_context 及其关联的 VM 资源。释放后 ctx 不可再使用。
void vc_free(vm_context* ctx);

// ============================================================
//  外部跳转控制 API（全局共享配置）
// ============================================================

/*
 * vc_set_jump_blacklist — 一次性设置跳转黑名单（清除旧配置后应用）
 *
 * @param names       SO 名称数组，NULL 结尾（如 {"libcrypto.so", "libssl.so", NULL}），可传 NULL
 * @param ranges      地址段数组 [base, end)，可传 NULL
 * @param range_count ranges 数组元素个数
 *
 * 示例:
 *   const char* bl[] = { "libcrypto.so", "libssl.so", NULL };
 *   vc_set_jump_blacklist(bl, NULL, 0);
 */
void vc_set_jump_blacklist(const char* const* names,
                           const uint64_t (*ranges)[2],
                           int range_count);

// 清除所有黑名单
void vc_clear_jump_blacklist();

// 设置是否允许跳入外部非系统库（默认 true），false = 全部跳出 host
void vc_set_external_jump_enabled(bool enabled);

// ============================================================
//  寄存器读写 API
// ============================================================

/*
 * vc_reg_read — 从 VM 中读取一个寄存器的值
 *
 * @param ctx    vm_context 指针（由 vc_make_handle 返回）
 * @param reg    要读取的寄存器编号，如 VC_REG_X0、VC_REG_PC、VC_REG_Q0 等
 * @param value  输出缓冲区。通用寄存器传 uint64_t*，Q 寄存器传 __uint128_t*
 * @return       VC_ERR_OK 成功，VC_ERR_ARG 参数错误，VC_ERR_HANDLE 上下文无效
 *
 * 示例:
 *   uint64_t pc;
 *   vc_reg_read(ctx, VC_REG_PC, &pc);
 */
// Inside VC_EVENT_ENABLE_CODE callbacks, this will use uc_reg_read_fast() when available.
vc_err vc_reg_read(vm_context* ctx, vc_reg reg, void* value);

/*
 * vc_reg_read_batch — 一次读取多个寄存器
 *
 * @param regs    寄存器编号数组
 * @param values  输出指针数组，values[i] 指向第 i 个寄存器值的存储位置
 * @param count   要读取的寄存器个数
 *
 * 示例:
 *   uint64_t pc, x0, sp;
 *   vc_reg regs[] = { VC_REG_PC, VC_REG_X0, VC_REG_SP };
 *   void* vals[]  = { &pc, &x0, &sp };
 *   vc_reg_read_batch(ctx, regs, vals, 3);
 */
// Inside VC_EVENT_ENABLE_CODE callbacks, this will use uc_reg_read_fast() per register.
vc_err vc_reg_read_batch(vm_context* ctx, const vc_reg* regs, void** values, int count);

/*
 * vc_reg_write — 向 VM 中写入一个寄存器的值
 *
 * 常见用途：在 EXTERNAL_JUMP 回调中伪造返回值，或修改 PC 改变执行流程。
 *
 * @param reg    要写入的寄存器编号
 * @param value  指向新值的指针。通用寄存器传 uint64_t*，Q 寄存器传 __uint128_t*
 *
 * 示例:
 *   uint64_t fake_ret = 42;
 *   vc_reg_write(ctx, VC_REG_X0, &fake_ret);
 */
vc_err vc_reg_write(vm_context* ctx, vc_reg reg, const void* value);

// 一次写入多个寄存器，参数格式同 vc_reg_read_batch。
vc_err vc_reg_write_batch(vm_context* ctx, const vc_reg* regs,
                          void* const* values, int count);

// ============================================================
//  控制与辅助 API
// ============================================================

// 停止 VM 模拟执行。可在回调中调用，VM 会在当前指令执行完后停止。
void vc_emu_stop(vm_context* ctx);

/*
 * vc_set_event_mask — 动态开关事件类型
 *
 * 修改哪些事件会触发回调。内部会根据 mask 变化自动增删底层 hook，
 * 所以关掉 CODE/MEM 后不会有任何额外性能开销。
 *
 * @param mask  vc_event_mask 的按位或组合
 *
 * 示例:
 *   // 开启逐指令监控
 *   vc_set_event_mask(ctx, ctx->eventMask | VC_EVENT_ENABLE_CODE);
 *   // 关闭基本块监控
 *   vc_set_event_mask(ctx, ctx->eventMask & ~VC_EVENT_ENABLE_BLOCK);
 */
void vc_set_event_mask(vm_context* ctx, uint32_t mask);

/*
 * vc_lookup_symbol — 根据地址查找函数符号名
 *
 * @param address  要查询的地址
 * @return         符号名字符串（内部管理生命周期），找不到返回 NULL
 */
const char* vc_lookup_symbol(vm_context* ctx, uint64_t address);


typedef size_t TraceHook;
typedef bool (*traceWriteListener)(vm_context* ctx, uint64_t address, int size, long value);
typedef bool (*traceReadListener)(vm_context* ctx, uint64_t address, int size);
void trace_write(vm_context* ctx, uint64_t begin, uint64_t end, traceWriteListener* onWrite);
void trace_write(vm_context* ctx, uint64_t begin, uint64_t end);
void trace_read(vm_context* ctx, uint64_t begin, uint64_t end, traceReadListener* onRead);
void trace_read(vm_context* ctx, uint64_t begin, uint64_t end);
void stopTrace(vm_context* ctx, TraceHook* hook_session);
typedef void (*callback)(void* data);
void add_hook(vm_context** ctx, TraceHook* hh, int type, void* callback,
              void* user_data, uint64_t begin, uint64_t end, ...);
void* trace(void* funcAddr, char* logPath, vm_context** ctx);
void* trace(void* funcAddr, char* logPath);
void freeTrace(uint64_t wrapper_func);
void* replace_trace(void* target_func, char* logPath);
void restore_function(void* target_func);

#endif //UNICRONTEST_ARM64EMULATOR_H
