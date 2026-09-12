//
// Created by ASUS on 2024-10-02.
// 2026-09: 从底层 VCPU 解耦到开源 trace 层。
//
// 内置内存读写监控 hook — trace_read / trace_write 的实现。
//
// 设计要点：
//   1. 每种 hook（read/write）per-ctx 只能添加一次（去重），防止用户多次调用导致
//      hook 叠加、输出翻倍。去重表在 trace 层自己维护（不再依赖 VCPU 内部字段）。
//   2. 输出优先级：trace 模式 DirectWriteBuf → 用户传入的 FILE* → 都没有则静默跳过。
//      trace() 走 DirectWriteBuf（4MB mmap 零拷贝），裸 vc_make_handle 走 FILE*。
//   3. hook 通过公开的 add_hook 注册：引擎起来前登记到 deferred（每个新 bundle 自动带上），
//      引擎运行中则直接装到当前 bundle。
//
// 解耦说明：本文件只用公开 API（ARM64Emulator.h），不含 unicorn.h / internal.h。
//   · 寄存器读取走 vc_reg_read（不碰 uc_engine）。
//   · hook 类型用 VC_HOOK_MEM_READ/WRITE（其值等于 Unicorn 的 UC_HOOK_MEM_READ/WRITE）。
//   · MEM 回调签名与 Unicorn 原生 mem 回调 ABI 兼容（首参 engine 用 void* 接住但不使用）。

#include "ARM64Emulator.h"
#include "LibraryUtils.h"
#include "Utils.h"
#include "logging.h"
#include "DirectWriteBuf.h"
#include "SafeMemRead.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <mutex>

// trace 层（EastTrace.cpp）定义的 per-thread 活跃输出 buffer。
// weak：裸 vc_make_handle（无 trace 输出）场景解析为 nullptr，printMsg 自动走 FILE* 或跳过。
__attribute__((weak)) extern thread_local DirectWriteBuf* tls_trace_active_dwbuf;

struct TraceMemoryHookStruct {
    vm_context* pVmContext;         // 所属 VM 上下文（传给 listener 回调）
    traceWriteListener* onWrite;    // 写过滤器：返回 true 才输出，nullptr = 全部输出
    traceReadListener* onRead;      // 读过滤器：同上
    FILE* output_file;              // 裸 vc_make_handle 时的输出文件（trace 模式下为 nullptr）
};

// per-ctx 去重表（trace 层自持，不依赖底层实现的内部字段）。
static std::mutex g_trace_mem_mutex;
static std::set<vm_context*> g_read_registered;
static std::set<vm_context*> g_write_registered;

// 格式化内存访问信息并输出。
// 输出优先级：DirectWriteBuf（trace 模式，零拷贝）→ FILE*（裸 VM 模式）→ 跳过。
static void printMsg(vm_context* ctx, char* type, unsigned long address, int size, char* value, FILE* fallback) {
    DirectWriteBuf* dwb = tls_trace_active_dwbuf;
    if (!dwb && !fallback) return;

    // PC/LR 走公开 API 读取（不碰 uc_engine），callback 里 tls 当前 bundle 即触发本次访问的引擎。
    uint64_t pc = 0, lr = 0;
    vc_reg_read(ctx, VC_REG_PC, &pc);
    vc_reg_read(ctx, VC_REG_LR, &lr);

    char buffer[520];
    int offset = 0;
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s%lx", type, address);
    if (size) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", data size=0x%x, data value=%s", size, value);
    }

    LibraryInfo current_pc_info = findModuleByAddress(pc);
    auto lr_info = findAddressDlInfo(lr);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", PC=0x%lx@[%s]%#lx", pc,
                       getPathFileName(current_pc_info.name.c_str()).c_str(), pc - current_pc_info.base_address);
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", LR=%s0x%lx@[%s]%#lx\n",
                       get_permissions_string(current_pc_info.permissions), lr, getPathFileName(lr_info.dli_fname).c_str(),
                       lr - (uint64_t)lr_info.dli_fbase);

    if (dwb) {
        dwb->write(buffer, offset);
    } else {
        fwrite(buffer, 1, offset, fallback);
    }
}

// UC_HOOK_MEM_READ 回调 — 每次 guest 读内存时触发。
// 签名与 Unicorn 原生 mem 回调 ABI 兼容：首参 engine 用 void* 接住但不使用，
// type(uc_mem_type 枚举=int) 也不使用。identity mapping 下 address 就是 host 地址，
// 用 safe_host_read（process_vm_readv）安全读取，避免 PROT_NONE / execute-only 页 SIGSEGV。
static void hook_read(void* /*uc*/, int /*type*/,
                      uint64_t address, int size, int64_t /*value*/,
                      void* traceMemoryHook) {
    auto traceMemory = (TraceMemoryHookStruct*)(traceMemoryHook);
    vm_context* ctx = traceMemory->pVmContext;

    char buffer[64];
    uint64_t tmp = 0;
    if (safe_host_read(address, &tmp, size <= 8 ? size : 8)) {
        switch (size) {
            case 1:  snprintf(buffer, sizeof(buffer), "0x%02x",    (unsigned)(uint8_t)tmp); break;
            case 2:  snprintf(buffer, sizeof(buffer), "0x%04x",    (unsigned)(uint16_t)tmp); break;
            case 4:  snprintf(buffer, sizeof(buffer), "0x%08x",    (unsigned)(uint32_t)tmp); break;
            case 8:  snprintf(buffer, sizeof(buffer), "0x%016llx", (unsigned long long)tmp); break;
            default: snprintf(buffer, sizeof(buffer), "0x%llx",    (unsigned long long)tmp); break;
        }
    } else {
        snprintf(buffer, sizeof(buffer), "<unreadable>");
    }

    if (traceMemory->onRead == nullptr || ((traceReadListener)traceMemory->onRead)(ctx, address, size)) {
        char time_buffer[38];
        getCurrentTimeWithMilliseconds(time_buffer, sizeof(time_buffer));
        strcat(time_buffer, "\n Memory READ at 0x");
        printMsg(ctx, time_buffer, address, size, buffer, traceMemory->output_file);
    }
}

// UC_HOOK_MEM_WRITE 回调 — 每次 guest 写内存时触发。
// write 的 value 由引擎直接传入（写入值），不需要 safe_host_read。
static void hook_write(void* /*uc*/, int /*type*/,
                       uint64_t address, int size, int64_t value,
                       void* traceMemoryHook) {
    auto traceMemory = (TraceMemoryHookStruct*)(traceMemoryHook);
    vm_context* ctx = traceMemory->pVmContext;

    if (traceMemory->onWrite == nullptr || ((traceWriteListener)traceMemory->onWrite)(ctx, address, size, value)) {
        char time_buffer[38];
        getCurrentTimeWithMilliseconds(time_buffer, sizeof(time_buffer));
        strcat(time_buffer, " Memory WRITE at 0x");
        char hexChar[24];

        switch (size) {
            case 1:  snprintf(hexChar, sizeof(hexChar), "0x%02x",    (unsigned)(uint8_t)(value & 0xff)); break;
            case 2:  snprintf(hexChar, sizeof(hexChar), "0x%04x",    (unsigned)(uint16_t)(value & 0xffff)); break;
            case 4:  snprintf(hexChar, sizeof(hexChar), "0x%08x",    (unsigned)(uint32_t)(value & 0xffffffff)); break;
            case 8:  snprintf(hexChar, sizeof(hexChar), "0x%016llx", (unsigned long long)value); break;
            default: snprintf(hexChar, sizeof(hexChar), "0x%llx",    (unsigned long long)value); break;
        }

        printMsg(ctx, time_buffer, address, size, hexChar, traceMemory->output_file);
    }
}

// trace_read(FILE*) — 裸 vc_make_handle 模式下添加内存读监控。
// per-ctx 去重：同一个 ctx 只添加一次，重复调用静默忽略（避免在 hook 中每次都调导致叠加）。
__attribute__((visibility("default"))) void trace_read(vm_context* ctx, uint64_t begin, uint64_t end, FILE* file) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lk(g_trace_mem_mutex);
        if (!g_read_registered.insert(ctx).second) return;
    }
    auto data = (TraceMemoryHookStruct*)calloc(1, sizeof(TraceMemoryHookStruct));
    data->pVmContext = ctx;
    data->output_file = file;

    TraceHook hh = 0;
    add_hook(&ctx, &hh, VC_HOOK_MEM_READ, (void*)hook_read, data, begin, end);
}

// trace_read(listener) — 带过滤器的内存读监控。listener 返回 false 抑制该条输出。
__attribute__((visibility("default"))) void trace_read(vm_context* ctx, uint64_t begin, uint64_t end, traceReadListener* onRead) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lk(g_trace_mem_mutex);
        if (!g_read_registered.insert(ctx).second) return;
    }
    auto data = (TraceMemoryHookStruct*)calloc(1, sizeof(TraceMemoryHookStruct));
    data->pVmContext = ctx;
    data->onRead = onRead;

    TraceHook hh = 0;
    add_hook(&ctx, &hh, VC_HOOK_MEM_READ, (void*)hook_read, data, begin, end);
}

__attribute__((visibility("default"))) void trace_read(vm_context* ctx, uint64_t begin, uint64_t end) {
    return trace_read(ctx, begin, end, (FILE*)nullptr);
}

// trace_write(FILE*) — 裸 vc_make_handle 模式下添加内存写监控。去重同 trace_read。
__attribute__((visibility("default"))) void trace_write(vm_context* ctx, uint64_t begin, uint64_t end, FILE* file) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lk(g_trace_mem_mutex);
        if (!g_write_registered.insert(ctx).second) return;
    }
    auto data = (TraceMemoryHookStruct*)calloc(1, sizeof(TraceMemoryHookStruct));
    data->pVmContext = ctx;
    data->output_file = file;

    TraceHook hh = 0;
    add_hook(&ctx, &hh, VC_HOOK_MEM_WRITE, (void*)hook_write, data, begin, end);
}

// trace_write(listener) — 带过滤器的内存写监控。
__attribute__((visibility("default"))) void trace_write(vm_context* ctx, uint64_t begin, uint64_t end, traceWriteListener* onWrite) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lk(g_trace_mem_mutex);
        if (!g_write_registered.insert(ctx).second) return;
    }
    auto data = (TraceMemoryHookStruct*)calloc(1, sizeof(TraceMemoryHookStruct));
    data->pVmContext = ctx;
    data->onWrite = onWrite;

    TraceHook hh = 0;
    add_hook(&ctx, &hh, VC_HOOK_MEM_WRITE, (void*)hook_write, data, begin, end);
}

__attribute__((visibility("default"))) void trace_write(vm_context* ctx, uint64_t begin, uint64_t end) {
    return trace_write(ctx, begin, end, (FILE*)nullptr);
}

__attribute__((visibility("default"))) void stopTrace(vm_context* ctx, TraceHook* hook_session) {
    // 预留：当前内存监控 hook 生命周期与 vm_context 绑定，随 vc_free 释放，无需单独摘除。
    (void)ctx; (void)hook_session;
}
