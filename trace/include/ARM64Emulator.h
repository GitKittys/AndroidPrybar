// 兼容垫片:历史上公开头叫 ARM64Emulator.h(vcpu + trace 合一)。
// 现已拆分为分离的 vcpu.h(VCPU 核心)与 trace.h(trace 工具)。
// trace 源码内部仍 #include "ARM64Emulator.h",这里聚合到分离后的头,免改源码。
// 对外分发的公开头请直接用 include/vcpu.h 或 include/trace.h。
#ifndef ARM64EMULATOR_COMPAT_H
#define ARM64EMULATOR_COMPAT_H
#include "trace.h"   // trace.h 顶部已 #include "vcpu.h"
#endif
