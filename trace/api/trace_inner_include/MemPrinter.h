//
// Created by ASUS on 2025-12-02.
//

#ifndef UNICRONTEST_MEMPRINTER_H
#define UNICRONTEST_MEMPRINTER_H
#include "ARM64Emulator.h"
#include "arm64.h"
#include "StringUtil.h"
#include "capstone.h"
#include "hexdump.h"
#include "Disassembly.h"
#include "ARM.h"

/**
 * 如果是load指令，直接输出load的地址的hexdump,顺便判断一下那块地址是否是字符串，做个额外输出
 * 如果是store 指令，输出内存值被改变前后的值
 * 参考格式
 * [MEM-WRITE] STP X0, X1, [X2] @ PC=0x400130

  Registers:
    X0 = 0x0000004000801234
    X1 = 0x0000000000000045
    X2 = 0x00007ffd12345000

  Target Memory:
    address = 0x7ffd12345000
    size    = 16 bytes

  Before:
    7ffd12345000: AA AA AA AA AA AA AA AA
    7ffd12345008: BB BB BB BB BB BB BB BB

  After:
    7ffd12345000: 34 12 08 00 40 00 00 00   ← X0
    7ffd12345008: 45 00 00 00 00 00 00 00   ← X1

  Optional Pointer Dump:
    X0 -> 0x4000801234
      4000801234: 48 65 6C 6C 6F 00 ...
      或者 。
      [MW] STP x29,x30,[sp,#0xb0] pc=0x7a6e411844
    sp=0x7fcd267970 x29=0x7fcd267fa0 x30=0x7a6e411e5c

    mem[sp+0xb0]:
      7fcd267a50: 34 12 08 00 40 00 00 00 |4...@...
      7fcd267a58: 45 00 00 00 00 00 00 00 |E.......

    reg[x29]:
      7fcd267fa0: 48 65 6C 6C 6F 00 00 00 |Hello...

    reg[x30]:
      7a6e411e5c: F0 5F BD A9 F3 03 00 AA |._......
 */
/*
 * 🧩 AArch64 LDR 指令常见寻址模式

    AArch64 load/store 共有多种 addressing mode：

    模式	示例	含义
    Unsigned offset	ldr x0, [x1, #0x20]	base + imm
    Register offset	ldr x0, [x1, x2]	base + index
    Scaled register offset	ldr x0, [x1, x2, lsl #3]	base + index * scale
    Pre-index	ldr x0, [x1, #8]!	base += imm; load from base
    Post-index	ldr x0, [x1], #8	load from base; base += imm
    Literal	ldr x0, =label	PC + offset（不使用 base/index）

    base就是寄存器的地址+imm立即数
    index就是另一个寄存器 base + reg的值
    scale是左移或者右移动
    detail结构体writeback = true 等于ldr x0, [x1, #8]!
    模式	MEM.disp	IMM operand	writeback	示例

    Pre-index	disp = imm	❌ 没有 IMM operand	✔ true	str x2, [x3, #-16]!
    Post-index	disp = 0	✔ 有 IMM operand	✔ true	str x2, [x3], #-16

 */
typedef struct PendingAccess {
    int is_load;        // 1 = load, 0 = store

    uint64_t addr;      // target address
    size_t size;        // mem size
    uint64_t last_mem_address; // 上一条需要dump的地址
    cs_insn* insn;
} PendingAccess;


void processMemoryAccess_before(vm_context *uc, cs_insn *insn, StringBuilder *sb);
void processMemoryAccess_after(vm_context *uc, StringBuilder *sb);
void appendMemoryHexdump(vm_context *uc, PendingAccess* pendingAccess, const uint64_t* cachedXRegs = nullptr);
void appendMemoryHexdump(vm_context *uc, cs_insn *insn, StringBuilder *builder);

#endif //UNICRONTEST_MEMPRINTER_H
