//
// Created by Administrator on 2024/12/9.
//

#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <unordered_map>
#include "ARM64Emulator.h"
#include "logging.h"
#include <jni.h>
#include "Utils.h"
#include "LibraryUtils.h"
#include "ARM.h"
#include "Disassembly.h"
#include <capstone.h>
#include "RegAccessPrinter.h"
#include "JniTrace.h"
#include "dlfcn_nougat.h"
#include <dlfcn.h>
#include <algorithm>
#include <vector>
#include "dobby.h"
#include "svc_handler.h"
#include <sys/mman.h>
#include "MemPrinter.h"
#include "SafeMemRead.h"
#include "FuncCallTrace.h"
#include "SymbolTable.h"
#include <signal.h>
#include <cstdlib>
#include <cxxabi.h>
#include <sys/stat.h>

#define Export_Symbol __attribute__((visibility("default")))

// C++ symbol demangling helper（带缓存）
// key 是 lookupSymbolExact 返回的符号名指针 —— 指向 SymbolTable entries 的 std::string
// 内部存储，进程生命周期内稳定（除非 clearSymbolCache），可安全作为 map key。
// 缓存避免内部 PLT 调用循环里每次都 malloc+demangle+free。
static std::unordered_map<const char*, std::string> s_east_demangle_cache;
static int s_east_demangle_lock = 0;

static const std::string& demangleSymbol(const char* mangled) {
    static const std::string empty;
    if (!mangled) return empty;
    while (__sync_lock_test_and_set(&s_east_demangle_lock, 1)) { __builtin_arm_wfe(); }
    auto it = s_east_demangle_cache.find(mangled);
    if (it != s_east_demangle_cache.end()) {
        const std::string& ref = it->second;
        __sync_lock_release(&s_east_demangle_lock);
        return ref;
    }
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    const std::string& ref = (status == 0 && demangled)
        ? (s_east_demangle_cache[mangled] = std::string(demangled))
        : (s_east_demangle_cache[mangled] = std::string(mangled)); // fallback to mangled name
    if (demangled) free(demangled);
    __sync_lock_release(&s_east_demangle_lock);
    return ref;
}
// 定义跳转指令的 ID 列表（ARM64 架构）
#define IS_ARM64_JUMP(id) ( \
    (id) == AARCH64_INS_B    || (id) == AARCH64_INS_BL   || \
    (id) == AARCH64_INS_BR   || (id) == AARCH64_INS_BLR  || \
    (id) == AARCH64_INS_RET  || (id) == AARCH64_INS_CBZ  || \
    (id) == AARCH64_INS_CBNZ || (id) == AARCH64_INS_TBZ  || \
    (id) == AARCH64_INS_TBNZ \
)
//JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
//    LOGD("jni_load")
//    javaVm = vm;
//    JNIEnv * env = nullptr;
//    // 获取 JNI 环境
//    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
//        LOGE("Failed to get JNI environment");
//        return JNI_ERR;
//    }
//    return JNI_VERSION_1_6;
//
//
//}

// ---- 反汇编缓存 (4-way set-associative, pseudo-LRU) ----
// 设计思路:
//   - 模拟 CPU L1/L2 缓存结构，16K 组 × 4 路 = 64K 条目上限
//   - 每组 4 个 Entry 恰好 64 字节 = 1 个 CPU cache line，查找时一次内存读取
//   - 命中时提升到 MRU 位置 (way 0)，淘汰 LRU (way 3)
//   - hash = (addr >> 2) ^ (addr >> 18)  利用高位分散，减少循环代码冲突
//   - 内存上限: 表本身 ~2MB + cs_insn 对象 ≤ 64K × ~1.5KB ≈ 96MB
//   - 比 unordered_map 快: 单次数组访问 vs 哈希+桶链+指针追踪
//
// 附带寄存器访问缓存:
//   - cs_regs_access 结果对同一条指令是确定性的，缓存后可跳过重复调用
//   - 每条目额外存储 read/write 寄存器 ID 和数量 (~18 字节)

struct CachedRegAccess {
    uint16_t regs_read[8];
    uint16_t regs_write[8];
    uint8_t read_count;
    uint8_t write_count;
};

class DisasmCache {
    static constexpr int WAYS = 4;
    static constexpr size_t SET_BITS = 14;   // 16K 组 × 4 路 = 64K 条目，freeTrace 时 clear() 回收
    static constexpr size_t NUM_SETS = 1u << SET_BITS;
    static constexpr size_t SET_MASK = NUM_SETS - 1;

    struct Entry {
        uint64_t addr;
        cs_insn* insn;
        CachedRegAccess regAccess;
        bool regAccessCached;
        uint8_t mnemonicLen;
        uint8_t opStrLen;
    };

    struct Set {
        Entry entries[WAYS];
    };

    Set* sets;

    static inline size_t hashAddr(uint64_t addr) {
        uint64_t h = addr >> 2;  // ARM64 指令 4 字节对齐，低 2 位无信息量
        h ^= (h >> 16);         // 高位混合，减少系统性冲突
        return h & SET_MASK;
    }

public:
    DisasmCache() : sets(nullptr) {
        sets = (Set*)calloc(NUM_SETS, sizeof(Set));
    }
    ~DisasmCache() {
        if (!sets) return;
        clear();
        free(sets);
    }

    void clear() {
        if (!sets) return;
        for (size_t s = 0; s < NUM_SETS; s++)
            for (int w = 0; w < WAYS; w++) {
                if (sets[s].entries[w].insn) cs_free(sets[s].entries[w].insn, 1);
                sets[s].entries[w] = {};
            }
    }

    // 释放每组 LRU 半边 (way 2, 3)，保留 MRU 半边 (way 0, 1)
    // 效果: 立即回收约一半 cs_insn 内存，热点地址大概率在 way 0/1 不受影响
    void trimCold() {
        if (!sets) return;
        for (size_t s = 0; s < NUM_SETS; s++) {
            for (int w = 2; w < WAYS; w++) {
                if (sets[s].entries[w].insn) {
                    cs_free(sets[s].entries[w].insn, 1);
                    sets[s].entries[w] = {};
                }
            }
        }
    }

    // 查找: 命中时提升到 MRU (way 0)
    inline cs_insn* get(uint64_t addr) {
        Set& set = sets[hashAddr(addr)];
        // 快速路径: MRU 命中 (循环中绝大多数情况)
        if (__builtin_expect(set.entries[0].addr == addr && set.entries[0].insn != nullptr, 1))
            return set.entries[0].insn;
        // 检查其余 way
        for (int w = 1; w < WAYS; w++) {
            if (set.entries[w].addr == addr && set.entries[w].insn != nullptr) {
                // 提升到 MRU
                Entry tmp = set.entries[w];
                for (int j = w; j > 0; j--)
                    set.entries[j] = set.entries[j - 1];
                set.entries[0] = tmp;
                return set.entries[0].insn;
            }
        }
        return nullptr;
    }

    // 插入: 淘汰 LRU (way 3)，新条目放到 MRU (way 0)
    inline void put(uint64_t addr, cs_insn* insn) {
        Set& set = sets[hashAddr(addr)];
        // 淘汰最久未用的 (way 3)
        if (set.entries[WAYS - 1].insn)
            cs_free(set.entries[WAYS - 1].insn, 1);
        // 整体后移
        for (int j = WAYS - 1; j > 0; j--)
            set.entries[j] = set.entries[j - 1];
        // 新条目放到 MRU，同时缓存字符串长度避免每条指令 strnlen
        set.entries[0].addr = addr;
        set.entries[0].insn = insn;
        set.entries[0].regAccessCached = false;
        set.entries[0].mnemonicLen = (uint8_t)strnlen(insn->mnemonic, 32);
        set.entries[0].opStrLen = (uint8_t)strnlen(insn->op_str, 160);
    }

    // 获取缓存的 mnemonic/op_str 长度（MRU 命中时直接返回）
    inline bool getStrLens(uint64_t addr, uint8_t& mnLen, uint8_t& opLen) {
        Set& set = sets[hashAddr(addr)];
        if (set.entries[0].addr == addr && set.entries[0].insn != nullptr) {
            mnLen = set.entries[0].mnemonicLen;
            opLen = set.entries[0].opStrLen;
            return true;
        }
        return false;
    }

    // 获取缓存的寄存器访问结果，未缓存则返回 nullptr
    inline const CachedRegAccess* getRegAccess(uint64_t addr) {
        Set& set = sets[hashAddr(addr)];
        // 绝大多数情况下刚调用过 get()，MRU 就是目标
        if (set.entries[0].addr == addr && set.entries[0].regAccessCached)
            return &set.entries[0].regAccess;
        return nullptr;
    }

    // 缓存寄存器访问结果
    inline void putRegAccess(uint64_t addr, const cs_regs regs_read, uint8_t read_count,
                             const cs_regs regs_write, uint8_t write_count) {
        Set& set = sets[hashAddr(addr)];
        if (set.entries[0].addr == addr) {
            CachedRegAccess& ra = set.entries[0].regAccess;
            uint8_t rc = read_count < 8 ? read_count : 8;
            uint8_t wc = write_count < 8 ? write_count : 8;
            for (int k = 0; k < rc; k++) ra.regs_read[k] = regs_read[k];
            for (int k = 0; k < wc; k++) ra.regs_write[k] = regs_write[k];
            ra.read_count = rc;
            ra.write_count = wc;
            set.entries[0].regAccessCached = true;
        }
    }
};
static DisasmCache disassembleCache;

#include "DirectWriteBuf.h"

struct TraceInfo{
    vm_context* vmContext;
    // ---- 每线程状态（本 TraceInfo 是 per-thread clone）----
    bool     jniWrapperSkip = false;       // JNI wrapper 指令跳过中
    uint64_t jniWrapperReturnAddr = 0;     // 跳过到此返回地址
    uint64_t entryLr = 0;                  // 入口 x30 = 调用者返回地址（判函数返回刷盘）
    bool     entryLrSet = false;
    // ---- RegAccessPrinter 复用 (消除每条指令 malloc/free ~144字节) ----
    RegAccessPrinter lastWritePrinter;
    bool hasLastWritePrinter = false;

    // ---- StringBuilder 复用 (消除每条指令 malloc/free) ----
    // 生命周期跟随 TraceInfo，用 resetStringBuilder 清空内容，不释放 buffer
    StringBuilder stringBuilder;
    bool sbInitialized = false;

    // ---- PendingAccess 复用 (消除每条指令 new/delete) ----
    PendingAccess pendingAccess;
    bool hasPendingAccess = false;

    // ---- 时间戳缓存 ----
    // 每条指令都调用 gettimeofday+localtime_r+snprintf 开销很大。
    // 改为每 1024 条指令才更新一次时间字符串，中间复用缓存。
    char cachedTimeStr[20] = {};
    uint32_t timeUpdateCounter = 0;
    static constexpr uint32_t TIME_UPDATE_INTERVAL = 1024;

    // ---- 指令行号 ----
    uint64_t insnCounter = 0;

    // ---- 定期 flush ----
    uint32_t flushCounter = 0;
    static constexpr uint32_t FLUSH_INTERVAL = 100000;

    // ---- 定期内存清理 ----
    // 每 300 万条指令释放冷缓存、收缩膨胀的 buffer，降低内存峰值
    uint32_t memCleanupCounter = 0;
    static constexpr uint32_t MEM_CLEANUP_INTERVAL = 3000000;

    // ---- 直写 buffer (替代 setvbuf + fwrite) ----
    DirectWriteBuf dwBuf;
    bool dwBufInitialized = false;

    // ---- 批量寄存器缓存 ----
    // trace_code 入口一次性读取 x0-x28, x29, x30, sp, nzcv，
    // 下游 regAccessPrinterImpl 直接从此数组取值，避免逐个 uc_reg_read_fast 的函数调用开销。
    // 索引: 0-28=x0-x28, 29=x29, 30=x30, 31=sp, 32=nzcv
    static constexpr int CACHED_XREG_COUNT = 33;
    uint64_t cachedXRegs[CACHED_XREG_COUNT]{};
    const uint64_t* xregsDirectPtr = nullptr;

    // ---- FuncCallTrace 上下文 ----
    void* funcCallCtx = nullptr;
    uint64_t funcCallPendingRet = 0;   // 外部 [CALL] 待补返回值的地址(=调用指令 addr+4)，每线程

    // ---- fd 追踪 + SVC pending（[CALL] 和 [SVC] 共享） ----
    FdTracker fdTracker;
    SvcPending svcPending;

    // ---- PLT 跳过机制 ----
    uint64_t pltSkipStart = 0;
    uint64_t pltSkipEnd   = 0;

    struct memDumpPrint{
        StringBuilder *hexDumpString;
    };
    struct currentModule{
        std::string libraryName;
        std::string fullPath;
        size_t maxLengthLibraryName;
        uint64_t baseAddress;
        uint64_t endAddress;
        uint64_t loadBias = 0;
        uint64_t pltStart = 0;
        uint64_t pltEnd   = 0;
    }currentSoInfo;

    // ---- 多线程 per-thread trace 支持 ----
    std::string logDir;          // 存放 per-thread 日志的目录
    std::string origLogPath;     // 原始 logPath 参数（TCP 时为 "tcp:port"）——自动线程追踪递归调 trace() 用
    bool auto_trace_threads = false;  // 自动线程追踪开关（trace 层特性，默认关，vc_set_auto_trace_threads 开）
    bool threadHookInstalled = false; // 线程追踪 hook 是否已挂（开启时才挂，且只挂一次）
    std::string filePrefix;      // "soName+0xOFFSET" 前缀
    bool isTemplate = false;     // true = 主模板（不直接输出），false = 可输出的实例
    TraceInfo* master = nullptr; // per-thread 克隆指向主模板
    int ownedFd = -1;            // 本实例拥有的 fd（per-thread 文件）
    uint64_t targetFuncAddr = 0; // 被 trace 的目标函数地址（master 设置，用于检测新调用入口）
    bool hasWrittenData = false;  // clone 是否已写入过数据（区分首次进入 vs 重新进入）

    // ---- TCP 远程 trace ----
    int tcpClientFd = -1;         // TCP 模式下共享的 client socket fd（-1 = 本地文件模式）

    // ---- fork 检测 ----
    pid_t ownerPid = 0;           // 创建此 clone 的进程 pid，fork 后子进程检测到不匹配则重建
};
struct InstructionVisitor{
    void(*visitLast)(TraceInfo* traceInfo,vm_context * ucEngine,StringBuilder * sb,uint64_t address);
    void(*visit)(TraceInfo* traceInfo,vm_context* ucEngine,StringBuilder* builder,cs_insn* insn,uint64_t currentAddress);
};

// ============================================================
//  多线程 per-thread trace 基础设施
// ============================================================

// 当前线程活跃的 DirectWriteBuf（FuncCallTrace / JniTrace 通过此指针写入 per-thread 文件）
thread_local DirectWriteBuf* tls_trace_active_dwbuf = nullptr;

// 进程身份重校验标志（trace 层自有，默认 false）：正常每条指令【不】校验 pid（getpid 太慢）。
// 仅当某个 fork 子进程「带着父的 TLS clone 重新进入 VM 继续 trace」时，才由 VCPU 层在那个转换点
// 置位，让 getOrCreatePerThreadTrace 校验一次 pid、给子进程重建独立 clone，然后清零。
// 现状：无人置位 —— 真 fork 子进程都 escape 逃离 VM（不 trace）；vfork 子进程与父【共享】同一
// dwBuf（不重建，见 do_vfork_in_vm / vfork_child_entry，父挂起不并发、按 tid 区分）。
// 保留此机制作为未来 fork 场景的开关，同时把每指令 getpid 降为「几乎永不调用」。
thread_local bool tls_trace_recheck_pid = false;

// master TraceInfo* → 本线程的 per-thread 克隆
static thread_local std::unordered_map<TraceInfo*, TraceInfo*> tls_trace_clones;

// 全局跟踪所有 per-thread 克隆，freeTrace 时统一清理
static int g_trace_clone_lock = 0;
static std::vector<TraceInfo*> g_trace_all_clones;

static bool g_logdir_created_once = false;

static std::string computeLogDir(const char* logPath) {
    std::string path(logPath);
    size_t dot = path.rfind('.');
    size_t slash = path.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        path = path.substr(0, dot);
    }
    if (!path.empty() && path.back() != '/') path += '/';
    return path;
}

static void cleanupPerThreadTrace(TraceInfo* ti) {
    if (ti->dwBufInitialized) {
        ti->dwBuf.destroy();
        ti->dwBufInitialized = false;
    }
    if (ti->ownedFd >= 0) {
        close(ti->ownedFd);
        ti->ownedFd = -1;
    }
    if (ti->sbInitialized) {
        freeStringBuilder(&ti->stringBuilder);
        ti->sbInitialized = false;
    }
}

// 扫描文件系统找到下一个不存在的序号
static int findNextFileIndex(const char* dir, const char* prefix, pid_t tid) {
    char path[512];
    for (int idx = 0; ; idx++) {
        snprintf(path, sizeof(path), "%s%s_tid%d_%d.lz4", dir, prefix, tid, idx);
        if (access(path, F_OK) != 0) return idx;
    }
}

// 为 clone 打开一个新的 trace 文件（首次创建或切换到下一次调用）
// TCP 模式下所有线程共享 master 的 socket fd，不创建本地文件
static bool openNextTraceFile(TraceInfo* ti, TraceInfo* master, pid_t tid) {
    // 关闭旧文件
    if (ti->dwBufInitialized) {
        ti->dwBuf.flush();
        ti->dwBuf.destroy();
        ti->dwBufInitialized = false;
    }
    if (ti->ownedFd >= 0) {
        close(ti->ownedFd);
        ti->ownedFd = -1;
    }

    if (master->tcpClientFd >= 0) {
        ti->tcpClientFd = master->tcpClientFd;
        ti->dwBuf.init_tcp(master->tcpClientFd);
        ti->dwBufInitialized = true;
        ti->hasWrittenData = false;
        return true;
    }

    int idx = findNextFileIndex(master->logDir.c_str(), master->filePrefix.c_str(), tid);
    char path[512];
    snprintf(path, sizeof(path), "%s%s_tid%d_%d.lz4",
             master->logDir.c_str(), master->filePrefix.c_str(), tid, idx);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOGE("per-thread trace file open failed: %s", path);
        return false;
    }
    ti->ownedFd = fd;
    ti->dwBuf.init_lz4(fd);
    ti->dwBufInitialized = true;
    ti->hasWrittenData = false;
    // 重置 StringBuilder（新文件重新开始）
    if (ti->sbInitialized) {
        resetStringBuilder(&ti->stringBuilder);
    }
    LOGD("per-thread trace: tid=%d → %s", tid, path);
    return true;
}

static TraceInfo* getOrCreatePerThreadTrace(TraceInfo* master, uint64_t address) {
    auto it = tls_trace_clones.find(master);
    if (it != tls_trace_clones.end()) {
        TraceInfo* ti = it->second;

        // fork 检测：只在 VCPU 于 fork/vfork 转换点置了 tls_trace_recheck_pid 时才 getpid 校验，
        // 避免每条指令一次 getpid（热路径开销）。子进程继承父的 TLS clone，pid 不匹配则重建。
        bool identity_stale = false;
        if (__builtin_expect(tls_trace_recheck_pid, false)) {
            tls_trace_recheck_pid = false;
            if (ti->ownerPid != 0 && ti->ownerPid != getpid()) identity_stale = true;
        }
        if (identity_stale) {
            if (ti->dwBufInitialized) {
                ti->dwBuf.flush();
                ti->dwBuf.destroy();
            }
            if (ti->ownedFd >= 0) close(ti->ownedFd);
            delete ti;
            tls_trace_clones.erase(it);
            // 落到下面的新建逻辑
        } else {
            // 检测新调用入口：address 等于目标函数起始地址 且 clone 已经写过数据
            if (ti->hasWrittenData && master->targetFuncAddr != 0 && address == master->targetFuncAddr) {
                pid_t tid = gettid();
                if (!openNextTraceFile(ti, master, tid)) {
                    tls_trace_active_dwbuf = nullptr;
                    return master;
                }
            }
            tls_trace_active_dwbuf = ti->dwBufInitialized ? &ti->dwBuf : nullptr;
            return ti;
        }
    }

    mkdir(master->logDir.c_str(), 0755);

    pid_t tid = gettid();
    auto* ti = new TraceInfo();
    ti->vmContext = master->vmContext;
    ti->currentSoInfo = master->currentSoInfo;
    ti->master = master;

    if (!openNextTraceFile(ti, master, tid)) {
        delete ti;
        return master;
    }

    ti->ownerPid = getpid();
    tls_trace_clones[master] = ti;
    tls_trace_active_dwbuf = &ti->dwBuf;

    while (__sync_lock_test_and_set(&g_trace_clone_lock, 1)) { __builtin_arm_wfe(); }
    g_trace_all_clones.push_back(ti);
    __sync_lock_release(&g_trace_clone_lock);

    return ti;
}

//负责打印上一条改变过的寄存器
void visitLastImpl(TraceInfo* traceInfo,vm_context * ucEngine,StringBuilder * sb,uint64_t address){
    if(traceInfo->hasLastWritePrinter){
        traceInfo->lastWritePrinter.cachedXRegs = traceInfo->cachedXRegs;
        regAccessPrinterImpl(&traceInfo->lastWritePrinter, ucEngine, sb, address);
        traceInfo->hasLastWritePrinter = false;
    }
}
//输出当前的指令和读取的寄存器，写入寄存器操作在visitLastImpl输出
void  visitImpl(TraceInfo* traceInfo,vm_context* ucEngine,StringBuilder* builder,cs_insn* insn,uint64_t currentAddress){
    uint8_t regs_read_count, regs_write_count;
    const uint16_t* regs_read_ptr;
    const uint16_t* regs_write_ptr;

    // 先查寄存器访问缓存，命中则跳过 cs_regs_access 调用
    const CachedRegAccess* cached = disassembleCache.getRegAccess(currentAddress);
    cs_regs regs_read_buf, regs_write_buf;

    if (cached) {
        regs_read_ptr = cached->regs_read;
        regs_write_ptr = cached->regs_write;
        regs_read_count = cached->read_count;
        regs_write_count = cached->write_count;
    } else {
        cs_regs_access(insn, regs_read_buf, &regs_read_count, regs_write_buf, &regs_write_count);
        disassembleCache.putRegAccess(currentAddress, regs_read_buf, regs_read_count,
                                      regs_write_buf, regs_write_count);
        regs_read_ptr = regs_read_buf;
        regs_write_ptr = regs_write_buf;
    }

    if(regs_write_count || regs_read_count){
        if(regs_read_count){
            RegAccessPrinter readRegsPrint;
            readRegsPrint.forWriteRegs = false;
            readRegsPrint.regs_count = regs_read_count < 8 ? regs_read_count : 8;
            for (int ri = 0; ri < readRegsPrint.regs_count; ri++)
                readRegsPrint.accessRegs[ri] = regs_read_ptr[ri];
            readRegsPrint.address = currentAddress;
            readRegsPrint.csInsn = insn;
            readRegsPrint.uc = ucEngine;
            readRegsPrint.cachedXRegs = traceInfo->cachedXRegs;
            regAccessPrinterImpl(&readRegsPrint, ucEngine, builder, currentAddress);
        }

        if (regs_write_count) {
            RegAccessPrinter* lastPrint = &traceInfo->lastWritePrinter;
            lastPrint->uc = ucEngine;
            lastPrint->address = currentAddress + 4;
            lastPrint->forWriteRegs = true;
            lastPrint->csInsn = insn;
            lastPrint->cachedXRegs = nullptr;
            lastPrint->regs_count = regs_write_count < 8 ? regs_write_count : 8;
            for (int ri = 0; ri < lastPrint->regs_count; ri++)
                lastPrint->accessRegs[ri] = regs_write_ptr[ri];
            traceInfo->hasLastWritePrinter = true;
        }
    }

}

/**
 * 这里负责打印执行过的每一行汇编,这里和printAssemble区别是，这里把指令缓存到下一条指令打印,因为，需要打印jni,这样方便
 * @param uc
 * @param file
 * @param address
 * @param size
 * @param maxLengthLibraryName
 * @param instructionVisitor
 * @return
 */
// 快速写入: 内联 syscall，绕过 stdio
static inline void writeToFile(DirectWriteBuf* dw, const char* buf, size_t len) {
    if (len > 0) dw->write(buf, len);
}

static void printAssemble2(vm_context* uc, DirectWriteBuf* file,uint64_t address, uint32_t size, TraceInfo* traceInfo,InstructionVisitor* instructionVisitor){
    // ---- StringBuilder 复用: 不再每条指令 malloc/free ----
    StringBuilder* builder = &traceInfo->stringBuilder;
    if (!traceInfo->sbInitialized) {
        initStringBuilder(builder);
        traceInfo->sbInitialized = true;
    }

    // 上一条指令没有写寄存器 → 直接 flush 并复用
    if(builder->length > 0 && !traceInfo->hasLastWritePrinter){
        writeToFile(file, builder->buffer, builder->length);
        resetStringBuilder(builder);
    }

    cs_insn* insns = disassembleCache.get(address);

    if(insns == nullptr){
        // 内存与 host 共享，直接指针访问，跳过 uc_mem_read 的地址翻译开销
        cs_disasm_iter_wrapper((const uint8_t*)address, size, address, &insns);
        if(insns == nullptr) {
            LOGW("printAssemble2: disasm failed at 0x%llx size=%u", (unsigned long long)address, size);
            return;
        }
        disassembleCache.put(address,insns);
    }

    if(traceInfo->hasLastWritePrinter) {
        instructionVisitor->visitLast(traceInfo, uc, builder, address);
        writeToFile(file, builder->buffer, builder->length);
        resetStringBuilder(builder);
    }

    // ---- PendingAccess: 内联访存标注 @[addr] ----
    if(traceInfo->hasPendingAccess){
        PendingAccess& pa = traceInfo->pendingAccess;
        if (pa.last_mem_address != 0) {
            char hexBuf[17];
            if (pa.is_load) {
                appendStringN(builder, "  mem_r[0x", 10);
            } else {
                appendStringN(builder, "  mem_w[0x", 10);
            }
            int hlen = uint64ToHex(pa.last_mem_address, hexBuf);
            appendStringN(builder, hexBuf, hlen);
            appendRegionTag(builder, pa.last_mem_address);   // 访存地址归属: so:ro/:code/:rw / [stack]/[heap]
            if (!pa.is_load) {
                appendStringN(builder, "]=0x", 4);
                cs_insn* pinsn = pa.insn;
                if (pinsn && pinsn->detail) {
                    cs_aarch64& arm = pinsn->detail->aarch64;
                    uint64_t src_val = 0;
                    bool found_src = false;
                    bool found_simd = false;
                    for (int i = 0; i < arm.op_count; i++) {
                        if (arm.operands[i].type == AARCH64_OP_REG && (arm.operands[i].access & CS_AC_READ)) {
                            aarch64_reg r = arm.operands[i].reg;
                            if (r == AARCH64_REG_XZR || r == AARCH64_REG_WZR) {
                                src_val = 0;
                                found_src = true;
                                break;
                            }
                            int qreg = -1;
                            int simd_bytes = 0;
                            if (r >= AARCH64_REG_Q0 && r <= AARCH64_REG_Q31) {
                                qreg = VC_REG_Q0 + (r - AARCH64_REG_Q0);
                                simd_bytes = 16;
                            } else if (r >= AARCH64_REG_D0 && r <= AARCH64_REG_D31) {
                                qreg = VC_REG_Q0 + (r - AARCH64_REG_D0);
                                simd_bytes = 8;
                            } else if (r >= AARCH64_REG_S0 && r <= AARCH64_REG_S31) {
                                qreg = VC_REG_Q0 + (r - AARCH64_REG_S0);
                                simd_bytes = 4;
                            } else if (r >= AARCH64_REG_H0 && r <= AARCH64_REG_H31) {
                                qreg = VC_REG_Q0 + (r - AARCH64_REG_H0);
                                simd_bytes = 2;
                            } else if (r >= AARCH64_REG_B0 && r <= AARCH64_REG_B31) {
                                qreg = VC_REG_Q0 + (r - AARCH64_REG_B0);
                                simd_bytes = 1;
                            }
                            if (qreg >= 0) {
                                uint8_t qval[16] = {};
                                vc_reg_read_fast(uc, (vc_reg)qreg, qval);
                                char simdHex[33];
                                for (int j = 0; j < simd_bytes; j++) {
                                    byteToHex(qval[j], simdHex + j * 2);
                                }
                                appendStringN(builder, simdHex, simd_bytes * 2);
                                found_simd = true;
                                break;
                            }
                            int ureg = mapToUnicornReg(r);
                            if (ureg >= 0) {
                                int idx = cachedXRegIndex(ureg);
                                if (idx >= 0) {
                                    src_val = traceInfo->cachedXRegs[idx];
                                } else {
                                    vc_reg_read_fast(uc, (vc_reg)ureg, &src_val);
                                }
                                found_src = true;
                            }
                            break;
                        }
                    }
                    if (found_src) {
                        hlen = uint64ToHex(src_val, hexBuf);
                        appendStringN(builder, hexBuf, hlen);
                    } else if (!found_simd) {
                        appendChar(builder, '?');
                    }
                } else {
                    appendChar(builder, '?');
                }
            } else {
                appendChar(builder, ']');
            }
            // 字符串检测（仅 >= 8 字节访问，跳过 ldrb/ldrh 循环拷贝噪音）
            if (pa.last_mem_address > 0x1000 && pa.size >= 8) {
                if (safe_looks_like_cstr(uc, pa.last_mem_address, 4)) {
                    // 地址本身是字符串 → 完整打印
                    appendStringN(builder, " str:\"", 6);
                    appendCStr(uc, builder, pa.last_mem_address);
                    appendChar(builder, '"');
                } else if (pa.is_load) {
                    // 加载的值是字符串指针 → 完整打印指向的字符串
                    uint64_t loaded_val = 0;
                    if (safe_host_read(pa.last_mem_address, &loaded_val, 8)
                        && loaded_val > 0x1000
                        && safe_looks_like_cstr(uc, loaded_val, 4)) {
                        appendStringN(builder, " ->\"", 4);
                        appendCStr(uc, builder, loaded_val);
                        appendChar(builder, '"');
                    }
                }
            }
        }
        traceInfo->hasPendingAccess = false;
    }

    const auto& soinfo = traceInfo->currentSoInfo;

    // ---- 紧凑行头: so+0xOFF PC: mnemonic operands ----
    traceInfo->insnCounter++;
    appendChar(builder, '\n');
    appendStringN(builder, soinfo.libraryName.data(), soinfo.libraryName.size());
    appendStringN(builder, "+0x", 3);
    {
        char offBuf[17];
        int olen = uint64ToHex(address - soinfo.baseAddress, offBuf);
        appendStringN(builder, offBuf, olen);
    }
    appendChar(builder, ' ');
    {
        char pcBuf[17];
        appendStringN(builder, "0x", 2);
        int plen = uint64ToHex(address, pcBuf);
        appendStringN(builder, pcBuf, plen);
    }
    appendStringN(builder, ": ", 2);
    {
        uint8_t mnLen, opLen;
        if (disassembleCache.getStrLens(address, mnLen, opLen)) {
            appendStringN(builder, insns->mnemonic, mnLen);
            appendChar(builder, ' ');
            appendStringN(builder, insns->op_str, opLen);
        } else {
            appendStringN(builder, insns->mnemonic, strnlen(insns->mnemonic, 32));
            appendChar(builder, ' ');
            appendStringN(builder, insns->op_str, strnlen(insns->op_str, 160));
        }
    }

   if(instructionVisitor->visit != nullptr){
       instructionVisitor->visit(traceInfo,uc,builder,insns,address);
       traceInfo->pendingAccess.last_mem_address = 0;
       traceInfo->pendingAccess.insn = insns;
       traceInfo->hasPendingAccess = true;
       appendMemoryHexdump(uc, &traceInfo->pendingAccess, traceInfo->cachedXRegs);
   }

   // SVC 返回值 flush
   if (traceInfo->svcPending.type != SVC_PEND_NONE) {
       svc_flush_pending(builder, traceInfo->cachedXRegs[0],
                         &traceInfo->fdTracker, &traceInfo->svcPending);
   }

   if(insns->id == AARCH64_INS_SVC){
       uint64_t nr = traceInfo->cachedXRegs[8];
       svc_handle(uc, nr, builder, &traceInfo->fdTracker, &traceInfo->svcPending);
   }
    if(insns->id == AARCH64_INS_RET){
        uint64_t x0 = traceInfo->cachedXRegs[0];
        appendStringN(builder, " x0=0x", 6);
        char x0Hex[17];
        int x0Len = uint64ToHex(x0, x0Hex);
        appendStringN(builder, x0Hex, x0Len);
        writeToFile(file, builder->buffer, builder->length);
        resetStringBuilder(builder);
        traceInfo->hasLastWritePrinter = false;
        // 目标函数返回：ret 的目标 PC(x30) == VM 退出地址 → 强制刷盘
        if (__builtin_expect(traceInfo->entryLrSet
                             && traceInfo->cachedXRegs[30] == traceInfo->entryLr, false)) {
            file->flush();
        }
    }
}
/**
备份，怕改炸了
*/
static void printAssemble(vm_context* uc, DirectWriteBuf* file,uint64_t address, uint32_t size, TraceInfo* traceInfo,InstructionVisitor* instructionVisitor){
    StringBuilder builder;
    initStringBuilder(&builder);
    cs_insn* insns = disassembleCache.get(address);

    bool needUpdateCache = false;

    if(insns != nullptr){
        uint8_t* cacheCode = insns->bytes;
        if((uint32_t*)address == (uint32_t*)cacheCode){
            needUpdateCache = true;
        }
    }else{
        needUpdateCache = true;
    }
    if(needUpdateCache){
        char* code = (char*)address;
        cs_disasm_iter_wrapper((uint8_t*)code,size,address,&insns);
        disassembleCache.put(address,insns);
//        LOGD("insn:%s",disassembleCache.get(address)->mnemonic);
    }
    if(traceInfo->hasLastWritePrinter){
        instructionVisitor->visitLast(traceInfo,uc,&builder,address); //打印上一条指令变化的寄存器
    }
    char time_buffer[20];
    appendString(&builder,"\n");
    appendString(&builder, getCurrentTimeWithMilliseconds(time_buffer,sizeof(time_buffer)));
    const auto& soinfo = traceInfo->currentSoInfo;
    if(instructionVisitor->visit != nullptr){
        assembleDetail(uc,insns,&builder,address, false,(int)soinfo.maxLengthLibraryName,(char*)soinfo.libraryName.c_str(),(int)soinfo.libraryName.size(),soinfo.baseAddress,traceInfo->currentSoInfo.endAddress-traceInfo->currentSoInfo.baseAddress);
        instructionVisitor->visit(traceInfo,uc,&builder,insns,address);
    }
//////    address += insns->size;
    writeToFile(file, builder.buffer, builder.length);
    freeStringBuilder(&builder);
}


// ============================================================
// resolvePltTarget — 解析 PLT stub 指令，从 GOT 读取已解析的函数地址
//
// AArch64 标准 PLT stub 格式 (每个 entry 16 字节):
//   adrp x16, GOT_page       ; x16 = PC_4K对齐 + immhi:immlo << 12
//   ldr  x17, [x16, #offset] ; x17 = *(x16 + offset) = GOT 中的函数指针
//   add  x16, x16, #offset   ; x16 = GOT slot 地址 (给 lazy resolver 用)
//   br   x17                 ; 跳转到实际函数
//
// 本函数只需要前两条指令:
//   1. 从 adrp 解码出 GOT page 基址
//   2. 从 ldr 解码出页内偏移
//   3. 读取 GOT[page + offset] 中已由动态链接器填入的函数指针
//
// 参数: pltEntryAddr — PLT stub 在内存中的运行时地址
// 返回: GOT 中存储的目标函数地址，解析失败返回 0
// ============================================================
static uint64_t resolvePltTarget(uint64_t pltEntryAddr) {
    auto* code = (uint32_t*)pltEntryAddr;
    uint32_t inst0 = code[0]; // adrp x16, ...
    uint32_t inst1 = code[1]; // ldr  x17, [x16, #...]

    // 验证 adrp x16: op=1, immlo, 10000, immhi, Rd=10000(x16)
    // mask: bit[31] + bits[28:24] + bits[4:0] = 0x9F00001F
    // pattern: 1_10000_10000 = 0x90000010
    if ((inst0 & 0x9F00001F) != 0x90000010) return 0;

    // 验证 ldr x17, [x16, #imm]: size=11, V=0, opc=01, Rn=x16(10000), Rt=x17(10001)
    // 完整 pattern: 11_111_0_01_01_............_10000_10001
    // mask bits: [31:30]+[29:27]+[26]+[25:24]+[23:22]+[9:5]+[4:0] = 0xFFC003FF
    // value: F9400211
    if ((inst1 & 0xFFC003FF) != 0xF9400211) return 0;

    // ---- 解码 adrp 的立即数 ----
    // immlo = bits[30:29], immhi = bits[23:5] (19 bits)
    // imm21 = immhi:immlo, 符号扩展后左移 12 位得到页偏移
    uint32_t immlo = (inst0 >> 29) & 0x3;
    int32_t immhi = (int32_t)((inst0 >> 5) & 0x7FFFF);
    int32_t imm21 = (immhi << 2) | (int32_t)immlo;
    if (imm21 & (1 << 20)) imm21 |= (int32_t)0xFFE00000; // 符号扩展到 32 位
    int64_t adrp_offset = (int64_t)imm21 << 12;

    // adrp 结果 = PC 按 4K 对齐 + 页偏移
    uint64_t page = (pltEntryAddr & ~0xFFFULL) + adrp_offset;

    // ---- 解码 ldr 的页内偏移 ----
    // imm12 = bits[21:10], 对于 64 位 load 需要左移 3 (× 8 字节)
    uint32_t imm12 = (inst1 >> 10) & 0xFFF;
    uint64_t ldr_offset = (uint64_t)imm12 << 3;

    // 读取 GOT entry: 动态链接器已经把实际函数地址填入这里
    uint64_t got_addr = page + ldr_offset;
    uint64_t target = *(uint64_t*)got_addr;
    return target;
}


// ============================================================
// findPltSection — 从磁盘上的 SO 文件中解析 .plt section 的地址范围
//
// 原理：
//   打开 SO 文件 → 读 ELF header → 读 section header string table →
//   遍历 section headers，找到名为 ".plt" 的 section →
//   用 loadBias + sh_addr 得到运行时地址。
//
// 参数：
//   soPath   - SO 文件在磁盘上的完整路径
//   loadBias - ELF 加载偏移量 (dl_iterate_phdr 返回的 dlpi_addr)
//
// 返回：
//   成功时 {pltStart, pltEnd}，失败时 {0, 0}（section headers 被 strip 等情况）
//
// 注意：
//   如果 SO 被完全 strip (e_shoff == 0)，此函数返回 {0, 0}，
//   调用方检测到 pltStart == 0 后不做 PLT 优化，正常打印所有指令。
// ============================================================
struct PltRange {
    uint64_t start;
    uint64_t end;
};

static PltRange findPltSection(const char* soPath, uint64_t loadBias) {
    PltRange result = {0, 0};
    int fd = open(soPath, O_RDONLY);
    if (fd < 0) return result;

    Elf64_Ehdr ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) { close(fd); return result; }

    // 验证 ELF magic
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) { close(fd); return result; }

    // section headers 可能被 strip 掉
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shstrndx >= ehdr.e_shnum) {
        close(fd);
        return result;
    }

    // 读取 section name string table header
    Elf64_Shdr shstrtab_hdr;
    lseek(fd, (off_t)(ehdr.e_shoff + ehdr.e_shstrndx * sizeof(Elf64_Shdr)), SEEK_SET);
    if (read(fd, &shstrtab_hdr, sizeof(shstrtab_hdr)) != sizeof(shstrtab_hdr)) {
        close(fd); return result;
    }

    // 读取 section name strings
    char* shstrtab = (char*)malloc(shstrtab_hdr.sh_size);
    if (!shstrtab) { close(fd); return result; }
    lseek(fd, (off_t)shstrtab_hdr.sh_offset, SEEK_SET);
    if (read(fd, shstrtab, shstrtab_hdr.sh_size) != (ssize_t)shstrtab_hdr.sh_size) {
        free(shstrtab); close(fd); return result;
    }

    // 遍历 section headers 找 .plt
    for (uint16_t i = 0; i < ehdr.e_shnum; i++) {
        Elf64_Shdr shdr;
        lseek(fd, (off_t)(ehdr.e_shoff + i * sizeof(Elf64_Shdr)), SEEK_SET);
        if (read(fd, &shdr, sizeof(shdr)) != sizeof(shdr)) break;

        if (shdr.sh_name < shstrtab_hdr.sh_size) {
            const char* name = shstrtab + shdr.sh_name;
            if (strcmp(name, ".plt") == 0) {
                result.start = loadBias + shdr.sh_addr;
                result.end = result.start + shdr.sh_size;
                break;
            }
        }
    }

    free(shstrtab);
    close(fd);
    return result;
}

/**
 * trace_code — 每条指令执行前的回调 (VC_HOOK_CODE)
 *
 * 核心功能: 反汇编当前指令并输出到 trace 文件
 *
 * PLT 优化逻辑:
 *   问题: 调用外部函数时，BL 跳转到 .plt stub，PLT 内部有 4 条辅助指令
 *         (adrp x16 / ldr x17 / add x16 / br x17) 会被逐条打印，干扰阅读。
 *   方案: 检测到 BL/B 目标落在 .plt section 内时:
 *         1. 立即 flush 当前 BL 指令行到文件 (确保在 block hook 输出之前)
 *         2. 设置 pltSkipStart/pltSkipEnd 范围
 *         3. 后续回调发现 PC 在 PLT 范围内则直接 return，不打印
 *         4. funcCallTraceHook (UC_HOOK_BLOCK) 会在 PLT 跳转到外部地址时
 *            输出 "→ symbol(...)"，紧跟在 BL 行后面
 *   效果: 原来 "bl ... / adrp / ldr / add / br / → symbol()" 变为 "bl ... / → symbol()"
 */
// 逐指令回调（VC_HOOK_CODE）。只拿 vm_context，寄存器一律走 vc_* 接口读。
static void trace_code(vm_context *uc, uint64_t address, uint32_t size,
               void *user_data){
    auto* masterOrSelf = (TraceInfo*)user_data;
    auto* traceInfo = masterOrSelf->isTemplate
        ? getOrCreatePerThreadTrace(masterOrSelf, address)
        : masterOrSelf;

    // ---- JNI 返回值延迟打印 ----
    // JNI 入口 block hook 已快照参数 + 记了返回地址 g_jni_pending_ret。执行到该地址说明真实
    // JNI 已由 hook_block 执行完、X0=返回值。先 flush 挂起的指令行（保证 >>> 出现在调用行之后），
    // 再调 jni_on_return 原样跑 handler 打印。放在最前面（PLT/wrapper 跳过判断之前）——
    // wrapper 调用的返回点可能落在被隐藏区间内，晚了就会被 return 掉。
    if (__builtin_expect(g_jni_pending_ret != 0, 0) && address == g_jni_pending_ret) {
        if (traceInfo->sbInitialized && traceInfo->stringBuilder.length > 0) {
            writeToFile(&traceInfo->dwBuf, traceInfo->stringBuilder.buffer, traceInfo->stringBuilder.length);
            resetStringBuilder(&traceInfo->stringBuilder);
        }
        traceInfo->hasLastWritePrinter = false;
        traceInfo->hasPendingAccess = false;
        jni_on_return(traceInfo->vmContext, uc);
    }

    // ---- 步骤 A: PLT 内部指令跳过 ----
    // 如果上一条 BL/B 设置了 PLT 跳过范围，当前 PC 仍在 .plt 内则不打印。
    // 一旦 PC 离开 .plt（外部函数执行完返回到 BL 后面），清除跳过状态。
    if (traceInfo->pltSkipEnd != 0) {
        if (address >= traceInfo->pltSkipStart && address < traceInfo->pltSkipEnd) {
            return;
        }
        // PC 已经不在 PLT 范围内了，清除跳过状态
        traceInfo->pltSkipEnd = 0;
        traceInfo->pltSkipStart = 0;
    }

    // ---- JNI wrapper 指令跳过 ----
    // BL→PLT 检测到目标是 JNI 函数时设置此标记，
    // wrapper 内部指令不输出到 trace，直到 PC 回到 caller（BL 的下一条指令）。
    // 效果: >>> JNI 注解紧跟 BL 行输出，wrapper 汇编（含 epilogue）全部隐藏。
    if (traceInfo->jniWrapperSkip) {
        if (address == traceInfo->jniWrapperReturnAddr) {
            traceInfo->jniWrapperSkip = false;
            traceInfo->jniWrapperReturnAddr = 0;
        } else {
            // wrapper(_JNIEnv::xxx) 内部指令不输出，但仍要认出它里面那次真正的 JNI 调用：
            // 判 BLR/BR Xn（就是 capstone 的调用语义，这里直接位判更快），目标命中 jni_table
            // 就跑入口段。BLR 返回到 addr+4；BR 是尾调，返回到原始调用点(jniWrapperReturnAddr)。
            if (__builtin_expect(traceInfo->xregsDirectPtr != nullptr, true)) {
                uint32_t raw = *(uint32_t*)address;
                bool isBlr = (raw & 0xFFFFFC1F) == 0xD63F0000;
                bool isBr  = (raw & 0xFFFFFC1F) == 0xD61F0000;
                if (__builtin_expect(isBlr || isBr, false)) {
                    uint64_t tgt = traceInfo->xregsDirectPtr[(raw >> 5) & 0x1F];
                    if (isJniAddress(tgt)) {
                        jni_on_call(traceInfo->vmContext, tgt,
                                    isBlr ? (address + 4) : traceInfo->jniWrapperReturnAddr);
                    }
                }
            }
            return;
        }
    }

    auto context = traceInfo->vmContext;

    // ---- 模块信息更新 ----
    // 当 PC 跑到新的 SO 模块时，重新获取模块信息和 .plt section 范围。
    // 只在模块切换时触发一次，不影响性能。
    if(traceInfo->currentSoInfo.baseAddress == 0 || address < traceInfo->currentSoInfo.baseAddress || address >= traceInfo->currentSoInfo.endAddress){
        LibraryInfo soInfo = findModuleByAddressExact(address);
        traceInfo->currentSoInfo.baseAddress = soInfo.segments_start;
        traceInfo->currentSoInfo.endAddress = soInfo.segments_end;
        traceInfo->currentSoInfo.libraryName = getPathFileName(soInfo.name.c_str());
        traceInfo->currentSoInfo.fullPath = soInfo.name;
        traceInfo->currentSoInfo.loadBias = soInfo.base_address;
        traceInfo->currentSoInfo.maxLengthLibraryName = traceInfo->currentSoInfo.libraryName.size();
        // 从磁盘 SO 文件解析 .plt section 地址范围，失败时 pltStart/pltEnd 为 0
        PltRange plt = findPltSection(soInfo.name.c_str(), soInfo.base_address);
        traceInfo->currentSoInfo.pltStart = plt.start;
        traceInfo->currentSoInfo.pltEnd = plt.end;
    }

    // ---- 批量读取 x 寄存器缓存（直接 memcpy，0 function calls） ----
    // uc 由 vc_current_uc(ctx) 在函数顶部取好（公开逃生舱）；这里直接用引擎原生 uc_code_fast_*
    // 拿寄存器文件直接指针 + NZCV，最快、无额外包装层。xregsDirectPtr 只取一次并缓存。
    if (__builtin_expect(traceInfo->xregsDirectPtr == nullptr, false)) {
        traceInfo->xregsDirectPtr = vc_code_fast_get_xregs(uc);
    }
    __builtin_memcpy(traceInfo->cachedXRegs, traceInfo->xregsDirectPtr, 32 * 8);
    traceInfo->cachedXRegs[32] = (uint64_t)vc_code_fast_get_nzcv(uc);
    if (__builtin_expect(!traceInfo->entryLrSet, false)) {
        traceInfo->entryLr = traceInfo->cachedXRegs[30];  // 入口 x30 = 调用者返回地址
        traceInfo->entryLrSet = true;
    }

    // ---- 外部 [CALL] 返回值补打：到达上次外部调用的返回地址(addr+4)时，X0=返回值 ----
    // 外部函数在 host 上执行，中间不触发 trace_code，所以返回值天然在此刻的 cachedXRegs[0]。
    if (__builtin_expect(traceInfo->funcCallPendingRet != 0
                         && address == traceInfo->funcCallPendingRet, false)) {
        funcCallTraceEmit(masterOrSelf->funcCallCtx, uc, 0, nullptr, nullptr,
                          traceInfo->cachedXRegs[0], true);
        traceInfo->funcCallPendingRet = 0;
    }

    traceInfo->hasWrittenData = true;

    // ---- 步骤 B: 反汇编并输出当前指令 ----
    static InstructionVisitor visitor = { visitLastImpl, visitImpl };

    printAssemble2(uc,&traceInfo->dwBuf,address,size,traceInfo,&visitor);

    // ---- 定期 flush: 防止进程被杀丢数据 ----
    if (__builtin_expect(++traceInfo->flushCounter >= TraceInfo::FLUSH_INTERVAL, false)) {
        traceInfo->flushCounter = 0;
        traceInfo->dwBuf.flush();
    }

    // ---- 定期内存清理: 每 300 万条指令释放冷数据 ----
    if (__builtin_expect(++traceInfo->memCleanupCounter >= TraceInfo::MEM_CLEANUP_INTERVAL, false)) {
        traceInfo->memCleanupCounter = 0;
        // 释放反汇编缓存中 LRU 半边 (way 2,3)，保留热点 (way 0,1)
        disassembleCache.trimCold();
        // 收缩膨胀的 StringBuilder（正常 ~512B，异常可能数 KB）
        if (traceInfo->sbInitialized && traceInfo->stringBuilder.capacity > 8192) {
            freeStringBuilder(&traceInfo->stringBuilder);
            initStringBuilder(&traceInfo->stringBuilder);
        }
    }

    // ---- 步骤 C: 检测 BL/B 到 .plt 的跳转 ----
    // printAssemble2 处理完后，pendingAccess->insn 中保存了当前指令信息。
    // 如果是 BL/B 指令且跳转目标在 .plt section 内，说明这是一次 PLT 调用。
    //
    // 为什么需要立即 flush:
    //   printAssemble2 采用延迟输出机制——当前指令的文本存在 stringBuilder 中，
    //   要等下一次 trace_code 调用时才 flush。但 PLT 中的指令被我们跳过了，
    //   如果不提前 flush BL 行，后续的 "→ symbol(...)" 会出现在 BL 行之前。
    //
    // 内部 PLT 调用处理:
    //   SO 内部的导出函数也可能通过 PLT 调用 (如 visibility("default") 的函数)。
    //   此时 funcCallTraceHook (block hook) 不会打印符号 (目标在 SO 内部被跳过)，
    //   所以我们在这里解析 PLT stub → GOT → 实际函数地址，直接打印符号名。
    //   对于外部 PLT 调用，funcCallTraceHook 会在目标函数入口处打印带参数的符号，
    //   这里不重复打印。
    if (traceInfo->hasPendingAccess && traceInfo->pendingAccess.insn) {
        cs_insn* insn = traceInfo->pendingAccess.insn;
        if (insn->id == AARCH64_INS_BL || insn->id == AARCH64_INS_B) {
            // 从机器码中解码跳转目标地址
            // BL/B 编码: [31:26]=opcode, [25:0]=imm26
            // 目标 = PC + sign_extend(imm26) * 4
            uint32_t raw = *(uint32_t*)address;
            int32_t imm26 = (int32_t)(raw & 0x03FFFFFF);
            if (imm26 & (1 << 25)) imm26 |= (int32_t)0xFC000000; // 符号扩展
            uint64_t target = address + ((int64_t)imm26 << 2);

            // 目标在 .plt section 内? (如果 pltStart == 0 说明 ELF 解析失败，跳过优化)
            uint64_t pltS = traceInfo->currentSoInfo.pltStart;
            uint64_t pltE = traceInfo->currentSoInfo.pltEnd;
            if (pltS != 0 && pltE != 0 && target >= pltS && target < pltE) {

                // 立即把 BL 指令行写入文件
                if (traceInfo->stringBuilder.length > 0) {
                    writeToFile(&traceInfo->dwBuf, traceInfo->stringBuilder.buffer, traceInfo->stringBuilder.length);
                    resetStringBuilder(&traceInfo->stringBuilder);
                }
                // BL 的写寄存器是 X30 (返回地址)，对于 PLT 调用不需要显示
                traceInfo->hasLastWritePrinter = false;
                if (traceInfo->hasPendingAccess) {
                    traceInfo->hasPendingAccess = false;
                }

                // ---- 内部 PLT 调用: 解析 PLT stub 拿到实际目标，打印符号 ----
                // 解析 PLT stub 的 adrp+ldr 指令，从 GOT 中读取已解析的函数地址
                uint64_t resolvedTarget = resolvePltTarget(target);
                if (resolvedTarget != 0) {
                    // 判断目标是否在当前 SO 内部
                    uint64_t soBase = traceInfo->currentSoInfo.baseAddress;
                    uint64_t soEnd  = traceInfo->currentSoInfo.endAddress;

                    if (resolvedTarget >= soBase && resolvedTarget < soEnd) {
                        // 内部 PLT 调用: funcCallTraceHook 不会打印 (快速路径跳过)，
                        // 所以在这里直接输出符号名
                        const char* symName = lookupSymbolExact(resolvedTarget);
                        if (symName) {
                            const std::string& pretty = demangleSymbol(symName);
                            writeToFile(&traceInfo->dwBuf, "  [CALL] ", 9);
                            writeToFile(&traceInfo->dwBuf, pretty.data(), pretty.size());

                            // 如果目标是 JNI wrapper (_JNIEnv::)，
                            // 跳过 wrapper 内部指令输出，使 >>> 注解紧跟 BL 行
                            if (pretty.find("_JNIEnv::") != std::string::npos) {
                                traceInfo->jniWrapperSkip = true;
                                traceInfo->jniWrapperReturnAddr = address + 4;
                            }
                        }
                    } else if (isJniAddress(resolvedTarget)) {
                        // 目标直接就是 JNI 函数：capstone 认出调用 + 查 jni_table 命中 →
                        // 就地跑入口段解析参数（不再靠独立的 BLOCK hook 监视 libart 地址）。
                        jni_on_call(context, resolvedTarget, address + 4);
                        // wrapper 内部指令不输出；返回值到 addr+4 时由 jni_on_return 补上
                        traceInfo->jniWrapperSkip = true;
                        traceInfo->jniWrapperReturnAddr = address + 4;
                    } else {
                        // 其他外部调用: trace_code 用 capstone 认出，直接出 [CALL] name(args)。
                        // 返回值在 addr+4 那条指令补 =>（外部函数在 host 上跑，中间无 trace_code）。
                        const char* sym = lookupSymbolExact(resolvedTarget);
                        funcCallTraceEmit(masterOrSelf->funcCallCtx, uc, resolvedTarget, sym,
                                          traceInfo->cachedXRegs, 0, false);
                        traceInfo->funcCallPendingRet = address + 4;
                    }
                }

                // 用整个 .plt section 范围作为跳过区间
                traceInfo->pltSkipStart = pltS;
                traceInfo->pltSkipEnd = pltE;
            }
        }
    }

    // ---- 步骤 D: br/blr 外部跳转 — 提前 flush 保证输出顺序 ----
    // funcCallTraceHook (UC_HOOK_BLOCK) 在跳转目标处直接写文件，
    // 如果不提前 flush，branch 指令还在 builder 中 →
    // [CALL] 输出出现在 br/blr 之前，顺序错误。
    if (traceInfo->hasPendingAccess && traceInfo->pendingAccess.insn) {
        cs_insn* insn = traceInfo->pendingAccess.insn;
        if (insn->id == AARCH64_INS_BR || insn->id == AARCH64_INS_BLR) {
            uint32_t raw = *(uint32_t*)address;
            int regIdx = (raw >> 5) & 0x1F;
            if (regIdx <= 30) {
                uint64_t targetAddr = traceInfo->cachedXRegs[regIdx];
                uint64_t soBase = traceInfo->currentSoInfo.baseAddress;
                uint64_t soEnd  = traceInfo->currentSoInfo.endAddress;
                if (targetAddr < soBase || targetAddr >= soEnd) {
                    if (traceInfo->stringBuilder.length > 0) {
                        writeToFile(&traceInfo->dwBuf, traceInfo->stringBuilder.buffer, traceInfo->stringBuilder.length);
                        resetStringBuilder(&traceInfo->stringBuilder);
                    }
                    traceInfo->hasLastWritePrinter = false;
                    traceInfo->hasPendingAccess = false;
                }
            }
        }
    }

}



std::unordered_map<uint64_t,TraceInfo*> traceContextMap;

// 前置声明：重复创建 trace 会话前，需要先把旧会话完整释放掉。
void freeTrace(uint64_t wrapper_func);

// 同一个目标函数再次调用 trace() 时，先销毁旧的 trace 会话，避免复用旧 VM、旧 Hook 和旧用户数据。
static void destroyPreviousTraceSession(void* funcAddr) {
    if (funcAddr == nullptr) {
        return;
    }

    // 在 trace 侧 map 里按 targetFuncAddr 找旧会话的 wrapper（不查 VCPU 的 get_vm）
    uint64_t wrapperFunc = 0;
    for (auto& kv : traceContextMap) {
        if (kv.second && kv.second->targetFuncAddr == (uint64_t)funcAddr) {
            wrapperFunc = kv.first;
            break;
        }
    }
    if (wrapperFunc != 0 && traceContextMap.find(wrapperFunc) != traceContextMap.end()) {
        freeTrace(wrapperFunc);
    }
}

// 重置单次执行期间产生的临时状态，避免同一个目标二次执行时复用旧缓存。
static void resetTraceInfoRuntimeState(TraceInfo* traceInfo,
                                       vm_context* vmContext,
                                       void* funcAddr) {
    if (traceInfo == nullptr || vmContext == nullptr || funcAddr == nullptr) {
        return;
    }

    traceInfo->vmContext = vmContext;
    traceInfo->timeUpdateCounter = 0;  // 重置时间缓存计数器
    traceInfo->cachedTimeStr[0] = '\0';
    // 先清理上一轮执行遗留的延迟输出对象，避免下一轮继续使用旧状态。
    traceInfo->hasLastWritePrinter = false;
    // StringBuilder 复用: 重置内容，保留 buffer
    if (traceInfo->sbInitialized) {
        resetStringBuilder(&traceInfo->stringBuilder);
    }
    traceInfo->hasPendingAccess = false;
    traceInfo->pltSkipStart = 0;
    traceInfo->pltSkipEnd = 0;

    // 刷新模块元数据，确保 PLT 跳过逻辑和符号解析仍然对应当前执行目标。
    traceInfo->currentSoInfo = {};
    LibraryInfo soInfo = findModuleByAddressExact((uint64_t)funcAddr);
    traceInfo->currentSoInfo.baseAddress = soInfo.segments_start;
    traceInfo->currentSoInfo.endAddress = soInfo.segments_end;
    traceInfo->currentSoInfo.libraryName = getPathFileName(soInfo.name.c_str());
    traceInfo->currentSoInfo.fullPath = soInfo.name;
    traceInfo->currentSoInfo.loadBias = soInfo.base_address;
    traceInfo->currentSoInfo.maxLengthLibraryName = traceInfo->currentSoInfo.libraryName.size();
    PltRange plt = findPltSection(soInfo.name.c_str(), soInfo.base_address);
    traceInfo->currentSoInfo.pltStart = plt.start;
    traceInfo->currentSoInfo.pltEnd = plt.end;
}

/*
 * 提供一个简单的traceApi,只需要提供输出的文件路径即可，如果需要输出JNI调用，则传入jniEnv,返回一个同等功能的函数指针
 */
Export_Symbol
// ============================================================
// pthread_create 自动 VM 追踪
// ============================================================
// trace 期间遇到 libc pthread_create：在它真正执行前（block hook，早于 external_jump
// 的寄存器快照），把 x2(start_routine) 换成 trace() 包装 —— 新线程于是在 VM 内执行并被
// 独立 trace。性能：hook 用地址范围锁定在 pthread_create 入口，非该地址的块连回调都不进。
// 命中才做昂贵包装，且按 start_routine 缓存，同一入口只包一次。
static uint64_t g_pthread_create_addr = 0;                     // libc pthread_create 入口（一次解析）
static std::unordered_map<uint64_t, void*> g_thread_wrappers;  // start_routine → wrapper（缓存）
static int g_thread_wrap_lock = 0;

// TCP 远程 trace 的共享连接：第一次 trace() 建立，后续所有 trace() 复用同一 socket，
// 引用计数到 0 时（最后一个 freeTrace）才发 end frame + 关闭。修复「同时 trace 多个函数时
// 第二个 trace(tcp:) 重复 setup_tcp_server 阻塞」的问题。
static int g_shared_tcp_fd = -1;
static int g_shared_tcp_refcount = 0;
static int g_shared_tcp_lock = 0;
// JavaVM 缓存：第一次检测到后缓存，后续 trace()（含线程 hook 触发的）跳过 JNI_GetCreatedJavaVMs
static JavaVM* g_cached_javavm = nullptr;

// pthread_create 入口 block hook（地址范围锁定，只在该入口触发）。
// 此刻 x0-x7=调用参数，x2=start_routine，且早于 external_jump 的寄存器快照，改 x2 生效。
// 【独立的可选功能 hook】自动线程追踪：和 trace_code 的判定逻辑完全分离 —— 它是可选项
// （vc_set_auto_trace_threads 开），所以单独挂一个地址锁定的 VC_HOOK_BLOCK，
// 只在 pthread_create 入口触发；关掉/没命中时对 trace 主链路零影响。
static void thread_create_wrap_hook(vm_context* uc, uint64_t address, uint32_t size, void* user_data) {
    (void)address; (void)size;
    auto* master = (TraceInfo*)user_data;
    if (!master || !master->auto_trace_threads) return;   // 运行时开关（trace 层）
    uint64_t start_routine = 0;
    vc_reg_read(uc, VC_REG_X2, &start_routine);
    if (start_routine == 0) return;

    // SO 过滤：只包目标 SO 内的线程函数，跳过 libc/ART 等系统内部线程
    uint64_t soBase = master->currentSoInfo.baseAddress;
    uint64_t soEnd  = master->currentSoInfo.endAddress;
    if (soBase == 0 || start_routine < soBase || start_routine >= soEnd) return;

    if (master->origLogPath.empty()) return;   // 无输出配置

    // 查缓存 → miss 就直接 2 参 trace() 包装（add_hook 按 ctx 路由到子 vm，从 hook 里调
    // 也正确）→ 存缓存。不持锁调 trace()（trace 可能触发 freeTrace，也要该锁）。
    while (__sync_lock_test_and_set(&g_thread_wrap_lock, 1)) { __builtin_arm_wfe(); }
    auto it = g_thread_wrappers.find(start_routine);
    void* wrapper = (it != g_thread_wrappers.end()) ? it->second : nullptr;
    bool need_create = (it == g_thread_wrappers.end());
    __sync_lock_release(&g_thread_wrap_lock);

    if (need_create) {
        // 把父函数前缀编进 logPath（"真实路径|sub|父前缀"），trace() 解析后让子会话文件名带上父。
        std::string childLogPath = master->origLogPath + "|sub|" + master->filePrefix;
        wrapper = trace((void*)start_routine, (char*)childLogPath.c_str());   // 2 参，替换即可
        while (__sync_lock_test_and_set(&g_thread_wrap_lock, 1)) { __builtin_arm_wfe(); }
        g_thread_wrappers[start_routine] = wrapper;   // 即使 null 也缓存，避免反复重试
        __sync_lock_release(&g_thread_wrap_lock);
    }

    if (wrapper) {
        uint64_t w = (uint64_t)wrapper;
        vc_reg_write(uc, VC_REG_X2, &w);       // external_jump 随后 sync 读到新 x2
    }
}

void* trace(void* funcAddr, char* logPath,vm_context** vmContext){
    // 顶层调用（非线程 hook 触发）时清空线程包装缓存 —— 保证每次 trace() 独立，不复用上一轮
    // 会话遗留的子 wrapper。线程 hook 触发的 trace() 一定带 "|sub|" 标记（childLogPath），
    // 据此区分：无 "|sub|" = 顶层调用 → 清缓存；有 = 子会话 → 保留缓存供并发同函数线程复用。
    if (!(logPath && strstr(logPath, "|sub|"))) {
        while (__sync_lock_test_and_set(&g_thread_wrap_lock, 1)) { __builtin_arm_wfe(); }
        g_thread_wrappers.clear();
        __sync_lock_release(&g_thread_wrap_lock);
    }
    // 解析父标记："真实路径|sub|父前缀" —— 自动线程追踪的子会话用它让文件名带上父函数。
    // 无标记则 parentPrefix=nullptr，走普通命名。realLogPath 必须活到函数结束（logPath 指向它）。
    std::string realLogPath;
    const char* parentPrefix = nullptr;
    if (const char* m = strstr(logPath, "|sub|")) {
        realLogPath.assign(logPath, m - logPath);
        parentPrefix = m + 5;                 // strlen("|sub|")
        logPath = (char*)realLogPath.c_str(); // 后续 TCP 判断/computeLogDir/origLogPath 都用真实路径
    }

    // 每次重新创建 trace 会话前，先把上一轮同目标函数的会话彻底清掉。
    destroyPreviousTraceSession(funcAddr);
    if (vmContext != nullptr) {
        *vmContext = nullptr;
    }
    auto * func = vc_make_handle((void*)funcAddr,vmContext);
    if(func == nullptr){
        exit(-1);
    }
    if(logPath == nullptr || strlen(logPath) == 0){
        return nullptr;
    }
    // JNI trace 获取 — 整条路径都做防御，任何一步失败就跳过（不影响核心 trace）
    do {
        // JavaVM 只检测一次并缓存；后续 trace()（含线程 hook 触发的）跳过 JNI_GetCreatedJavaVMs，
        // 只做 addJniTrace（deferred，安全）—— 避免在 Unicorn 回调里调 JNI 触发 ART 栈边界检查。
        if (!g_cached_javavm) {
            uint64_t jniGetVMsAddr = resolveSymbolInSo("/libart.so", "JNI_GetCreatedJavaVMs");
            if (!jniGetVMsAddr) {
                LOGW("trace: JNI_GetCreatedJavaVMs not found, skipping JNI trace");
                break;
            }
            typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
            auto JNI_GetCreatedJavaVMs_fn = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(jniGetVMsAddr);
            JavaVM* javaVm = nullptr;
            jsize success = 0;
            JNI_GetCreatedJavaVMs_fn(&javaVm, 1, &success);
            if (!javaVm || success < 1) {
                LOGW("trace: no JavaVM found, skipping JNI trace");
                break;
            }
            g_cached_javavm = javaVm;
        }
        // 不再注册 JNI 的 BLOCK hook —— JNI 调用由 trace_code 认出。这里只需把表建好
        // （必须在顶层非 hook 上下文做，里面要 GetEnv）。
        jni_ensure_table();
    } while (false);
//    tryGetJvm();
    // TCP 远程 trace 检测 —— 全局共享一条连接：第一次建立，后续 trace() 复用，引用计数管理。
    int tcpClientFd = -1;
    bool isTcpMode = (strncmp(logPath, "tcp:", 4) == 0);
    if (isTcpMode) {
        while (__sync_lock_test_and_set(&g_shared_tcp_lock, 1)) { __builtin_arm_wfe(); }
        if (g_shared_tcp_fd < 0) {
            g_shared_tcp_fd = DirectWriteBuf::setup_tcp_server(logPath + 4);
            if (g_shared_tcp_fd >= 0) LOGI("trace: TCP connected (shared), fd=%d", g_shared_tcp_fd);
        }
        tcpClientFd = g_shared_tcp_fd;
        if (tcpClientFd >= 0) g_shared_tcp_refcount++;
        __sync_lock_release(&g_shared_tcp_lock);
        if (tcpClientFd < 0) {
            LOGE("trace: TCP server setup failed");
            return func;
        }
    }

    // 计算 per-thread 日志目录和文件名前缀
    std::string logDir = isTcpMode ? "" : computeLogDir(logPath);
    LibraryInfo funcSoInfo = findModuleByAddressExact((uint64_t)funcAddr);
    std::string soName = getPathFileName(funcSoInfo.name.c_str());
    char prefixBuf[384];
    unsigned long funcOff = (unsigned long)((uint64_t)funcAddr - funcSoInfo.base_address);
    if (parentPrefix) {
        // 自动线程追踪的子会话：文件名带上父函数，一眼看出是谁开的线程
        snprintf(prefixBuf, sizeof(prefixBuf), "%s__sub__%s+0x%lx",
                 parentPrefix, soName.c_str(), funcOff);
    } else {
        snprintf(prefixBuf, sizeof(prefixBuf), "%s+0x%lx", soName.c_str(), funcOff);
    }

    auto traceContextIt = traceContextMap.find((uint64_t)func);
    if (traceContextIt == traceContextMap.end()) {
        vc_hook_h codehook = 0;
        TraceInfo* traceInfo = new TraceInfo();

        traceInfo->isTemplate = true;
        traceInfo->logDir = logDir;
        traceInfo->origLogPath = logPath;   // 自动线程追踪递归调 trace() 用
        traceInfo->filePrefix = prefixBuf;
        traceInfo->targetFuncAddr = (uint64_t)funcAddr;
        traceInfo->tcpClientFd = tcpClientFd;

        resetTraceInfoRuntimeState(traceInfo, *vmContext, funcAddr);
        traceInfo->vmContext = *vmContext;

        traceInfo->currentSoInfo = {};
        traceInfo->currentSoInfo.baseAddress = funcSoInfo.segments_start;
        traceInfo->currentSoInfo.endAddress = funcSoInfo.segments_end;
        traceInfo->currentSoInfo.libraryName = soName;
        traceInfo->currentSoInfo.fullPath = funcSoInfo.name;
        traceInfo->currentSoInfo.loadBias = funcSoInfo.base_address;
        traceInfo->currentSoInfo.maxLengthLibraryName = soName.size();
        PltRange plt = findPltSection(funcSoInfo.name.c_str(), funcSoInfo.base_address);
        traceInfo->currentSoInfo.pltStart = plt.start;
        traceInfo->currentSoInfo.pltEnd = plt.end;

        traceInfo->hasLastWritePrinter = false;
        traceContextMap[(uint64_t)func] = traceInfo;

        // 注册逐指令回调。回调只拿 vm_context，不接触底层引擎句柄。
        vc_hook_add(*vmContext, &codehook, VC_HOOK_CODE, (void *) trace_code, traceInfo, 0, 0);
        traceInfo->funcCallCtx = addFuncCallTrace(vmContext, &traceInfo->fdTracker, nullptr);

        // 自动线程追踪的 hook 不在这里挂 —— 它是可选功能，等用户真的调
        // vc_set_auto_trace_threads(ctx,true) 时才注册（见该函数）。不开就完全不存在这个 hook。

    } else {
        TraceInfo* traceInfo = traceContextIt->second;
        // 清理调用线程的 TLS 引用
        tls_trace_clones.erase(traceInfo);
        // 清理旧的 per-thread 克隆（重新 trace 同一目标时）
        while (__sync_lock_test_and_set(&g_trace_clone_lock, 1)) { __builtin_arm_wfe(); }
        for (auto it = g_trace_all_clones.begin(); it != g_trace_all_clones.end(); ) {
            if ((*it)->master == traceInfo) {
                cleanupPerThreadTrace(*it);
                delete *it;
                it = g_trace_all_clones.erase(it);
            } else {
                ++it;
            }
        }
        __sync_lock_release(&g_trace_clone_lock);

        traceInfo->logDir = logDir;
        traceInfo->origLogPath = logPath;
        traceInfo->filePrefix = prefixBuf;
        traceInfo->targetFuncAddr = (uint64_t)funcAddr;
        traceInfo->tcpClientFd = tcpClientFd;
        resetTraceInfoRuntimeState(traceInfo, *vmContext, funcAddr);
    }
    return func;
}
void freeTrace(uint64_t wrapper_func){
    auto traceContextIt = traceContextMap.find(wrapper_func);
    if (traceContextIt == traceContextMap.end()) {
        return;
    }

    TraceInfo* traceInfo = traceContextIt->second;

    // 清理调用线程的 TLS 引用（防止地址复用导致 use-after-free）
    tls_trace_clones.erase(traceInfo);
    tls_trace_active_dwbuf = nullptr;

    // 清理所有 per-thread 克隆
    while (__sync_lock_test_and_set(&g_trace_clone_lock, 1)) { __builtin_arm_wfe(); }
    for (auto it = g_trace_all_clones.begin(); it != g_trace_all_clones.end(); ) {
        if ((*it)->master == traceInfo) {
            TraceInfo* clone = *it;
            if (clone->sbInitialized && clone->stringBuilder.length > 0 && clone->dwBufInitialized) {
                clone->dwBuf.write(clone->stringBuilder.buffer, clone->stringBuilder.length);
            }
            cleanupPerThreadTrace(clone);
            delete clone;
            it = g_trace_all_clones.erase(it);
        } else {
            ++it;
        }
    }
    __sync_lock_release(&g_trace_clone_lock);

    // 清理主模板自身（不拥有文件，但可能有 StringBuilder 等）
    traceInfo->hasPendingAccess = false;
    if (traceInfo->sbInitialized) {
        freeStringBuilder(&traceInfo->stringBuilder);
        traceInfo->sbInitialized = false;
    }
    traceInfo->hasLastWritePrinter = false;
    disassembleCache.clear();
    clearSymbolCache();
    freeFuncCallTraceCtx(traceInfo->funcCallCtx);
    traceInfo->funcCallCtx = nullptr;
    if (traceInfo->tcpClientFd >= 0) {
        int fd = traceInfo->tcpClientFd;
        traceInfo->tcpClientFd = -1;
        // 共享 TCP 连接：引用计数到 0（最后一个会话）才发 end frame + 关闭，否则别的会话还在用。
        while (__sync_lock_test_and_set(&g_shared_tcp_lock, 1)) { __builtin_arm_wfe(); }
        if (fd == g_shared_tcp_fd) {
            if (--g_shared_tcp_refcount <= 0) {
                DirectWriteBuf::send_end_frame(fd);
                close(fd);
                g_shared_tcp_fd = -1;
                g_shared_tcp_refcount = 0;
            }
        } else {
            DirectWriteBuf::send_end_frame(fd);  // 非共享 fd（异常路径），照旧关
            close(fd);
        }
        __sync_lock_release(&g_shared_tcp_lock);
    }
    if (traceInfo->vmContext != nullptr) {
        vc_free(traceInfo->vmContext);
        traceInfo->vmContext = nullptr;
    }
    delete traceInfo;
    traceContextMap.erase(traceContextIt);
}

// ============================================================
//  崩溃 / exit 前强制刷 trace（可选）—— vc_set_trace_crash_flush(true) 开启
//  目标崩了(SIGSEGV 等)或退出时，把 trace 缓冲(文件模式)抢救到盘，不丢崩溃前那段。
// ============================================================
static struct sigaction g_trace_old_sa[NSIG];
static volatile sig_atomic_t g_trace_crash_armed = 0;

// 崩溃时别碰free和锁，十有八九死锁。只抢数据：sb里没刷完的先塞进dwBuf，再刷盘关fd。脏就脏了，总比丢了好。仅文件模式。
static void trace_salvage_all_best_effort() {
    for (TraceInfo* ti : g_trace_all_clones) {
        if (!ti || !ti->dwBufInitialized) continue;
        if (ti->sbInitialized && ti->stringBuilder.length > 0) {
            ti->dwBuf.write(ti->stringBuilder.buffer, ti->stringBuilder.length);   // 补最后一行
            ti->stringBuilder.length = 0;
        }
        ti->dwBuf.flushFromSignal();   // 刷当前缓冲
        ti->dwBuf.closeFromSignal();   // 关文件
    }
}


static void trace_crash_handler(int sig, siginfo_t* info, void* uctx) {
    trace_salvage_all_best_effort();
    // 链回旧 handler：不吞掉崩溃，让 tombstone / core dump / 上层照常
    struct sigaction& old = g_trace_old_sa[sig];
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
        old.sa_sigaction(sig, info, uctx);
    } else if (old.sa_handler && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);   // 恢复默认再重抛 → 正常崩溃流程
    }
}

static void trace_atexit_flush() { trace_salvage_all_best_effort(); }

// 有时候，有些检测会导致app崩溃，但是缓存还没写到文件里，导致trace不全。开启后，会在app崩溃前，把缓存写到文件里。
void vc_set_trace_crash_flush(bool enable) {
    if (!enable || g_trace_crash_armed) return;
    g_trace_crash_armed = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = trace_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM, SIGINT };
    for (int s : sigs) sigaction(s, &sa, &g_trace_old_sa[s]);
    atexit(trace_atexit_flush);
}
void* trace(void* funcAddr, char* logPath){
    vm_context* ctx = nullptr;
    void* result = trace(funcAddr,logPath,&ctx);
    return result;
}

// 开关某个 trace 会话的自动线程追踪（trace 层特性，默认开）。ctx 来自 trace(f, path, &ctx)。
// 按 ctx 找到对应 TraceInfo 设 flag —— VCPU 层(vm_context)不再持有此状态。
void vc_set_auto_trace_threads(vm_context* ctx, bool enable){
    if (!ctx) return;
    for (auto& kv : traceContextMap) {
        TraceInfo* ti = kv.second;
        if (!ti || ti->vmContext != ctx) continue;
        ti->auto_trace_threads = enable;

        // 【开启时才挂 hook】—— 这是可选功能，用户不开就压根不注册，引擎里连这个地址范围
        // 都不存在，对 trace 主链路零影响。只挂一次；之后关掉开关走回调里的 flag 早退即可
        // （hook 没法摘，但地址锁定在 pthread_create 入口，命中即 return，代价可忽略）。
        if (enable && !ti->threadHookInstalled) {
            if (g_pthread_create_addr == 0) {
                g_pthread_create_addr = resolveSymbolInSo("/libc.so", "pthread_create");
            }
            if (g_pthread_create_addr) {
                vm_context* c = ctx;
                vc_hook_h thh = 0;
                vc_hook_add(c, &thh, VC_HOOK_BLOCK, (void*)thread_create_wrap_hook,
                         ti, g_pthread_create_addr, g_pthread_create_addr);
                ti->threadHookInstalled = true;
            } else {
                LOGW("vc_set_auto_trace_threads: pthread_create 未解析到，自动线程追踪未生效");
            }
        }
        return;
    }
}
//void* original_test_thread = nullptr ;  // 原始函数指针

//封装了dobby，可以很方便的一键替换要trace的地方进入trace
void* replace_trace(void* target_func, char* logPath){
    void* trace_ptr = trace(target_func,logPath);
    //    // 使用 Dobby 进行 Hook
    LOGD("trace save to:%s",logPath);

    int hook_result = DobbyHook((void*)target_func,
                                trace_ptr, nullptr);
    if(hook_result){
        return nullptr;
    }
    return trace_ptr;
}

void restore_function(void* target_func){
    DobbyDestroy(target_func);
}
