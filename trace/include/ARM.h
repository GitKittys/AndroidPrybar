//
// Created by ASUS on 2025-01-20.
//

#ifndef UNICRONTEST_ARM_H
#define UNICRONTEST_ARM_H
#include "ARM64Emulator.h"
#include "capstone.h"
#include "StringUtil.h"

//字节序转换,用宏定义的原因是因为，如果重复调用函数，入栈出栈速度慢
#define TO_LITTLE_ENDIAN(value) ( \
    (((value) & 0xFF000000) >> 24) | \
    (((value) & 0x00FF0000) >> 8)  | \
    (((value) & 0x0000FF00) << 8)  | \
    (((value) & 0x000000FF) << 24) \
)

int mapToUnicornReg(uint16_t reg_id);
void appendHex(StringBuilder* builder, const char* hex, int hexLen, int width, char placeholder, bool reverse);
void appendHex(StringBuilder* builder, const char* hex, int width, char placeholder, bool reverse);
void appendHex(StringBuilder* builder, long value, int width, char placeholder, bool reverse);
void assembleDetail(vm_context* uc,cs_insn* insn,StringBuilder* sb,uint64_t address,bool isThumb,bool current,int libraryMaxNameLength
        ,char* soName,int soNameLen,uint64_t soBase,uint32_t moduleSize);
void assembleDetail(vm_context* uc,cs_insn* insn,StringBuilder* builder, uint64_t address,bool isThumb,int libraryNameLength
        ,char* soName,int soNameLen,uint64_t soBase,uint32_t moduleSize);

#endif //UNICRONTEST_ARM_H
