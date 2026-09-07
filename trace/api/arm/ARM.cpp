//
// Created by ASUS on 2025-01-20.
//

#include "ARM64Emulator.h"
#include <cstring>
#include "ARM.h"
//#include "unicorn/unicorn.h"
//#include "capstone.h"
#include "Utils.h"
#include "SymbolTable.h"

// ---- Capstone → Unicorn 寄存器映射表 (数组直接索引，O(1)) ----
// 替代 unordered_map，消除哈希 + 桶查找开销。
// AARCH64_REG_ENDING = 700，数组仅占 2.8KB。
static int capstoneToUnicornRegTable[AARCH64_REG_ENDING];
static bool regTableInitialized = false;

static void initRegTable() {
    if (regTableInitialized) return;
    // 全部初始化为 -1 (未映射)
    memset(capstoneToUnicornRegTable, 0xFF, sizeof(capstoneToUnicornRegTable));

    capstoneToUnicornRegTable[AARCH64_REG_NZCV] = VC_REG_NZCV;
    capstoneToUnicornRegTable[AARCH64_REG_SP]   = VC_REG_SP;
    capstoneToUnicornRegTable[AARCH64_REG_LR]   = VC_REG_LR;
    // X0-X29
    capstoneToUnicornRegTable[AARCH64_REG_X0]  = VC_REG_X0;
    capstoneToUnicornRegTable[AARCH64_REG_X1]  = VC_REG_X1;
    capstoneToUnicornRegTable[AARCH64_REG_X2]  = VC_REG_X2;
    capstoneToUnicornRegTable[AARCH64_REG_X3]  = VC_REG_X3;
    capstoneToUnicornRegTable[AARCH64_REG_X4]  = VC_REG_X4;
    capstoneToUnicornRegTable[AARCH64_REG_X5]  = VC_REG_X5;
    capstoneToUnicornRegTable[AARCH64_REG_X6]  = VC_REG_X6;
    capstoneToUnicornRegTable[AARCH64_REG_X7]  = VC_REG_X7;
    capstoneToUnicornRegTable[AARCH64_REG_X8]  = VC_REG_X8;
    capstoneToUnicornRegTable[AARCH64_REG_X9]  = VC_REG_X9;
    capstoneToUnicornRegTable[AARCH64_REG_X10] = VC_REG_X10;
    capstoneToUnicornRegTable[AARCH64_REG_X11] = VC_REG_X11;
    capstoneToUnicornRegTable[AARCH64_REG_X12] = VC_REG_X12;
    capstoneToUnicornRegTable[AARCH64_REG_X13] = VC_REG_X13;
    capstoneToUnicornRegTable[AARCH64_REG_X14] = VC_REG_X14;
    capstoneToUnicornRegTable[AARCH64_REG_X15] = VC_REG_X15;
    capstoneToUnicornRegTable[AARCH64_REG_X16] = VC_REG_X16;
    capstoneToUnicornRegTable[AARCH64_REG_X17] = VC_REG_X17;
    capstoneToUnicornRegTable[AARCH64_REG_X18] = VC_REG_X18;
    capstoneToUnicornRegTable[AARCH64_REG_X19] = VC_REG_X19;
    capstoneToUnicornRegTable[AARCH64_REG_X20] = VC_REG_X20;
    capstoneToUnicornRegTable[AARCH64_REG_X21] = VC_REG_X21;
    capstoneToUnicornRegTable[AARCH64_REG_X22] = VC_REG_X22;
    capstoneToUnicornRegTable[AARCH64_REG_X23] = VC_REG_X23;
    capstoneToUnicornRegTable[AARCH64_REG_X24] = VC_REG_X24;
    capstoneToUnicornRegTable[AARCH64_REG_X25] = VC_REG_X25;
    capstoneToUnicornRegTable[AARCH64_REG_X26] = VC_REG_X26;
    capstoneToUnicornRegTable[AARCH64_REG_X27] = VC_REG_X27;
    capstoneToUnicornRegTable[AARCH64_REG_X28] = VC_REG_X28;
    capstoneToUnicornRegTable[AARCH64_REG_X29] = VC_REG_X29;
    // W0-W30
    capstoneToUnicornRegTable[AARCH64_REG_W0]  = VC_REG_W0;
    capstoneToUnicornRegTable[AARCH64_REG_W1]  = VC_REG_W1;
    capstoneToUnicornRegTable[AARCH64_REG_W2]  = VC_REG_W2;
    capstoneToUnicornRegTable[AARCH64_REG_W3]  = VC_REG_W3;
    capstoneToUnicornRegTable[AARCH64_REG_W4]  = VC_REG_W4;
    capstoneToUnicornRegTable[AARCH64_REG_W5]  = VC_REG_W5;
    capstoneToUnicornRegTable[AARCH64_REG_W6]  = VC_REG_W6;
    capstoneToUnicornRegTable[AARCH64_REG_W7]  = VC_REG_W7;
    capstoneToUnicornRegTable[AARCH64_REG_W8]  = VC_REG_W8;
    capstoneToUnicornRegTable[AARCH64_REG_W9]  = VC_REG_W9;
    capstoneToUnicornRegTable[AARCH64_REG_W10] = VC_REG_W10;
    capstoneToUnicornRegTable[AARCH64_REG_W11] = VC_REG_W11;
    capstoneToUnicornRegTable[AARCH64_REG_W12] = VC_REG_W12;
    capstoneToUnicornRegTable[AARCH64_REG_W13] = VC_REG_W13;
    capstoneToUnicornRegTable[AARCH64_REG_W14] = VC_REG_W14;
    capstoneToUnicornRegTable[AARCH64_REG_W15] = VC_REG_W15;
    capstoneToUnicornRegTable[AARCH64_REG_W16] = VC_REG_W16;
    capstoneToUnicornRegTable[AARCH64_REG_W17] = VC_REG_W17;
    capstoneToUnicornRegTable[AARCH64_REG_W18] = VC_REG_W18;
    capstoneToUnicornRegTable[AARCH64_REG_W19] = VC_REG_W19;
    capstoneToUnicornRegTable[AARCH64_REG_W20] = VC_REG_W20;
    capstoneToUnicornRegTable[AARCH64_REG_W21] = VC_REG_W21;
    capstoneToUnicornRegTable[AARCH64_REG_W22] = VC_REG_W22;
    capstoneToUnicornRegTable[AARCH64_REG_W23] = VC_REG_W23;
    capstoneToUnicornRegTable[AARCH64_REG_W24] = VC_REG_W24;
    capstoneToUnicornRegTable[AARCH64_REG_W25] = VC_REG_W25;
    capstoneToUnicornRegTable[AARCH64_REG_W26] = VC_REG_W26;
    capstoneToUnicornRegTable[AARCH64_REG_W27] = VC_REG_W27;
    capstoneToUnicornRegTable[AARCH64_REG_W28] = VC_REG_W28;
    capstoneToUnicornRegTable[AARCH64_REG_W29] = VC_REG_W29;
    capstoneToUnicornRegTable[AARCH64_REG_W30] = VC_REG_W30;
    // Q0-Q31
    capstoneToUnicornRegTable[AARCH64_REG_Q0]  = VC_REG_Q0;
    capstoneToUnicornRegTable[AARCH64_REG_Q1]  = VC_REG_Q1;
    capstoneToUnicornRegTable[AARCH64_REG_Q2]  = VC_REG_Q2;
    capstoneToUnicornRegTable[AARCH64_REG_Q3]  = VC_REG_Q3;
    capstoneToUnicornRegTable[AARCH64_REG_Q4]  = VC_REG_Q4;
    capstoneToUnicornRegTable[AARCH64_REG_Q5]  = VC_REG_Q5;
    capstoneToUnicornRegTable[AARCH64_REG_Q6]  = VC_REG_Q6;
    capstoneToUnicornRegTable[AARCH64_REG_Q7]  = VC_REG_Q7;
    capstoneToUnicornRegTable[AARCH64_REG_Q8]  = VC_REG_Q8;
    capstoneToUnicornRegTable[AARCH64_REG_Q9]  = VC_REG_Q9;
    capstoneToUnicornRegTable[AARCH64_REG_Q10] = VC_REG_Q10;
    capstoneToUnicornRegTable[AARCH64_REG_Q11] = VC_REG_Q11;
    capstoneToUnicornRegTable[AARCH64_REG_Q12] = VC_REG_Q12;
    capstoneToUnicornRegTable[AARCH64_REG_Q13] = VC_REG_Q13;
    capstoneToUnicornRegTable[AARCH64_REG_Q14] = VC_REG_Q14;
    capstoneToUnicornRegTable[AARCH64_REG_Q15] = VC_REG_Q15;
    capstoneToUnicornRegTable[AARCH64_REG_Q16] = VC_REG_Q16;
    capstoneToUnicornRegTable[AARCH64_REG_Q17] = VC_REG_Q17;
    capstoneToUnicornRegTable[AARCH64_REG_Q18] = VC_REG_Q18;
    capstoneToUnicornRegTable[AARCH64_REG_Q19] = VC_REG_Q19;
    capstoneToUnicornRegTable[AARCH64_REG_Q20] = VC_REG_Q20;
    capstoneToUnicornRegTable[AARCH64_REG_Q21] = VC_REG_Q21;
    capstoneToUnicornRegTable[AARCH64_REG_Q22] = VC_REG_Q22;
    capstoneToUnicornRegTable[AARCH64_REG_Q23] = VC_REG_Q23;
    capstoneToUnicornRegTable[AARCH64_REG_Q24] = VC_REG_Q24;
    capstoneToUnicornRegTable[AARCH64_REG_Q25] = VC_REG_Q25;
    capstoneToUnicornRegTable[AARCH64_REG_Q26] = VC_REG_Q26;
    capstoneToUnicornRegTable[AARCH64_REG_Q27] = VC_REG_Q27;
    capstoneToUnicornRegTable[AARCH64_REG_Q28] = VC_REG_Q28;
    capstoneToUnicornRegTable[AARCH64_REG_Q29] = VC_REG_Q29;
    capstoneToUnicornRegTable[AARCH64_REG_Q30] = VC_REG_Q30;
    capstoneToUnicornRegTable[AARCH64_REG_Q31] = VC_REG_Q31;

    regTableInitialized = true;
}

// Capstone RegId → Unicorn regId: O(1) 数组直接索引
int mapToUnicornReg(uint16_t reg_id){
    if (__builtin_expect(!regTableInitialized, false)) initRegTable();
    if (reg_id >= AARCH64_REG_ENDING) return -1;
    return capstoneToUnicornRegTable[reg_id];
}


void appendHex(StringBuilder* builder, const char* hex, int hexLen, int width, char placeholder, bool reverse){
    int pad = width - hexLen;
    if (pad < 0) pad = 0;
    if (pad > 0) {
        char padBuf[32];
        int padLen = pad < 32 ? pad : 32;
        memset(padBuf, placeholder, padLen);
        if (reverse) {
            appendStringN(builder, hex, hexLen);
            appendStringN(builder, padBuf, padLen);
        } else {
            appendStringN(builder, padBuf, padLen);
            appendStringN(builder, hex, hexLen);
        }
    } else {
        appendStringN(builder, hex, hexLen);
    }
}
void appendHex(StringBuilder* builder, const char* hex, int width, char placeholder, bool reverse){
    appendHex(builder, hex, (int)strlen(hex), width, placeholder, reverse);
}

void appendHex(StringBuilder* builder, uint64_t value, int width, char placeholder, bool reverse){
    appendStringN(builder,"0x",2);
    char hex[17];
    uint64ToHex(value, hex);
    appendHex(builder,hex,width,placeholder,reverse);
}

void assembleDetail(vm_context* uc,cs_insn* insn,StringBuilder* sb,uint64_t address,bool isThumb,bool current,int libraryMaxNameLength
        ,char* soName,int soNameLen,uint64_t soBase,uint32_t moduleSize){

    char space = current ? '*' : ' ';
    //so 基址 offset address
    if(soName!= nullptr && soBase!=0){
        appendChar(sb,'[');
        appendHex(sb, soName, soNameLen, libraryMaxNameLength, ' ', true);
        appendChar(sb, space);
        appendHex(sb, address - soBase+ (isThumb ? 1 : 0), 8, '0', false);
        appendChar(sb,']');
        appendChar(sb, space);
    } else if (insn->id != AARCH64_INS_SVC){
        appendChar(sb,'[');
        appendHex(sb, "Arm64Svc", libraryMaxNameLength, ' ', true);
        appendChar(sb, space);
        appendHex(sb,address-soBase, libraryMaxNameLength,'0', false);
        appendChar(sb,']');
    }
    appendChar(sb,'[');
    char opcodeHex[9];
    uint32ToHex8(TO_LITTLE_ENDIAN(*((uint32_t*)insn->bytes)), opcodeHex);
    appendStringN(sb, opcodeHex, 8);
    appendChar(sb,']');
    appendChar(sb, space);
    appendHex(sb,address,10, '0', false);
    appendChar(sb,':');
    appendChar(sb, space);
    appendChar(sb,'"');
    // 直接追加 mnemonic 和 op_str，避免 sprintf 中转
    appendString(sb, insn->mnemonic);
    appendChar(sb, ' ');
    appendString(sb, insn->op_str);
    appendChar(sb,'"');

}
void assembleDetail(vm_context* uc,cs_insn* insn,StringBuilder* builder, uint64_t address,bool isThumb,int libraryNameLength
                    ,char* soName,int soNameLen,uint64_t soBase,uint32_t moduleSize){
    return assembleDetail(uc,insn,builder,address, isThumb, false,libraryNameLength,soName,soNameLen,soBase,moduleSize);
}
