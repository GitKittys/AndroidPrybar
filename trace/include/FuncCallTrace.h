//
// Created by ASUS on 2026-03-16.
//

#ifndef UNICRONTEST_FUNCCALLTRACE_H
#define UNICRONTEST_FUNCCALLTRACE_H

#include "ARM64Emulator.h"     // vm_context（公开 API，不再依赖 internal.h）
#include "svc_handler.h"
#include "StringUtil.h"   // StringBuilder

struct DirectWriteBuf;
void* addFuncCallTrace(vm_context** vmContext, FdTracker* shared_fd_tracker = nullptr, DirectWriteBuf* dwBuf = nullptr);
void freeFuncCallTraceCtx(void* ctx);

// trace_code 用 capstone 认出外部调用指令时直接调用（[CALL] 全在 trace 层做，不经 VCPU）：
//   is_return=false: 调用前，address=外部目标, pre_x0_x7=入参 → 打印 [CALL] name(args)
//   is_return=true:  返回时(下一条 addr+4), ret_x0=X0 → 补 => 返回值
void funcCallTraceEmit(void* fctCtx, vm_context* uc,
                       uint64_t address, const char* symName,
                       const uint64_t* pre_x0_x7, uint64_t ret_x0,
                       bool is_return);

// ---- 逐指令 trace 给「数据地址」标注归属（复用 maps 快照 PtrRegion，热路径 TLS 缓存）----
// appendRegionTag: 给 mem_r/mem_w 的访存地址追加归属:
//   命名 SO → " so名:ro/:code/:rw" +（命中符号时）" 符号+0xoff"；匿名 → " [stack]/[heap]/anon"。
//   热路径:TLS 上次命中缓存(区域+符号表指针)，findContaining 二分、无锁无 dladdr。
void appendRegionTag(StringBuilder* sb, uint64_t addr);

#endif //UNICRONTEST_FUNCCALLTRACE_H
