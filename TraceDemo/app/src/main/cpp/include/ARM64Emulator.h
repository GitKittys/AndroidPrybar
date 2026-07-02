//
// Created by ASUS on 2024-07-14.
//

#ifndef UNICRONTEST_ARM64EMULATOR_H
#define UNICRONTEST_ARM64EMULATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

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

    _VC_REG_COUNT,   // （内部使用）枚举总数

    // --- 常用别名（放在 _VC_REG_COUNT 之后，避免重置枚举计数器）---
    VC_REG_FP = VC_REG_X29,   // 帧指针，等价于 X29
    VC_REG_LR = VC_REG_X30,   // 链接寄存器，等价于 X30
} vc_reg;

// ============================================================
//  动作控制
// ============================================================

struct vm_context;

typedef enum {
    VC_ACTION_CONTINUE = 0,  // 正常执行（默认）
    VC_ACTION_SKIP     = 1,  // 跳过这次外部函数调用，VM 直接从 LR 地址继续执行
} vc_event_action;

// ============================================================
//  Hook 系统 — 分类型注册回调
// ============================================================

/*
 * vc_hook_type — hook 类型枚举（位值与 Unicorn UC_HOOK_* 一致）
 *
 * 包含 Unicorn 原生 hook 类型和自定义扩展类型。
 * 可按位或组合使用（如 VC_HOOK_MEM_ALL）。
 */
typedef enum {
    // Unicorn 原生 hook 类型（位值 = UC_HOOK_*）
    VC_HOOK_INTR       = 1 << 0,   // 中断回调          (= UC_HOOK_INTR)
    VC_HOOK_CODE       = 1 << 2,   // 逐指令回调        (= UC_HOOK_CODE, 内部用 CODE_FAST)
    VC_HOOK_BLOCK      = 1 << 3,   // 基本块入口回调    (= UC_HOOK_BLOCK)
    VC_HOOK_MEM_READ   = 1 << 10,  // 内存读回调        (= UC_HOOK_MEM_READ)
    VC_HOOK_MEM_WRITE  = 1 << 11,  // 内存写回调        (= UC_HOOK_MEM_WRITE)
    VC_HOOK_MEM_READ_AFTER = 1 << 13, // 内存读后回调   (= UC_HOOK_MEM_READ_AFTER)

    // 组合便利宏
    VC_HOOK_MEM_ALL    = (1 << 10) | (1 << 11),

    // 自定义扩展 hook 类型（高位，不与 UC_HOOK_* 冲突）
    VC_HOOK_SVC            = 1 << 20,  // SVC 系统调用回调
    VC_HOOK_EXTERNAL_JUMP  = 1 << 21,  // 外部函数跳转回调
} vc_hook_type;

/*
 * vc_mem_type — 内存访问类型（值与 Unicorn uc_mem_type 一致）
 *
 * MEM hook 回调的 type 参数使用此枚举。
 */
typedef enum {
    VC_MEM_READ       = 16,   // 内存读
    VC_MEM_WRITE      = 17,   // 内存写
    VC_MEM_READ_AFTER = 25,   // 内存读后（可拿到读取的值）
} vc_mem_type;

/*
 * vc_cb_hookcode_t — CODE / BLOCK 回调签名（= uc_cb_hookcode_t，首参换 vm_context*）
 *
 * @param ctx        当前 VM 上下文
 * @param address    CODE: 当前指令地址; BLOCK: 基本块起始地址
 * @param size       CODE: 指令字节数; BLOCK: 块大小(字节)
 * @param user_data  注册时传入的自定义数据
 */
typedef void (*vc_cb_hookcode_t)(vm_context* ctx, uint64_t address, uint32_t size, void* user_data);

/*
 * vc_cb_hookintr_t — INTR 回调签名（= uc_cb_hookintr_t，首参换 vm_context*）
 *
 * @param intno      中断号（intno==2 为 SVC，但如果你只关心 SVC，建议用 VC_HOOK_SVC）
 */
typedef void (*vc_cb_hookintr_t)(vm_context* ctx, uint32_t intno, void* user_data);

/*
 * vc_cb_hookmem_t — MEM 回调签名（= uc_cb_hookmem_t，首参换 vm_context*）
 *
 * @param type       实际触发的访问类型（vc_mem_type: VC_MEM_READ / VC_MEM_WRITE）
 * @param address    访问的内存地址
 * @param size       访问字节数
 * @param value      写入的值（WRITE 时有效），读取的值（READ_AFTER 时有效）
 */
typedef void (*vc_cb_hookmem_t)(vm_context* ctx, vc_mem_type type,
                                uint64_t address, int size, int64_t value, void* user_data);

/*
 * vc_cb_hooksvc_t — SVC 回调签名（自定义扩展）
 *
 * @param address    SVC 指令地址
 * @param syscall_nr 系统调用号（X8 的值）
 */
typedef void (*vc_cb_hooksvc_t)(vm_context* ctx, uint64_t address, uint32_t syscall_nr, void* user_data);

/*
 * vc_cb_hookjump_t — EXTERNAL_JUMP 回调签名（自定义扩展）
 *
 * 在回调中可以：
 *   1. 用 vc_reg_write(ctx, VC_REG_X0, &fake_ret) 设置伪造返回值
 *   2. 设置 *action = VC_ACTION_SKIP 阻止真正调用
 *
 * @param address      跳转目标地址
 * @param symbol_name  目标函数符号名（可能为 NULL）
 * @param action       输出参数，设置 VC_ACTION_SKIP 可阻止跳转
 */
typedef void (*vc_cb_hookjump_t)(vm_context* ctx, uint64_t address,
                                 const char* symbol_name, vc_event_action* action, void* user_data);

// Hook 句柄（用于 vc_hook_del 删除）
typedef size_t vc_hook_h;

// ============================================================
//  vm_context — 不透明 VM 上下文（内部定义在 internal.h）
// ============================================================

// ============================================================
//  核心 API
// ============================================================

/*
 * vc_make_handle — 创建一个由 VM 托管的可调用函数句柄
 *
 * 把 target_func 包装成一个新的函数指针，调用这个新指针时，
 * target_func 会在 VM（模拟器）中执行，而不是直接在 CPU 上跑。
 * 创建后通过 vc_hook_add 注册回调来监控执行过程。
 *
 * @param target_func  要放入 VM 执行的原始函数指针
 * @param ctx          输出参数，返回创建好的 vm_context（用于后续操作）
 * @return             包装后的函数指针，直接强转为对应签名调用即可；失败返回 NULL
 *
 * 示例:
 *   vm_context* ctx = NULL;
 *   auto fn = (int(*)(int,int))vc_make_handle((void*)my_add, &ctx);
 *   vc_hook_h hh;
 *   vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)my_block_cb, NULL, 0, 0);
 *   int result = fn(1, 2);  // my_add(1,2) 在 VM 中执行
 *   vc_free(ctx);           // 用完释放
 */
void* vc_make_handle(void* target_func, vm_context** ctx);

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
//  Hook 注册/删除 API
// ============================================================

/*
 * vc_hook_add — 注册 hook 回调
 *
 * 根据 type 注册对应类型的回调，callback 的签名必须与 type 匹配：
 *   VC_HOOK_CODE / VC_HOOK_BLOCK       → vc_cb_hookcode_t
 *   VC_HOOK_INTR                       → vc_cb_hookintr_t
 *   VC_HOOK_MEM_READ / VC_HOOK_MEM_WRITE / VC_HOOK_MEM_ALL → vc_cb_hookmem_t
 *   VC_HOOK_SVC                        → vc_cb_hooksvc_t
 *   VC_HOOK_EXTERNAL_JUMP              → vc_cb_hookjump_t
 *
 * @param ctx        VM 上下文
 * @param hh         输出参数，返回 hook 句柄（用于后续删除）
 * @param type       hook 类型（vc_hook_type 的值或按位或组合）
 * @param callback   回调函数指针（签名必须与 type 对应）
 * @param user_data  传给回调的自定义数据
 * @param begin      hook 生效的起始地址（0 = 不限）
 * @param end        hook 生效的结束地址（0 = 不限）
 * @return           VC_ERR_OK 成功
 *
 * 示例:
 *   // 监听外部函数跳转
 *   vc_hook_h hh;
 *   void my_jump(vm_context* ctx, uint64_t addr, const char* sym,
 *                vc_event_action* action, void* ud) {
 *       printf("跳转到: %s\n", sym);
 *       if (sym && strcmp(sym, "exit") == 0) {
 *           *action = VC_ACTION_SKIP;
 *       }
 *   }
 *   vc_hook_add(ctx, &hh, VC_HOOK_EXTERNAL_JUMP, (void*)my_jump, NULL, 0, 0);
 *
 *   // 监听基本块
 *   void my_block(vm_context* ctx, uint64_t addr, uint32_t size, void* ud) {
 *       printf("block @ 0x%lx size=%u\n", addr, size);
 *   }
 *   vc_hook_add(ctx, &hh, VC_HOOK_BLOCK, (void*)my_block, NULL, 0, 0);
 */
vc_err vc_hook_add(vm_context* ctx, vc_hook_h* hh, int type,
                   void* callback, void* user_data,
                   uint64_t begin, uint64_t end);

/*
 * vc_hook_del — 删除已注册的 hook
 *
 * @param ctx  VM 上下文
 * @param hh   vc_hook_add 返回的句柄
 */
vc_err vc_hook_del(vm_context* ctx, vc_hook_h hh);

// ============================================================
//  控制与辅助 API
// ============================================================

/*
 * vc_emu_stop — 停止 VM 模拟执行
 *
 * 可在任何回调中调用。VM 会在当前 TB 执行完后停止，
 * 包括正在处理信号的情况下也会立即退出信号循环。
 * 停止后 vc_reg_read 可读取停止时的寄存器状态。
 */
void vc_emu_stop(vm_context* ctx);

const char* vc_lookup_symbol(vm_context* ctx, uint64_t address);

// ============================================================
//  单步与受控执行 API（仅在 hook 回调中调用）
// ============================================================

/*
 * vc_single_step — 执行 N 条指令后暂停
 *
 * 在 BLOCK/CODE/EXTERNAL_JUMP 等回调中调用。当前回调返回后，
 * VM 恢复执行 count 条指令，然后再次触发回调链。
 * 回调中不调用此函数则恢复正常（无限制）执行。
 *
 * @param count  要执行的指令数（1 = 单步）
 */
void vc_single_step(vm_context* ctx, size_t count);

/*
 * vc_set_until — 设置一个停止地址
 *
 * 在回调中调用。VM 执行到 addr 时停止（等价于临时断点）。
 * 传 0 清除。到达 until 地址后自动清除。
 */
void vc_set_until(vm_context* ctx, uint64_t addr);

// ============================================================
//  反汇编 API
// ============================================================

typedef struct {
    uint64_t address;   // 指令地址
    uint8_t  size;      // 指令字节数（ARM64 固定 4）
    uint32_t bytes;     // 原始机器码
    char mnemonic[32];  // 助记符
    char op_str[160];   // 操作数
} vc_insn;

/*
 * vc_disasm — 反汇编指定地址的 ARM64 指令
 *
 * identity mapping 下 addr 就是 host 指针，不需要 vm_context。
 * 内部使用 Capstone 引擎。
 *
 * @param addr       起始地址
 * @param max_count  最多反汇编几条
 * @param out        输出数组（调用者分配）
 * @return           实际反汇编的指令数，失败返回 0
 */
int vc_disasm(uint64_t addr, int max_count, vc_insn* out);

// ============================================================
//  CPU 状态快照 API
// ============================================================

// 不透明类型，持有某一时刻的完整 CPU 状态（所有寄存器 + 内部标志）。
typedef struct vc_cpu_context vc_cpu_context;

/*
 * vc_context_save — 保存当前 CPU 状态快照
 *
 * 在回调中调用，捕获当前指令执行时的完整寄存器状态。
 * 返回的快照可在之后用 vc_context_restore 恢复。
 *
 * @param ctx   VM 上下文
 * @param snap  输出参数，返回分配好的快照。用完必须调 vc_context_free 释放。
 */
vc_err vc_context_save(vm_context* ctx, vc_cpu_context** snap);

/*
 * vc_context_restore — 恢复之前保存的 CPU 状态
 *
 * 在回调中调用，将所有寄存器恢复到快照时的状态。
 * 恢复后继续执行时 PC 会从快照中的地址开始。
 *
 * @param ctx   VM 上下文
 * @param snap  之前由 vc_context_save 返回的快照
 */
vc_err vc_context_restore(vm_context* ctx, vc_cpu_context* snap);

/*
 * vc_context_free — 释放 CPU 状态快照
 *
 * @param snap  要释放的快照，传 NULL 安全无操作
 */
void vc_context_free(vc_cpu_context* snap);

// ============================================================
//  Trace 快捷 API
// ============================================================

void* trace(void* funcAddr, char* logPath, vm_context** ctx);
void* trace(void* funcAddr, char* logPath);
void freeTrace(uint64_t wrapper_func);
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

#endif //UNICRONTEST_ARM64EMULATOR_H
