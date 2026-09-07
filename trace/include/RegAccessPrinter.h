//
// Created by ASUS on 2025-01-22.
//

#ifndef UNICRONTEST_REGACCESSPRINTER_H
#define UNICRONTEST_REGACCESSPRINTER_H


#include "ARM64Emulator.h"
#include "capstone.h"
#include "StringUtil.h"

// 批量寄存器缓存索引: 0-28=x0-x28, 29=x29, 30=x30, 31=sp, 32=nzcv
// 返回 -1 表示不在缓存中（Q/V/D/S 等）
static inline int cachedXRegIndex(int uc_reg_id) {
    if (uc_reg_id >= VC_REG_X0 && uc_reg_id <= VC_REG_X28)
        return uc_reg_id - VC_REG_X0;       // 0-28
    if (uc_reg_id == VC_REG_X29) return 29;
    if (uc_reg_id == VC_REG_X30) return 30;
    if (uc_reg_id == VC_REG_SP)  return 31;
    if (uc_reg_id == VC_REG_NZCV) return 32;
    // w0-w30 → 对应 x 寄存器的低 32 位
    if (uc_reg_id >= VC_REG_W0 && uc_reg_id <= VC_REG_W30)
        return uc_reg_id - VC_REG_W0;        // 0-30
    return -1;
}

struct RegAccessPrinter;
void regAccessPrinterImpl(RegAccessPrinter* regAccessPrinter,vm_context * ucEngine,StringBuilder* builder,uint64_t address);
struct RegAccessPrinter{
    vm_context *uc;
    uint64_t address; //要打印的地址
    cs_insn* csInsn;
    cs_regs accessRegs; //被操作的寄存器
    unsigned char regs_count; //读写寄存器的数量
    bool forWriteRegs;
    void(*print)(RegAccessPrinter* regAccessPrinter,vm_context * ucEngine,StringBuilder* builder,uint64_t address);
    const uint64_t* cachedXRegs; // 指向 TraceInfo::cachedXRegs，为 nullptr 时 fallback 到 vc_reg_read_fast
};

#endif //UNICRONTEST_REGACCESSPRINTER_H
