//
// Created by ASUS on 2025-01-22.
//

#include "ARM64Emulator.h"
#include "RegAccessPrinter.h"
#include "ARM.h"
#include "Disassembly.h"
#include "arm64.h"

struct CachedRegName { const char* name; uint8_t len; };
static CachedRegName s_rn_cache[AARCH64_REG_ENDING + 1];
static bool s_rn_init = false;

static inline void appendRegName(StringBuilder* builder, uint16_t capstone_reg_id) {
    if (__builtin_expect(!s_rn_init, false)) {
        for (int i = 0; i < AARCH64_REG_ENDING; i++) {
            const char* n = cs_reg_name(i);
            s_rn_cache[i].name = n ? n : "?";
            s_rn_cache[i].len = (uint8_t)strlen(s_rn_cache[i].name);
        }
        s_rn_cache[AARCH64_REG_ENDING] = {"?", 1};
        s_rn_init = true;
    }
    int idx = (capstone_reg_id < AARCH64_REG_ENDING) ? capstone_reg_id : AARCH64_REG_ENDING;
    appendStringN(builder, s_rn_cache[idx].name, s_rn_cache[idx].len);
}


/**
 * 输出读写的寄存器，在里面判断读写的寄存器类型，打印出不同的格式，在反汇编字符串的末尾拼接上
 * @param regAccessPrinter
 * @param ucEngine
 * @param builder
 * @param address
 */
void regAccessPrinterImpl(RegAccessPrinter* regAccessPrinter,vm_context * ucEngine,StringBuilder* builder,uint64_t address){
#define hashBit(value, offset) (((value) >> (offset)) & 1) == 1
    if(regAccessPrinter->address != address){
        return;
    }
    const uint64_t* cache = regAccessPrinter->cachedXRegs;
    if(regAccessPrinter->regs_count){
        for(int i = 0;i<regAccessPrinter->regs_count;i++){
            uint16_t capstone_reg_id = regAccessPrinter->accessRegs[i];
            int reg_id =  mapToUnicornReg(capstone_reg_id);
            if(reg_id == -1){
                return;
            }
            // 注意：显式列举，不依赖枚举相邻关系（vc_reg 里 NZCV 不挨着 SP，
            // 而 Unicorn 枚举里是挨着的——照抄区间会漏掉 NZCV）
            if((reg_id >= VC_REG_X0 && reg_id <= VC_REG_X30) ||
               reg_id == VC_REG_SP || reg_id == VC_REG_NZCV){
                if (regAccessPrinter->forWriteRegs) {
                    appendStringN(builder," =>",3);
                    regAccessPrinter->forWriteRegs = false;
                }
                if(reg_id == VC_REG_NZCV){
                    uint32_t nzcvValue;
                    int idx = cache ? cachedXRegIndex(reg_id) : -1;
                    if (idx >= 0) {
                        nzcvValue = (uint32_t)cache[idx];
                    } else {
                        vc_reg_read_fast(ucEngine,VC_REG_NZCV,&nzcvValue);
                    }
                    appendStringN(builder, " nzcv: N=", 9);
                    appendChar(builder, (hashBit(nzcvValue,31)) ? '1' : '0');
                    appendStringN(builder, ", Z=", 4);
                    appendChar(builder, (hashBit(nzcvValue,30)) ? '1' : '0');
                    appendStringN(builder, ", C=", 4);
                    appendChar(builder, (hashBit(nzcvValue,29)) ? '1' : '0');
                    appendStringN(builder, ", V=", 4);
                    appendChar(builder, (hashBit(nzcvValue,28)) ? '1' : '0');
                } else{
                    uint64_t value;
                    int idx = cache ? cachedXRegIndex(reg_id) : -1;
                    if (idx >= 0) {
                        value = cache[idx];
                    } else {
                        vc_reg_read_fast(ucEngine,(vc_reg)reg_id,&value);
                    }
                    appendChar(builder, ' ');
                    appendRegName(builder, capstone_reg_id);
                    appendStringN(builder, "=0x", 3);
                    char hex[17];
                    int hexLen = uint64ToHex(value, hex);
                    appendStringN(builder, hex, hexLen);
                }

            } else if(reg_id>=VC_REG_W0 && reg_id <=VC_REG_W30){
                if(regAccessPrinter->forWriteRegs){
                    appendStringN(builder," =>",3);
                    regAccessPrinter->forWriteRegs = false;
                }
                uint32_t value;
                int idx = cache ? cachedXRegIndex(reg_id) : -1;
                if (idx >= 0) {
                    value = (uint32_t)cache[idx];
                } else {
                    vc_reg_read_fast(ucEngine,(vc_reg)reg_id,&value);
                }
                appendChar(builder, ' ');
                appendRegName(builder, capstone_reg_id);
                appendStringN(builder, "=0x", 3);
                char hex[17];
                int hexLen = uint64ToHex(value, hex);
                appendStringN(builder, hex, hexLen);
            }

            else if (reg_id >= VC_REG_Q0 && reg_id <= VC_REG_Q31) {
                if (regAccessPrinter->forWriteRegs) {
                    appendStringN(builder, " =>",3);
                    regAccessPrinter->forWriteRegs = false;
                }
                uint8_t qvalue[16];
                vc_reg_read_fast(ucEngine, (vc_reg)reg_id, qvalue);
                char hexStr[33];
                for (int j = 0; j < 16; j++) {
                    byteToHex(qvalue[j], hexStr + j * 2);
                }
                hexStr[32] = '\0';
                appendChar(builder, ' ');
                appendRegName(builder, capstone_reg_id);
                appendStringN(builder, "=0x", 3);
                appendStringN(builder, hexStr, 32);
            }
        }

    }

}
