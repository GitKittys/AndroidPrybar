//
// Created by ASUS on 2025-12-02.
//

#include "ARM64Emulator.h"
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include "MemPrinter.h"
#include "RegAccessPrinter.h"
#include "SafeMemRead.h"
#include "logging.h"

std::string hexDump(uint64_t addr, const uint8_t* buf, size_t len);
static void appendHexDump(StringBuilder* sb, uint64_t addr, const uint8_t* buf, size_t len);

// 单个寄存器的位宽 → 字节数。capstone 的寄存器枚举是连续区间，直接区间判定。
static inline size_t aarch64_reg_width(unsigned int reg)
{
    if (reg >= AARCH64_REG_B0 && reg <= AARCH64_REG_B31) return 1;
    if (reg >= AARCH64_REG_H0 && reg <= AARCH64_REG_H31) return 2;
    if (reg >= AARCH64_REG_S0 && reg <= AARCH64_REG_S31) return 4;
    if (reg >= AARCH64_REG_D0 && reg <= AARCH64_REG_D31) return 8;
    if (reg >= AARCH64_REG_Q0 && reg <= AARCH64_REG_Q31) return 16;
    if (reg >= AARCH64_REG_W0 && reg <= AARCH64_REG_W30) return 4;
    if (reg >= AARCH64_REG_X0 && reg <= AARCH64_REG_X28) return 8;
    if (reg == AARCH64_REG_WZR || reg == AARCH64_REG_WSP) return 4;
    if (reg == AARCH64_REG_XZR || reg == AARCH64_REG_SP ||
        reg == AARCH64_REG_FP  || reg == AARCH64_REG_LR) return 8;
    return 0;
}

// 访存字节数。判据是「被搬运的那个寄存器有多宽」，不是指令名。
// 旧实现对 LDR/STR 一律返回 8（源码里留着 "// W寄存器 ?" 的疑问），
// 于是 `str w0, [sp,#0xc]` 这种 4 字节访问被当成 8 字节，后果有两个：
//   1) [MEM ... size=N] 和 hexdump 长度直接打错；
//   2) EastTrace 里 `pa.size >= 8` 的字符串探测门槛被误开，去嗅那 4 字节
//      后面的栈残留，判定随残留内容漂移 —— 换个优化等级输出就变。
static size_t arm64_get_mem_access_size(const cs_insn *insn, const cs_aarch64_op &op)
{
    (void)op;   // 宽度由被传输寄存器决定，与内存操作数本身无关
    // 这几条的访存宽度由指令名定死，和目标寄存器宽度无关
    // （ldrb 的目标是 W 寄存器，但只访存 1 字节）
    switch (insn->id) {
        case ARM64_INS_LDRB:  case ARM64_INS_STRB:
        case ARM64_INS_LDURB: case ARM64_INS_STURB:
        case ARM64_INS_LDRSB: case ARM64_INS_LDURSB:
            return 1;
        case ARM64_INS_LDRH:  case ARM64_INS_STRH:
        case ARM64_INS_LDURH: case ARM64_INS_STURH:
        case ARM64_INS_LDRSH: case ARM64_INS_LDURSH:
            return 2;
        case ARM64_INS_LDRSW: case ARM64_INS_LDURSW:
            return 4;
        // ldpsw 取两个 4 字节再符号扩展进 X 寄存器：按寄存器宽度算会得 16，实际只访存 8
        case ARM64_INS_LDPSW:
            return 8;
        default:
            break;
    }

    if (!insn->detail) return 8;
    const cs_aarch64 &arm = insn->detail->aarch64;
    size_t w = 0;
    int nregs = 0;
    for (int i = 0; i < arm.op_count; i++) {
        const cs_aarch64_op &o = arm.operands[i];
        if (o.type != AARCH64_OP_REG) continue;   // 基址寄存器在 op.mem 里，不算数据寄存器
        size_t rw = aarch64_reg_width(o.reg);
        if (rw == 0) continue;
        // 取最宽的那个，不是第一个：store-exclusive 形如 `stxr w0, x1, [x2]`，
        // 第一个寄存器 W0 只是状态回写位，真正搬运的是 X1。按第一个取会判成 4 字节，
        // 比旧实现的「一律 8」还错。取最大值时 str w0 仍得 4，stxr 仍得 8。
        if (rw > w) w = rw;
        nregs++;
    }
    if (w == 0) return 8;   // 认不出来时保守按 8，行为与旧实现一致

    // LDP/STP 搬两个寄存器，总访存量翻倍
    if (nregs >= 2) {
        switch (insn->id) {
            case ARM64_INS_LDP:  case ARM64_INS_STP:
            case ARM64_INS_LDNP: case ARM64_INS_STNP:
                return w * 2;
            default:
                break;
        }
    }
    return w;
}
// identity mapping 下 guest 地址 == host 地址。
// 所有内存读取用 safe_host_read（process_vm_readv），比 uc_mem_read（softmmu）快几十倍，
// 且不会在 PROT_NONE / guard page 上触发 SIGSEGV。
bool uc_is_valid_ptr(vm_context *uc, uint64_t addr) {
    (void)uc;
    return addr != 0;
}
bool looksLikeCString(vm_context *uc, uint64_t addr, char string1[256], unsigned long i1) {
    (void)string1; (void)i1;
    return safe_looks_like_cstr(uc, addr, 3);
}
void printCString(vm_context *uc, uint64_t addr, StringBuilder *sb) {
    appendStringN(sb, "    string: \"", 13);
    if (!appendCStr(uc, sb, addr)) {
        sb->length -= 13;
        return;
    }
    appendStringN(sb, "\"\n", 2);
}

static void tryPrintRegisterAsPointer(vm_context *uc, uint64_t val, StringBuilder *sb) {
    if (val == 0 || val < 0x1000) return;

    if (safe_looks_like_cstr(uc, val, 3)) {
        printCString(uc, val, sb);
        return;
    }

    uint8_t tmp[32];
    size_t safe_len = 32;
    if (safe_host_read(val, tmp, safe_len)) {
        appendHexDump(sb, val, tmp, safe_len);
    }
}
const char* aarch64OpTypeName(aarch64_op_type type)
{
    switch (type) {
        case AARCH64_OP_INVALID:            return "AARCH64_OP_INVALID";
        case AARCH64_OP_REG:                return "AARCH64_OP_REG";
        case AARCH64_OP_IMM:                return "AARCH64_OP_IMM";
        case AARCH64_OP_MEM_REG:            return "AARCH64_OP_MEM_REG";
        case AARCH64_OP_MEM_IMM:            return "AARCH64_OP_MEM_IMM";
        case AARCH64_OP_MEM:                return "AARCH64_OP_MEM";
        case AARCH64_OP_FP:                 return "AARCH64_OP_FP";
        case AARCH64_OP_CIMM:               return "AARCH64_OP_CIMM";
        case AARCH64_OP_REG_MRS:            return "AARCH64_OP_REG_MRS";
        case AARCH64_OP_REG_MSR:            return "AARCH64_OP_REG_MSR";
        case AARCH64_OP_IMPLICIT_IMM_0:     return "AARCH64_OP_IMPLICIT_IMM_0";
        case AARCH64_OP_SVCR:               return "AARCH64_OP_SVCR";
        case AARCH64_OP_AT:                 return "AARCH64_OP_AT";
        case AARCH64_OP_DB:                 return "AARCH64_OP_DB";
        case AARCH64_OP_DC:                 return "AARCH64_OP_DC";
        case AARCH64_OP_ISB:                return "AARCH64_OP_ISB";
        case AARCH64_OP_TSB:                return "AARCH64_OP_TSB";
        case AARCH64_OP_PRFM:               return "AARCH64_OP_PRFM";
        case AARCH64_OP_SVEPRFM:            return "AARCH64_OP_SVEPRFM";
        case AARCH64_OP_RPRFM:              return "AARCH64_OP_RPRFM";
        case AARCH64_OP_PSTATEIMM0_15:      return "AARCH64_OP_PSTATEIMM0_15";
        case AARCH64_OP_PSTATEIMM0_1:       return "AARCH64_OP_PSTATEIMM0_1";
        case AARCH64_OP_PSB:                return "AARCH64_OP_PSB";
        case AARCH64_OP_BTI:                return "AARCH64_OP_BTI";
        case AARCH64_OP_SVEPREDPAT:         return "AARCH64_OP_SVEPREDPAT";
        case AARCH64_OP_SVEVECLENSPECIFIER: return "AARCH64_OP_SVEVECLENSPECIFIER";
        case AARCH64_OP_SME:                return "AARCH64_OP_SME";
        case AARCH64_OP_IMM_RANGE:          return "AARCH64_OP_IMM_RANGE";
        case AARCH64_OP_TLBI:               return "AARCH64_OP_TLBI";
        case AARCH64_OP_IC:                 return "AARCH64_OP_IC";
        case AARCH64_OP_DBNXS:              return "AARCH64_OP_DBNXS";
        case AARCH64_OP_EXACTFPIMM:         return "AARCH64_OP_EXACTFPIMM";
        case AARCH64_OP_SYSREG:             return "AARCH64_OP_SYSREG";
        case AARCH64_OP_SYSIMM:             return "AARCH64_OP_SYSIMM";
        case AARCH64_OP_SYSALIAS:           return "AARCH64_OP_SYSALIAS";
        case AARCH64_OP_PRED:               return "AARCH64_OP_PRED";
        default:                            return "UNKNOWN_AARCH64_OP_TYPE";
    }
}
void appendMemoryHexdump(vm_context *uc, cs_insn *insn, StringBuilder *builder)
{
    if (!insn || !insn->detail)
        return;

    cs_detail *detail = insn->detail;
    cs_aarch64 &arm = detail->aarch64;

    // ----------- 新增：寻找写入源寄存器（store source） ----------
    int src_reg_cs = -1;
    for (int i = 0; i < arm.op_count; i++) {
        const cs_aarch64_op &op = arm.operands[i];
        if (op.type == ARM64_OP_REG && (op.access & CS_AC_READ)) {
            src_reg_cs = op.reg;
            break;
        }
    }

    uint64_t src_val = 0;
    if (src_reg_cs != -1) {
        int ureg = mapToUnicornReg(src_reg_cs);
        if (ureg >= 0) vc_reg_read_fast(uc, (vc_reg)ureg, &src_val);
    }

    // --------------------------------------------------------------
    /*
     * 这里是在循环读取指令的每个操作数，去判断类型是否是ARM64_OP_MEM
     */
    // 打印 operand

    bool is_post_index = false;

    for (int i = 0; i < arm.op_count; i++) {
        const cs_aarch64_op &op = arm.operands[i];

        if (op.type != AARCH64_OP_MEM)
            continue;



        bool is_post_index = false;
        int64_t off_addr = op.mem.disp;  // 默认用 mem.disp
        int64_t off_wb   = 0;            // 用于写回

        //writeback == true 且后面紧跟一个 IMM → 就是 post-index 否则（writeback == true 但没有后一个 IMM）就是 pre-index ✅
        if (detail->writeback) {
            if (arm.post_index) {
                //如果是post_index ，那么dump的地址应该是[reg]
                // post-index: [x1], #8
                is_post_index = true;
                off_addr = op.mem.disp;           // 一般是 0
                off_wb   = arm.operands[i + 1].imm;
//                LOGD("post-index")
            } else {
                //如果是post_index ，那么dump的地址应该是[reg+disp]
                // pre-index: [x1, #8]!
                off_addr = op.mem.disp;
                off_wb   = op.mem.disp;
//                LOGD("pre-index");

            }
        }
//        LOGD("opstr:%s %s",insn->mnemonic,insn->op_str);
        // 计算目标地址
        uint64_t base = 0;
        if (op.mem.base != ARM64_REG_INVALID) {
            int ureg = mapToUnicornReg(op.mem.base);
            if (ureg < 0 || vc_reg_read_fast(uc, (vc_reg)ureg, &base) != VC_ERR_OK)
                continue;
        }

        uint64_t index = 0;
        if (op.mem.index != ARM64_REG_INVALID) {
            int ireg = mapToUnicornReg(op.mem.index);
            if (ireg >= 0) vc_reg_read_fast(uc, (vc_reg)ireg, &index);
        }

//        LOGD("base:%lx index:%lx disp:%x",base,index,op.mem.disp)
        uint64_t addr;
        if (op.mem.base == ARM64_REG_INVALID && op.mem.index == ARM64_REG_INVALID) {
            // PC 相对 / literal：disp 是 int32_t，bit31=1 时会被符号扩展导致地址偏低 4GiB
            addr = (insn->address & ~0xFFFFFFFFULL) | (uint64_t)(uint32_t)(op.mem.disp);
        } else {
            addr = base + index + off_addr;
        }
        size_t size = arm64_get_mem_access_size(insn, op);
        if (size == 0) size = 8;

        if (addr == 0) continue;

        const char *rw = (op.access & CS_AC_WRITE) ? "WRITE" :
                         (op.access & CS_AC_READ)  ? "READ" : "ACCESS";

        appendStringN(builder, "\n    [MEM ", 10);
        appendString(builder, rw);
        appendStringN(builder, " @ 0x", 5);
        char addrHex[17];
        int addrHexLen = uint64ToHex(addr, addrHex);
        appendStringN(builder, addrHex, addrHexLen);
        appendStringN(builder, ", size=", 7);
        char sizeStr[21];
        int sizeStrLen = uint64ToDec(size, sizeStr);
        appendStringN(builder, sizeStr, sizeStrLen);
        appendStringN(builder, "]\n", 2);

        if (src_reg_cs != -1 && (op.access & CS_AC_WRITE)) {
            const char* reg_name = cs_reg_name(src_reg_cs);

            appendStringN(builder, "    value from ", 15);
            appendString(builder, reg_name ? reg_name : "?");
            appendStringN(builder, " = 0x", 5);
            char valHex[17];
            int valHexLen = uint64ToHex(src_val, valHex);
            appendStringN(builder, valHex, valHexLen);
            appendChar(builder, '\n');

            if (src_val != 0) {
                if (safe_looks_like_cstr(uc, src_val, 3)) {
                    appendStringN(builder, "    ascii: \"", 12);
                    appendCStr(uc, builder, src_val);
                    appendStringN(builder, "\"\n\n", 3);
                } else {
                    appendStringN(builder, "    pointed data:\n", 18);
                    uint8_t tmpbuf[32];
                    if (safe_host_read(src_val, tmpbuf, 32)) {
                        appendHexDump(builder, src_val, tmpbuf, 32);
                    }
                }
            }
        }
        size_t safe_sz = size;
        if (safe_sz > 64) safe_sz = 64;
        uint8_t membuf[64];
        if (safe_sz > 0 && safe_host_read(addr, membuf, safe_sz)) {
            appendHexDump(builder, addr, membuf, safe_sz);
        }
    }
}
// 直接写入 StringBuilder，避免 std::string 堆分配
static void appendHexDump(StringBuilder* sb, uint64_t addr, const uint8_t* buf, size_t len) {
    char line[80];
    for (size_t i = 0; i < len; i += 16) {
        uint64ToHexPadded(addr + i, line, 16);
        line[16] = ':';
        line[17] = ' ';
        int pos = 18;
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                byteToHex(buf[i + j], line + pos);
                line[pos + 2] = ' ';
            } else {
                line[pos] = ' '; line[pos+1] = ' '; line[pos+2] = ' ';
            }
            pos += 3;
        }
        line[pos++] = ' ';
        line[pos++] = '|';
        appendStringN(sb, line, pos);
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            char c = (char)buf[i + j];
            appendChar(sb, (c >= 32 && c <= 126) ? c : '.');
        }
        appendStringN(sb, "|\n", 2);
    }
}

std::string hexDump(uint64_t addr, const uint8_t* buf, size_t len) {
    std::string out;
    out.reserve(len * 5);
    char line[80];

    for (size_t i = 0; i < len; i += 16) {
        uint64ToHexPadded(addr + i, line, 16);
        line[16] = ':';
        line[17] = ' ';
        int pos = 18;

        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                byteToHex(buf[i + j], line + pos);
                line[pos + 2] = ' ';
            } else {
                line[pos] = ' '; line[pos+1] = ' '; line[pos+2] = ' ';
            }
            pos += 3;
        }

        line[pos++] = ' ';
        line[pos++] = '|';
        out.append(line, pos);

        for (size_t j = 0; j < 16 && i + j < len; j++) {
            char c = (char)buf[i + j];
            out.push_back((c >= 32 && c <= 126) ? c : '.');
        }

        out.append("|\n", 2);
    }
    return out;
}

void appendMemoryHexdump(
        vm_context *uc,
        PendingAccess *pendingAccess,
        const uint64_t* cachedXRegs)
{

    cs_insn  *insn = pendingAccess->insn;
    if (!insn || !insn->detail) return;
    cs_detail *detail = insn->detail;
    cs_aarch64 &arm = detail->aarch64;
    for (int i = 0; i < arm.op_count; i++) {
        const cs_aarch64_op &op = arm.operands[i];
        if (op.type != AARCH64_OP_MEM)
            continue;
        if(op.mem.disp ==0 && op.mem.index ==0 && op.mem.base){
            return;
        }
        int64_t off_addr = op.mem.disp;
        if (detail->writeback) {
            if (arm.post_index) {
                off_addr = 0;
            } else {
                off_addr = op.mem.disp;
            }
        }

        uint64_t base = 0;
        if (op.mem.base != ARM64_REG_INVALID) {
            int ureg = mapToUnicornReg(op.mem.base);
            if (ureg < 0) continue;
            int idx = cachedXRegs ? cachedXRegIndex(ureg) : -1;
            if (idx >= 0) {
                base = cachedXRegs[idx];
            } else if (vc_reg_read_fast(uc, (vc_reg)ureg, &base) != VC_ERR_OK) {
                continue;
            }
        }

        uint64_t index = 0;
        if (op.mem.index != ARM64_REG_INVALID) {
            int ireg = mapToUnicornReg(op.mem.index);
            if (ireg >= 0) {
                int idx = cachedXRegs ? cachedXRegIndex(ireg) : -1;
                if (idx >= 0) {
                    index = cachedXRegs[idx];
                } else {
                    vc_reg_read_fast(uc, (vc_reg)ireg, &index);
                }
            }
        }
        uint64_t addr = 0;
        size_t size = 0;
        if(op.mem.index ==0 && op.mem.base ==0){
            addr = (insn->address & ~0xFFFFFFFFULL) | (uint64_t)(uint32_t)(op.mem.disp);
            size = arm64_get_mem_access_size(insn, op);
        } else{
            addr = base + index + off_addr;
            size = arm64_get_mem_access_size(insn, op);
        }
//        LOGD("base:%lx index=%lx addr=%lx disp:%lx",base,index,addr,op.mem.disp);

//        pendingAccess->last_mem_address = addr;
//        pendingAccess->size = size;
//        pendingAccess->is_load = op.access == CS_AC_READ;
//        pendingAccess->insn = insn;
        pendingAccess->last_mem_address = addr;
        pendingAccess->size = size;
        if(op.access == CS_AC_WRITE){
//            pendingAccess->last_mem_address = addr;
//            pendingAccess->size = size;
            if(op.mem.base == AARCH64_REG_X29){
//                char buf[64];
//                snprintf(buf,sizeof buf,"[STACK] fp%x", op.mem.disp);
                pendingAccess->last_mem_address = 0;
                return;
            }

            pendingAccess->is_load = false;
            return;
        } else{
            pendingAccess->is_load = true;
        }

//        LOGD("buf:%s", pendingAccess->string.c_str());
//        if(op.access & CS_AC_WRITE) {
//            if(pendingAccess->last_mem_address == 0){
//                //如果是写入目标地址，则下一条指令再打印
//                pendingAccess->is_load = true;
//                pendingAccess->last_mem_address = addr;
//                return;
//            } else{
//                addr = pendingAccess->last_mem_address;
//            }
//        }



    }
}