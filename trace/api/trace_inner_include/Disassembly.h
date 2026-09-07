//
// Created by ASUS on 2025-01-22.
//

#ifndef UNICRONTEST_DISASSEMBLY_H
#define UNICRONTEST_DISASSEMBLY_H
#include "capstone.h"
cs_err cs_regs_access(const cs_insn *insn,
                      cs_regs regs_read, uint8_t *regs_read_count,
                      cs_regs regs_write, uint8_t *regs_write_count);
const char * cs_reg_name(int cs_reg_id);
bool cs_disasm_iter_wrapper(const uint8_t *code, size_t size, uint64_t address, cs_insn **insn);

#endif //UNICRONTEST_DISASSEMBLY_H
