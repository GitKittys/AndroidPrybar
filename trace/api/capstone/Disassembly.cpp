//
// Created by Administrator on 2024/12/10.
//
//#include "capstone.h"
#include "ARM64Emulator.h"
#include "Disassembly.h"
#include "logging.h"
#include <cstring>   // vc_disasm 用 memcpy/strncpy

static thread_local csh handle; //用线程局部变量的原因是，capstone不支持多线程操作


const char * cs_reg_name(int cs_reg_id){
    return cs_reg_name(handle,cs_reg_id);
}

cs_err cs_regs_access(const cs_insn *insn,
                      cs_regs regs_read, uint8_t *regs_read_count,
                      cs_regs regs_write, uint8_t *regs_write_count){
    return cs_regs_access(handle,insn,regs_read,regs_read_count,regs_write,regs_write_count);
}

bool cs_disasm_iter_wrapper(const uint8_t *code, size_t size, uint64_t address, cs_insn **insn){
    // 检查 AArch64 是否支持
    if(handle == 0){
//        LOGD("handle:%lx",handle );
        cs_err err = cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &handle);
//        LOGD("err:%s handle:%lx",cs_strerror(err),handle );
        if(err == CS_ERR_OK){
            cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON); // 开启详细信息
        }else{
            return false;
        }
    }
    *insn = cs_malloc(handle);
    bool result = cs_disasm_iter(handle, &code, &size, &address, *insn);
//    LOGD("Disassembled: 0x%" PRIx64 "\n", (*insn)->address);

    return result;
}

//cs_err cs_close_thread_local(){
//    return cs_close(&handle);
//}

// vc_disasm —— 反汇编指定地址的 ARM64 指令（公开工具 API）。
// 从 VCPU(ExternalCpuEventSupport.cpp) 迁到 trace 层：反汇编是 trace/工具功能，
// 不属于 VM 执行核心，capstone 是 trace 侧依赖，VCPU 运行时用不到。
// 自包含：只读 addr 处内存 + capstone，无任何 VCPU 内部状态。
__attribute__((visibility("default")))
int vc_disasm(uint64_t addr, int max_count, vc_insn* out) {
    if (addr == 0 || addr < 0x1000 || max_count <= 0 || out == nullptr) return 0;

    csh h = 0;
    if (cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &h) != CS_ERR_OK) return 0;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_OFF);

    const uint8_t* code = (const uint8_t*)addr;
    size_t code_size = (size_t)max_count * 4;
    uint64_t pc = addr;
    cs_insn* insn = cs_malloc(h);
    int n = 0;

    while (n < max_count && cs_disasm_iter(h, &code, &code_size, &pc, insn)) {
        vc_insn* o = &out[n];
        o->address = insn->address;
        o->size = (uint8_t)insn->size;
        memcpy(&o->bytes, (const void*)insn->address, 4);
        strncpy(o->mnemonic, insn->mnemonic, sizeof(o->mnemonic) - 1);
        o->mnemonic[sizeof(o->mnemonic) - 1] = '\0';
        strncpy(o->op_str, insn->op_str, sizeof(o->op_str) - 1);
        o->op_str[sizeof(o->op_str) - 1] = '\0';
        n++;
    }

    cs_free(insn, 1);
    cs_close(&h);
    return n;
}
