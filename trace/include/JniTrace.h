//
// Created by ASUS on 2025-04-28.
//

#ifndef UNICRONTEST_JNITRACE_H
#define UNICRONTEST_JNITRACE_H

#include "ARM64Emulator.h"        // vm_context / vc_hook（公开 API，不再依赖 internal.h）


// 建表（一次，须在非 hook 上下文调，trace() setup 里）
void jni_ensure_table();
// 地址是否是 libart 的 JNI 函数（trace_code 认出调用后查表用）
bool isJniAddress(uint64_t address);
// 【入口段】trace_code 认出「调用指令 + 目标命中 jni_table」时调（调用点，尚未跳转）。
// retAddr 由调用方算：BLR/BL -> addr+4；BR(尾调) -> 原始调用点的返回地址。
void jni_on_call(vm_context* ctx, uint64_t jniAddr, uint64_t retAddr);

// ---- 解耦后的 JNI 返回值延迟打印 ----
// g_jni_pending_ret: 有 JNI 调用飞行中 = 其返回地址，否则 0。trace_code 顶部廉价比较用。
extern thread_local uint64_t g_jni_pending_ret;
// trace_code 命中 g_jni_pending_ret 那一行时调：X0=返回值天然在，回填参数寄存器后原样跑 handler 打印。
void jni_on_return(vm_context* ctx, vm_context* uc);

#endif //UNICRONTEST_JNITRACE_H
