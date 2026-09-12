//
// Created by ASUS on 2026-03-16.
//

#include "ARM64Emulator.h"
#include "FuncCallTrace.h"
#include "SymbolTable.h"
#include "logging.h"
#include "StringUtil.h"
#include "SafeMemRead.h"
#include "DirectWriteBuf.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <mutex>
#include <cxxabi.h>
#include <dlfcn.h>
#include <link.h>
#include "LibraryUtils.h"

// 内联字符串比较，避免 strcmp PLT 间接跳转
static inline bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// EastTrace.cpp 导出的 per-thread 活跃 DirectWriteBuf
extern thread_local DirectWriteBuf* tls_trace_active_dwbuf;

static std::unordered_map<const char*, std::string> s_demangle_cache;
static int s_demangle_lock = 0;

static const std::string& demangleSymbol(const char* mangled) {
    static const std::string empty;
    if (!mangled) return empty;
    while (__sync_lock_test_and_set(&s_demangle_lock, 1)) { __builtin_arm_wfe(); }
    auto it = s_demangle_cache.find(mangled);
    if (it != s_demangle_cache.end()) {
        const std::string& ref = it->second;
        __sync_lock_release(&s_demangle_lock);
        return ref;
    }
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        auto& ref = s_demangle_cache[mangled] = std::string(demangled);
        free(demangled);
        __sync_lock_release(&s_demangle_lock);
        return ref;
    }
    auto& ref = s_demangle_cache[mangled] = std::string(mangled);
    __sync_lock_release(&s_demangle_lock);
    return ref;
}

// ============================================================
// 参数类型与函数签名描述
// ============================================================

enum ParamType {
    PT_INT,       // 十进制整数
    PT_HEX,       // 十六进制地址/值
    PT_STR,       // const char* 字符串
    PT_STR_ADDR,  // const char* 字符串 + 地址（"content" @0xaddr）
    PT_SIZE,      // size_t 十进制
    PT_FD,        // 文件描述符
    PT_FLAGS,     // 标志位 十六进制
};

struct ParamDesc {
    const char* name;
    ParamType type;
};

struct FuncSig {
    const char* name;
    uint8_t param_count;
    ParamDesc params[8];
};

// ============================================================
// 已知函数签名表
// ============================================================

static const FuncSig g_func_sigs[] = {
    // ---- 字符串 ----
    {"strlen",   1, {{"s", PT_STR_ADDR}}},
    {"strcmp",    2, {{"s1", PT_STR}, {"s2", PT_STR}}},
    {"strncmp",  3, {{"s1", PT_STR}, {"s2", PT_STR}, {"n", PT_SIZE}}},
    {"strcpy",   2, {{"dst", PT_HEX}, {"src", PT_STR}}},
    {"strncpy",  3, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}}},
    {"strstr",   2, {{"haystack", PT_STR}, {"needle", PT_STR}}},
    {"strchr",   2, {{"s", PT_STR}, {"c", PT_INT}}},
    {"strrchr",  2, {{"s", PT_STR}, {"c", PT_INT}}},
    {"strcat",   2, {{"dst", PT_HEX}, {"src", PT_STR}}},
    {"strncat",  3, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}}},
    {"strdup",   1, {{"s", PT_STR}}},
    {"strtok",   2, {{"s", PT_STR}, {"delim", PT_STR}}},
    {"strerror", 1, {{"errnum", PT_INT}}},
    {"strtol",   3, {{"nptr", PT_STR}, {"endptr", PT_HEX}, {"base", PT_INT}}},
    {"strtoul",  3, {{"nptr", PT_STR}, {"endptr", PT_HEX}, {"base", PT_INT}}},

    // ---- 字符串 __*_chk (Android fortified libc) ----
    {"__strlen_chk",   2, {{"s", PT_STR_ADDR}, {"bufsize", PT_HEX}}},
    {"__strcpy_chk",   3, {{"dst", PT_HEX}, {"src", PT_STR}, {"bufsize", PT_HEX}}},
    {"__strncpy_chk",  4, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"__strcat_chk",   3, {{"dst", PT_HEX}, {"src", PT_STR}, {"bufsize", PT_HEX}}},
    {"__strncat_chk",  4, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"__strlcpy_chk",  4, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"__strlcat_chk",  4, {{"dst", PT_HEX}, {"src", PT_STR}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},

    // ---- 内存 ----
    {"memcpy",   3, {{"dst", PT_HEX}, {"src", PT_HEX}, {"n", PT_SIZE}}},
    {"memmove",  3, {{"dst", PT_HEX}, {"src", PT_HEX}, {"n", PT_SIZE}}},
    {"memset",   3, {{"s", PT_HEX}, {"c", PT_INT}, {"n", PT_SIZE}}},
    {"memcmp",   3, {{"s1", PT_HEX}, {"s2", PT_HEX}, {"n", PT_SIZE}}},

    // ---- 内存 __*_chk ----
    {"__memcpy_chk",   4, {{"dst", PT_HEX}, {"src", PT_HEX}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"__memmove_chk",  4, {{"dst", PT_HEX}, {"src", PT_HEX}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"__memset_chk",   4, {{"s", PT_HEX}, {"c", PT_INT}, {"n", PT_SIZE}, {"bufsize", PT_HEX}}},
    {"mmap",     6, {{"addr", PT_HEX}, {"length", PT_SIZE}, {"prot", PT_FLAGS}, {"flags", PT_FLAGS}, {"fd", PT_FD}, {"offset", PT_HEX}}},
    {"mmap64",   6, {{"addr", PT_HEX}, {"length", PT_SIZE}, {"prot", PT_FLAGS}, {"flags", PT_FLAGS}, {"fd", PT_FD}, {"offset", PT_HEX}}},
    {"munmap",   2, {{"addr", PT_HEX}, {"length", PT_SIZE}}},
    {"mprotect", 3, {{"addr", PT_HEX}, {"len", PT_SIZE}, {"prot", PT_FLAGS}}},

    // ---- 分配 ----
    {"malloc",   1, {{"size", PT_SIZE}}},
    {"calloc",   2, {{"nmemb", PT_SIZE}, {"size", PT_SIZE}}},
    {"realloc",  2, {{"ptr", PT_HEX}, {"size", PT_SIZE}}},
    {"free",     1, {{"ptr", PT_HEX}}},
    {"memalign", 2, {{"alignment", PT_SIZE}, {"size", PT_SIZE}}},

    // ---- 文件 I/O ----
    {"open",     3, {{"pathname", PT_STR}, {"flags", PT_FLAGS}, {"mode", PT_FLAGS}}},
    {"openat",   4, {{"dirfd", PT_FD}, {"pathname", PT_STR}, {"flags", PT_FLAGS}, {"mode", PT_FLAGS}}},
    {"close",    1, {{"fd", PT_FD}}},
    {"read",     3, {{"fd", PT_FD}, {"buf", PT_HEX}, {"count", PT_SIZE}}},
    {"write",    3, {{"fd", PT_FD}, {"buf", PT_HEX}, {"count", PT_SIZE}}},
    {"pread64",  4, {{"fd", PT_FD}, {"buf", PT_HEX}, {"count", PT_SIZE}, {"offset", PT_INT}}},
    {"pwrite64", 4, {{"fd", PT_FD}, {"buf", PT_HEX}, {"count", PT_SIZE}, {"offset", PT_INT}}},
    {"lseek",    3, {{"fd", PT_FD}, {"offset", PT_INT}, {"whence", PT_INT}}},
    {"fopen",    2, {{"filename", PT_STR}, {"mode", PT_STR}}},
    {"fclose",   1, {{"stream", PT_HEX}}},
    {"fread",    4, {{"ptr", PT_HEX}, {"size", PT_SIZE}, {"nmemb", PT_SIZE}, {"stream", PT_HEX}}},
    {"fwrite",   4, {{"ptr", PT_HEX}, {"size", PT_SIZE}, {"nmemb", PT_SIZE}, {"stream", PT_HEX}}},
    {"fgets",    3, {{"s", PT_HEX}, {"size", PT_INT}, {"stream", PT_HEX}}},
    {"fputs",    2, {{"s", PT_STR}, {"stream", PT_HEX}}},
    {"fflush",   1, {{"stream", PT_HEX}}},
    {"stat",     2, {{"pathname", PT_STR}, {"statbuf", PT_HEX}}},
    {"fstat",    2, {{"fd", PT_FD}, {"statbuf", PT_HEX}}},
    {"access",   2, {{"pathname", PT_STR}, {"mode", PT_INT}}},
    {"unlink",   1, {{"pathname", PT_STR}}},
    {"rename",   2, {{"oldpath", PT_STR}, {"newpath", PT_STR}}},
    {"mkdir",    2, {{"pathname", PT_STR}, {"mode", PT_FLAGS}}},

    // ---- 格式化 ----
    {"printf",   1, {{"format", PT_STR}}},
    {"sprintf",  2, {{"str", PT_HEX}, {"format", PT_STR}}},
    {"snprintf", 3, {{"str", PT_HEX}, {"size", PT_SIZE}, {"format", PT_STR}}},
    {"fprintf",  2, {{"stream", PT_HEX}, {"format", PT_STR}}},
    {"puts",     1, {{"s", PT_STR}}},

    // ---- 格式化 __*_chk ----
    {"__sprintf_chk",   4, {{"str", PT_HEX}, {"flag", PT_INT}, {"slen", PT_SIZE}, {"format", PT_STR}}},
    {"__snprintf_chk",  5, {{"str", PT_HEX}, {"size", PT_SIZE}, {"flag", PT_INT}, {"slen", PT_SIZE}, {"format", PT_STR}}},
    {"__vsnprintf_chk", 5, {{"str", PT_HEX}, {"size", PT_SIZE}, {"flag", PT_INT}, {"slen", PT_SIZE}, {"format", PT_STR}}},

    // ---- 动态链接 ----
    {"dlopen",   2, {{"filename", PT_STR}, {"flags", PT_FLAGS}}},
    {"dlsym",    2, {{"handle", PT_HEX}, {"symbol", PT_STR}}},
    {"dlclose",  1, {{"handle", PT_HEX}}},
    {"dladdr",   2, {{"addr", PT_HEX}, {"info", PT_HEX}}},
    {"dlerror",  0, {}},

    // ---- 线程 ----
    {"pthread_create",       4, {{"thread", PT_HEX}, {"attr", PT_HEX}, {"start_routine", PT_HEX}, {"arg", PT_HEX}}},
    {"pthread_join",         2, {{"thread", PT_HEX}, {"retval", PT_HEX}}},
    {"pthread_mutex_lock",   1, {{"mutex", PT_HEX}}},
    {"pthread_mutex_unlock", 1, {{"mutex", PT_HEX}}},
    {"pthread_mutex_init",   2, {{"mutex", PT_HEX}, {"attr", PT_HEX}}},
    {"pthread_key_create",   2, {{"key", PT_HEX}, {"destructor", PT_HEX}}},
    {"pthread_setspecific",  2, {{"key", PT_INT}, {"value", PT_HEX}}},
    {"pthread_getspecific",  1, {{"key", PT_INT}}},

    // ---- 进程 ----
    {"fork",     0, {}},
    {"vfork",    0, {}},
    {"execve",   3, {{"pathname", PT_STR}, {"argv", PT_HEX}, {"envp", PT_HEX}}},
    {"exit",     1, {{"status", PT_INT}}},
    {"_exit",    1, {{"status", PT_INT}}},
    {"abort",    0, {}},
    {"kill",     2, {{"pid", PT_INT}, {"sig", PT_INT}}},
    {"getpid",   0, {}},
    {"getuid",   0, {}},

    // ---- 其他 ----
    {"getenv",   1, {{"name", PT_STR}}},
    {"setenv",   3, {{"name", PT_STR}, {"value", PT_STR}, {"overwrite", PT_INT}}},
    {"system",   1, {{"command", PT_STR}}},
    {"atoi",     1, {{"nptr", PT_STR}}},
    {"atol",     1, {{"nptr", PT_STR}}},
    {"abs",      1, {{"j", PT_INT}}},
    {"rand",     0, {}},
    {"srand",    1, {{"seed", PT_INT}}},
    {"time",     1, {{"tloc", PT_HEX}}},
    {"sleep",    1, {{"seconds", PT_INT}}},
    {"usleep",   1, {{"usec", PT_INT}}},
    {"nanosleep",2, {{"req", PT_HEX}, {"rem", PT_HEX}}},
    {"ioctl",    3, {{"fd", PT_FD}, {"request", PT_HEX}, {"arg", PT_HEX}}},
    {"prctl",    5, {{"option", PT_INT}, {"arg2", PT_HEX}, {"arg3", PT_HEX}, {"arg4", PT_HEX}, {"arg5", PT_HEX}}},
    {"madvise",  3, {{"addr", PT_HEX}, {"length", PT_SIZE}, {"advice", PT_INT}}},

    // ---- Android ----
    {"__system_property_get",  2, {{"name", PT_STR}, {"value", PT_HEX}}},
    {"__system_property_find", 1, {{"name", PT_STR}}},
    {"__android_log_print",    3, {{"prio", PT_INT}, {"tag", PT_STR}, {"fmt", PT_STR}}},
    {"__android_log_write",    3, {{"prio", PT_INT}, {"tag", PT_STR}, {"text", PT_STR}}},

    // ---- 信号 ----
    {"signal",    2, {{"signum", PT_INT}, {"handler", PT_HEX}}},
    {"sigaction", 3, {{"signum", PT_INT}, {"act", PT_HEX}, {"oldact", PT_HEX}}},
    {"raise",     1, {{"sig", PT_INT}}},
};

static const size_t g_func_sig_count = sizeof(g_func_sigs) / sizeof(g_func_sigs[0]);

// ============================================================
// 签名查找表（懒初始化）
// ============================================================

static std::unordered_map<std::string, const FuncSig*> g_sig_map;
static std::once_flag g_sig_map_init;

static void initSigMap() {
    g_sig_map.reserve(g_func_sig_count * 2);
    for (size_t i = 0; i < g_func_sig_count; i++) {
        g_sig_map[g_func_sigs[i].name] = &g_func_sigs[i];
    }
}

static inline const FuncSig* findFuncSig(const char* name) {
    std::call_once(g_sig_map_init, initSigMap);
    auto it = g_sig_map.find(name);
    return (it != g_sig_map.end()) ? it->second : nullptr;
}

// appendCStr 已移至 SafeMemRead.h

// ============================================================
// 签名库地址范围缓存 — 只对 libc/libdl/liblog 等做签名查找
// ============================================================

struct SigLibRange { uint64_t base; uint64_t end; };
static constexpr int kMaxSigLibs = 4;
static SigLibRange s_sig_libs[kMaxSigLibs];
static int s_sig_lib_count = 0;
static bool s_sig_libs_inited = false;
static int s_sig_libs_init_lock = 0;

static int phdr_find_lib_end_cb(struct dl_phdr_info* info, size_t, void* data) {
    auto* range = (SigLibRange*)data;
    if ((uint64_t)info->dlpi_addr != range->base) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uint64_t seg_end = range->base + info->dlpi_phdr[i].p_vaddr
                             + info->dlpi_phdr[i].p_memsz;
            if (seg_end > range->end) range->end = seg_end;
        }
    }
    return 1;
}

static void addSigLibBySymbol(const char* sym) {
    if (s_sig_lib_count >= kMaxSigLibs) return;
    void* p = dlsym(RTLD_DEFAULT, sym);
    if (!p) return;
    Dl_info info;
    if (!dladdr(p, &info) || !info.dli_fbase) return;
    uint64_t base = (uint64_t)info.dli_fbase;
    for (int i = 0; i < s_sig_lib_count; i++) {
        if (s_sig_libs[i].base == base) return;
    }
    SigLibRange& entry = s_sig_libs[s_sig_lib_count];
    entry = {base, base};
    dl_iterate_phdr(phdr_find_lib_end_cb, &entry);
    s_sig_lib_count++;
}

static void initSigLibRanges() {
    addSigLibBySymbol("malloc");              // libc
    addSigLibBySymbol("dlopen");              // libdl（Android 新版可能和 libc 合并）
    addSigLibBySymbol("__android_log_print"); // liblog
    s_sig_libs_inited = true;
}

static inline __attribute__((always_inline)) bool isKnownSigLib(uint64_t addr) {
    if (__builtin_expect(!s_sig_libs_inited, false)) {
        while (__sync_lock_test_and_set(&s_sig_libs_init_lock, 1)) __builtin_arm_wfe();
        if (!s_sig_libs_inited) initSigLibRanges();
        __sync_lock_release(&s_sig_libs_init_lock);
    }
    for (int i = 0; i < s_sig_lib_count; i++) {
        if (addr >= s_sig_libs[i].base && addr < s_sig_libs[i].end) return true;
    }
    return false;
}

// ============================================================
// 安全 buffer dump — 读取内存并输出可打印内容
// ============================================================

static void safeDumpBuf(vm_context* uc, StringBuilder* sb, uint64_t addr, size_t len) {
    if (addr == 0 || addr < 0x1000 || len == 0) return;
    static constexpr size_t MAX_DUMP = 128;
    size_t dump_len = len > MAX_DUMP ? MAX_DUMP : len;
    uint8_t tmp[MAX_DUMP];
    ssize_t got = 0;

    if (safe_host_read(addr, tmp, dump_len)) {
        got = (ssize_t)dump_len;
    }
    if (got <= 0) return;

    // 判断是否像文本：超过 70% 可打印字符就输出为字符串
    int printable = 0;
    for (ssize_t i = 0; i < got; i++) {
        if ((tmp[i] >= 32 && tmp[i] <= 126) || tmp[i] == '\n' || tmp[i] == '\r' || tmp[i] == '\t')
            printable++;
    }
    appendStringN(sb, "    [BUF] ", 10);
    if (printable * 10 >= got * 7) {
        appendChar(sb, '"');
        for (ssize_t i = 0; i < got; i++) {
            uint8_t c = tmp[i];
            if (c == '\n') { appendStringN(sb, "\\n", 2); }
            else if (c == '\r') { appendStringN(sb, "\\r", 2); }
            else if (c == '\t') { appendStringN(sb, "\\t", 2); }
            else if (c == '"') { appendStringN(sb, "\\\"", 2); }
            else if (c >= 32 && c <= 126) { appendChar(sb, (char)c); }
            else { appendChar(sb, '.'); }
        }
        if (len > MAX_DUMP) appendStringN(sb, "...", 3);
        appendStringN(sb, "\"\n", 2);
    } else {
        // 二进制数据输出 hex
        char hex[3];
        size_t hex_limit = got > 32 ? 32 : (size_t)got;
        for (size_t i = 0; i < hex_limit; i++) {
            if (i > 0) appendChar(sb, ' ');
            byteToHex(tmp[i], hex);
            hex[2] = '\0';
            appendStringN(sb, hex, 2);
        }
        if (len > 32) appendStringN(sb, " ...", 4);
        appendChar(sb, '\n');
    }
}

// ============================================================
// 指针参数的模块归属标注 —— [CALL] 里把地址标成 (so+0xoff) / (anon perms)
// ============================================================
// [CALL] 是 per-external-call（非 per-instruction），可承受一次二分查找。
// 自带按地址升序的 maps 快照，命中直接二分；缺失时【不】刷新（避免 /proc/self/maps
// 反复重解析），改为每 256 次查找刷新一次，新加载模块最终会被纳入。
struct PtrRegion {
    uint64_t start;
    uint64_t end;
    uint64_t module_base; // 命名模块：该 SO 最低段起始(=ELF 加载基址)；匿名段=start
    short    perms;       // bit0=r bit1=w bit2=x
    bool     named;       // 路径以 '/' 开头（真实文件映射）
    char     label[128];  // 命名=basename；匿名=[stack]/[anon:xxx]，纯匿名为空
};
static std::vector<PtrRegion> g_ptr_regions; // 按 start 升序
static int      g_ptr_regions_lock = 0;
static uint32_t g_regions_built_gen = 0xFFFFFFFF;   // g_ptr_regions 是在哪个 maps generation 建的

static void rebuildPtrRegions() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    std::vector<PtrRegion> v;
    v.reserve(512);
    std::unordered_map<std::string, uint64_t> firstBase; // 完整路径 → 最低 start
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uint64_t s, e;
        char perms[8] = {0}, off[24], dev[16], ino[24], path[512];
        path[0] = '\0';
        int n = sscanf(line, "%lx-%lx %7s %23s %15s %23s %511[^\n]",
                       &s, &e, perms, off, dev, ino, path);
        if (n < 6) continue; // 至少 addr+perms+offset+dev+inode；path 可选(n==6=纯匿名)
        char* pth = path;
        while (*pth == ' ') pth++; // 去前导空格

        PtrRegion r;
        r.start = s; r.end = e;
        r.perms = (short)((strchr(perms,'r')?1:0)|(strchr(perms,'w')?2:0)|(strchr(perms,'x')?4:0));
        r.named = (pth[0] == '/');
        const char* lbl = pth;
        if (r.named) { const char* sl = strrchr(pth, '/'); lbl = sl ? sl + 1 : pth; }
        size_t ll = strlen(lbl);
        if (ll >= sizeof(r.label)) ll = sizeof(r.label) - 1;
        memcpy(r.label, lbl, ll); r.label[ll] = '\0';

        if (r.named) {
            auto it = firstBase.find(pth);
            if (it == firstBase.end()) { firstBase.emplace(pth, s); r.module_base = s; }
            else r.module_base = it->second;
        } else {
            r.module_base = s;
        }
        v.push_back(r);
    }
    fclose(f);
    // maps 本身按地址升序，无需再排序
    g_ptr_regions.swap(v);
}

static const PtrRegion* findPtrRegion(uint64_t addr) {
    size_t lo = 0, hi = g_ptr_regions.size();
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        const PtrRegion& r = g_ptr_regions[mid];
        if (addr < r.start)      hi = mid;
        else if (addr >= r.end)  lo = mid + 1;
        else return &r;
    }
    return nullptr;
}

// 只在内存真的变过(VM 的 g_maps_generation 前进)或首次时重建区域表；平时零重扫、零 /proc 解析。
// VM 每次刷新 maps 缓存(拦到 mmap/mprotect/munmap 后 + 缺页兜底)就 ++g_maps_generation。
// 必须持有 g_ptr_regions_lock 调用。
static inline void ensureRegionsFresh_locked() {
    uint32_t g = vc_maps_generation();
    if (g_ptr_regions.empty() || g_regions_built_gen != g) {
        rebuildPtrRegions();
        g_regions_built_gen = g;
    }
}

// 把疑似指针的值标注模块归属，追加到 sb（如 " (libfoo.so+0x1234)" / " (anon rwx)" /
// " ([stack] rw-)"）。非指针(过小)或未映射 → 不追加。
static void appendPtrAnnotation(StringBuilder* sb, uint64_t rawval) {
    uint64_t addr = rawval & 0x00FFFFFFFFFFFFFFULL; // 剥 TBI/MTE 顶字节(Scudo 0xb4.. tag)
    if (addr < 0x100000) return;                    // 太小 → flag/小整数而非指针

    while (__sync_lock_test_and_set(&g_ptr_regions_lock, 1)) { __builtin_arm_wfe(); }
    ensureRegionsFresh_locked();
    const PtrRegion* r = findPtrRegion(addr);
    if (r) {
        appendStringN(sb, " (", 2);
        if (r->named) {
            appendStringN(sb, r->label, strlen(r->label));
            appendStringN(sb, "+0x", 3);
            char b[17];
            int hn = uint64ToHex(addr - r->module_base, b);
            appendStringN(sb, b, hn);
        } else {
            // 匿名段：有 [stack]/[heap]/[anon:xxx] 标签就直接用，否则记 "anon"，都带权限。
            // 可执行的匿名段(rwx/r-x)最值得注意 —— 常是 JIT / 脱壳后代码。
            if (r->label[0] == '[') appendStringN(sb, r->label, strlen(r->label));
            else                    appendStringN(sb, "anon", 4);
            char pp[3];
            pp[0] = (r->perms & 1) ? 'r' : '-';
            pp[1] = (r->perms & 2) ? 'w' : '-';
            pp[2] = (r->perms & 4) ? 'x' : '-';
            appendChar(sb, ' ');
            appendStringN(sb, pp, 3);
        }
        appendChar(sb, ')');
    }
    __sync_lock_release(&g_ptr_regions_lock);
}

// ---- 逐指令热路径:数据地址 → 区域标注（只区域，不查符号，不 dladdr）----
// 性能:thread_local 上次命中缓存(存副本,rebuild 后不悬空)→ 循环打同一段 O(1) 无锁;
// miss 才取锁二分。不主动刷 maps(靠 appendPtrAnnotation 走 [CALL] 时的周期刷新保新鲜),
// 只在快照为空时 build 一次。
void appendRegionTag(StringBuilder* sb, uint64_t rawaddr) {
    uint64_t addr = rawaddr & 0x00FFFFFFFFFFFFFFULL;   // 剥 TBI/MTE 顶字节
    if (addr < 0x1000) return;                          // 太小 → 非地址

    static thread_local uint64_t tl_s = 1, tl_e = 0, tl_base = 0;   // 上次命中区间(初始空)
    static thread_local short    tl_p = 0;
    static thread_local bool     tl_named = false;
    static thread_local char     tl_lbl[64] = {0};
    static thread_local SymbolTable* tl_tbl = nullptr;   // 命名 SO 的符号表(随区域缓存,不 dladdr)
    static thread_local uint32_t tl_gen = 0xFFFFFFFF;    // TLS 建立时的 maps generation

    // 热路径:多读一个 g_maps_generation(一次 load,近 0)。gen 没变且地址还在上次区间内 → O(1)。
    // gen 变了(内存真的发生过 mmap/mprotect/munmap)才作废 TLS、走下面重取,顺带反映权限变化。
    uint32_t cur_gen = vc_maps_generation();
    if (!(cur_gen == tl_gen && addr >= tl_s && addr < tl_e)) {   // TLS 失效 → 取锁
        while (__sync_lock_test_and_set(&g_ptr_regions_lock, 1)) { __builtin_arm_wfe(); }
        ensureRegionsFresh_locked();                    // 只有 gen 变了才真重建
        const PtrRegion* r = findPtrRegion(addr);
        if (!r) { __sync_lock_release(&g_ptr_regions_lock); return; }  // 未映射 → 不标
        tl_s = r->start; tl_e = r->end; tl_p = r->perms; tl_named = r->named; tl_base = r->module_base;
        size_t ll = strlen(r->label); if (ll >= sizeof(tl_lbl)) ll = sizeof(tl_lbl) - 1;
        memcpy(tl_lbl, r->label, ll); tl_lbl[ll] = '\0';
        __sync_lock_release(&g_ptr_regions_lock);
        tl_gen = cur_gen;
        // 符号表只在区域切换时取一次(loadSymbolTableForSo 按 base 缓存,不 dladdr)
        tl_tbl = tl_named ? loadSymbolTableForSo(tl_base) : nullptr;
    }

    appendChar(sb, ' ');
    if (tl_named) {
        appendStringN(sb, tl_lbl, strlen(tl_lbl));
        // 模块内偏移(= addr - SO 加载基址,和代码地址 so+0xOFF 同基准,没符号也能直接对 IDA)
        appendStringN(sb, "+0x", 3);
        { char ob[17]; int on = uint64ToHex(addr - tl_base, ob); appendStringN(sb, ob, on); }
        // 段类型近似(用 maps 段权限):ro=r--(rodata/const 类)、code=r-x、rw=可写数据
        const char* seg = (tl_p & 4) ? ":code" : (tl_p & 2) ? ":rw" : ":ro";
        appendStringN(sb, seg, strlen(seg));
        // 命中命名 SO 内符号 → 追加符号名。用随区域缓存的符号表 findContaining(二分,无锁、无 dladdr;
        // entries 载入后不可变、SymbolTable* 在 g_sym_cache 里稳定)。这是"地址符号化"落点:
        // 访存的是解析后的完整地址(如 &g_key),比 adrp 页基址更可能命中。
        if (tl_tbl && !tl_tbl->empty()) {
            const SymEntry* e = tl_tbl->findContaining(addr);
            if (e && !e->name.empty()) {
                appendChar(sb, ' ');
                appendStringN(sb, e->name.c_str(), e->name.size());
                uint64_t off = addr - e->addr;
                if (off) { appendStringN(sb, "+0x", 3); char b[17]; int n = uint64ToHex(off, b); appendStringN(sb, b, n); }
            }
        }
    } else {
        if (tl_lbl[0] == '[') appendStringN(sb, tl_lbl, strlen(tl_lbl));  // [stack]/[heap]/[anon:..]
        else                  appendStringN(sb, "anon", 4);
        if (tl_p & 4) appendStringN(sb, " rwx", 4);   // 可执行匿名段(JIT/脱壳代码)最值得标
    }
}

// ============================================================
// 参数格式化
// ============================================================

static inline void formatParam(vm_context* uc, StringBuilder* sb, const ParamDesc& param, uint64_t val, bool first) {
    if (!first) appendStringN(sb, ", ", 2);

    char num_buf[24];

    switch (param.type) {
        case PT_STR:
            if (val == 0) {
                appendStringN(sb, "NULL", 4);
            } else {
                appendChar(sb, '"');
                if (!appendCStr(uc, sb, val))
                    appendStringN(sb, "(bad ptr)", 9);
                appendChar(sb, '"');
            }
            break;
        case PT_STR_ADDR:
            if (val == 0) {
                appendStringN(sb, "NULL", 4);
            } else {
                appendChar(sb, '"');
                if (!appendCStr(uc, sb, val))
                    appendStringN(sb, "(bad ptr)", 9);
                appendStringN(sb, "\" @0x", 5);
                int addrLen = uint64ToHex(val, num_buf);
                appendStringN(sb, num_buf, addrLen);
            }
            break;
        case PT_INT: {
            int64_t sval = (int64_t)val;
            if (sval < 0) {
                appendChar(sb, '-');
                sval = -sval;
            }
            int len = uint64ToDec((uint64_t)sval, num_buf);
            appendStringN(sb, num_buf, len);
            break;
        }
        case PT_SIZE: {
            int len = uint64ToDec(val, num_buf);
            appendStringN(sb, num_buf, len);
            break;
        }
        case PT_FD: {
            int32_t fd = (int32_t)val;
            if (fd < 0) {
                appendChar(sb, '-');
                fd = -fd;
            }
            int len = uint64ToDec((uint64_t)fd, num_buf);
            appendStringN(sb, num_buf, len);
            break;
        }
        case PT_HEX: {
            appendStringN(sb, "0x", 2);
            int hexLen = uint64ToHex(val, num_buf);
            appendStringN(sb, num_buf, hexLen);
            appendPtrAnnotation(sb, val); // 指针参数标注模块归属
            break;
        }
        case PT_FLAGS: {
            appendStringN(sb, "0x", 2);
            int hexLen = uint64ToHex((uint32_t)val, num_buf);
            appendStringN(sb, num_buf, hexLen);
            break;
        }
    }
}

// ============================================================
// fd 参数格式化（带路径关联）
// ============================================================

static inline void formatFdWithPath(StringBuilder* sb, int32_t fd, FdTracker* tracker) {
    char num_buf[24];
    if (fd == -100) {
        appendStringN(sb, "AT_FDCWD", 8);
    } else {
        if (fd < 0) {
            appendChar(sb, '-');
            fd = -fd;
        }
        int len = uint64ToDec((uint64_t)fd, num_buf);
        appendStringN(sb, num_buf, len);
    }
    if (tracker) {
        const char* path = tracker->lookup(fd);
        if (path) {
            appendStringN(sb, " ->\"", 4);
            appendString(sb, path);
            appendChar(sb, '"');
        }
    }
}

// ============================================================
// Pending call — 等待返回值的调用
// ============================================================

enum PendingType {
    PEND_NONE = 0,
    PEND_FD_OPEN,       // open/openat → 返回 fd，建立映射
    PEND_READ_BUF,      // read/pread64 → 返回后 dump buf
    PEND_PROP_GET,      // __system_property_get → 返回后 dump value
    PEND_SPRINTF,       // sprintf/snprintf → 返回后 dump output buf
    PEND_FGETS,         // fgets → 返回后 dump buf
    PEND_GETENV,        // getenv → 返回值是字符串指针
    PEND_DLSYM,         // dlsym → 返回值是地址
    PEND_DLOPEN,        // dlopen → 返回值是 handle
    PEND_FOPEN,         // fopen → 返回值是 FILE*
    PEND_RET_INT,       // 通用：只输出返回值(int)
    PEND_RET_HEX,       // 通用：只输出返回值(hex)
    PEND_RET_STR,       // strdup/strtok 等 → 返回值是字符串指针
};

struct PendingCall {
    PendingType type = PEND_NONE;
    uint64_t arg0 = 0;      // 保存关键参数
    uint64_t arg1 = 0;
    uint64_t arg2 = 0;
    char path[256];          // 保存路径字符串副本
};

// ============================================================
// Block Hook 回调 — 外部函数调用符号打印
// ============================================================

struct FuncCallTraceCtx {
    vm_context* vmCtx;
    StringBuilder sb;
    bool sbInit = false;

    // fd 追踪表：共享指针，来自 TraceInfo 的 FdTracker（[CALL] 和 [SVC] 同源）
    FdTracker* fdTracker = nullptr;

    // 直写 buffer（来自 TraceInfo，跳过 stdio）
    DirectWriteBuf* dwBuf = nullptr;

    // 待捕获返回值
    PendingCall pending;

    // per-thread 克隆时指向 master
    FuncCallTraceCtx* master = nullptr;
};

// ============================================================
// 多线程 per-thread FuncCallTraceCtx 克隆
// ============================================================
static thread_local std::unordered_map<FuncCallTraceCtx*, FuncCallTraceCtx*> tls_fct_clones;

static int g_fct_clone_lock = 0;
static std::vector<FuncCallTraceCtx*> g_fct_all_clones;

static inline FuncCallTraceCtx* getOrCreatePerThreadFCT(FuncCallTraceCtx* master) {
    auto it = tls_fct_clones.find(master);
    if (it != tls_fct_clones.end()) {
        it->second->dwBuf = tls_trace_active_dwbuf;
        return it->second;
    }

    auto* clone = new FuncCallTraceCtx();
    clone->vmCtx = master->vmCtx;
    clone->fdTracker = master->fdTracker;
    clone->dwBuf = tls_trace_active_dwbuf;
    clone->master = master;

    tls_fct_clones[master] = clone;

    while (__sync_lock_test_and_set(&g_fct_clone_lock, 1)) { __builtin_arm_wfe(); }
    g_fct_all_clones.push_back(clone);
    __sync_lock_release(&g_fct_clone_lock);

    return clone;
}

// 处理返回值：从外部调用返回到目标 SO 时触发
static inline void flushPendingReturn(vm_context* uc, FuncCallTraceCtx* ctx) {
    PendingCall& p = ctx->pending;
    if (p.type == PEND_NONE) return;

    if (!ctx->dwBuf) { p.type = PEND_NONE; return; }

    uint64_t ret;
    vc_reg_read(uc, VC_REG_X0, &ret);

    if (!ctx->sbInit) {
        initStringBuilder(&ctx->sb);
        ctx->sbInit = true;
    }
    StringBuilder* sb = &ctx->sb;
    resetStringBuilder(sb);

    char num_buf[24];

    switch (p.type) {
        case PEND_FD_OPEN: {
            int32_t fd = (int32_t)ret;
            appendStringN(sb, "    => fd=", 10);
            if (fd < 0) {
                appendChar(sb, '-');
                int len = uint64ToDec((uint64_t)(-fd), num_buf);
                appendStringN(sb, num_buf, len);
                appendStringN(sb, " (error)\n", 9);
            } else {
                int len = uint64ToDec((uint64_t)fd, num_buf);
                appendStringN(sb, num_buf, len);
                appendChar(sb, '\n');
                if (p.path[0] && ctx->fdTracker)
                    ctx->fdTracker->add(fd, p.path);
            }
            break;
        }
        case PEND_READ_BUF: {
            int64_t bytes_read = (int64_t)ret;
            appendStringN(sb, "    => ", 7);
            if (bytes_read < 0) {
                appendStringN(sb, "-1 (error)\n", 11);
            } else {
                int len = uint64ToDec((uint64_t)bytes_read, num_buf);
                appendStringN(sb, num_buf, len);
                appendStringN(sb, " bytes\n", 7);
                if (bytes_read > 0)
                    safeDumpBuf(uc, sb, p.arg0, (size_t)bytes_read);
            }
            break;
        }
        case PEND_PROP_GET: {
            appendStringN(sb, "    => value=\"", 14);
            appendCStr(uc, sb, p.arg0);
            appendStringN(sb, "\"\n", 2);
            break;
        }
        case PEND_SPRINTF: {
            int64_t sret = (int64_t)ret;
            appendStringN(sb, "    => ", 7);
            int len = uint64ToDec(sret < 0 ? 0 : (uint64_t)sret, num_buf);
            appendStringN(sb, num_buf, len);
            appendStringN(sb, " chars: \"", 9);
            appendCStr(uc, sb, p.arg0);
            appendStringN(sb, "\"\n", 2);
            break;
        }
        case PEND_FGETS: {
            if (ret == 0) {
                appendStringN(sb, "    => NULL\n", 12);
            } else {
                appendStringN(sb, "    => \"", 8);
                appendCStr(uc, sb, p.arg0);
                appendStringN(sb, "\"\n", 2);
            }
            break;
        }
        case PEND_GETENV: {
            if (ret == 0) {
                appendStringN(sb, "    => NULL\n", 12);
            } else {
                appendStringN(sb, "    => \"", 8);
                appendCStr(uc, sb, ret);
                appendStringN(sb, "\"\n", 2);
            }
            break;
        }
        case PEND_RET_STR: {
            if (ret == 0) {
                appendStringN(sb, "    => NULL\n", 12);
            } else {
                appendStringN(sb, "    => \"", 8);
                appendCStr(uc, sb, ret);
                appendStringN(sb, "\"\n", 2);
            }
            break;
        }
        case PEND_DLSYM: {
            appendStringN(sb, "    => ", 7);
            if (ret == 0) {
                appendStringN(sb, "NULL\n", 5);
            } else {
                appendStringN(sb, "0x", 2);
                int hexLen = uint64ToHex(ret, num_buf);
                appendStringN(sb, num_buf, hexLen);
                // 反查符号
                const char* sym = lookupSymbolExact(ret);
                if (sym) {
                    appendStringN(sb, " (", 2);
                    appendString(sb, sym);
                    appendChar(sb, ')');
                }
                appendChar(sb, '\n');
            }
            break;
        }
        case PEND_DLOPEN:
        case PEND_RET_HEX: {
            appendStringN(sb, "    => 0x", 9);
            int hexLen = uint64ToHex(ret, num_buf);
            appendStringN(sb, num_buf, hexLen);
            appendChar(sb, '\n');
            break;
        }
        case PEND_FOPEN: {
            appendStringN(sb, "    => ", 7);
            if (ret == 0) {
                appendStringN(sb, "NULL (error)\n", 13);
            } else {
                appendStringN(sb, "FILE*=0x", 8);
                int hexLen = uint64ToHex(ret, num_buf);
                appendStringN(sb, num_buf, hexLen);
                appendChar(sb, '\n');
            }
            break;
        }
        case PEND_RET_INT: {
            appendStringN(sb, "    => ", 7);
            int64_t sval = (int64_t)ret;
            if (sval < 0) {
                appendChar(sb, '-');
                sval = -sval;
            }
            int len = uint64ToDec((uint64_t)sval, num_buf);
            appendStringN(sb, num_buf, len);
            appendChar(sb, '\n');
            break;
        }
        default:
            break;
    }

    if (sb->length > 0) {
        size_t len = sb->length;
        if (sb->buffer[len - 1] == '\n') len--;
        ctx->dwBuf->write(sb->buffer, len);
    }

    p.type = PEND_NONE;
}

// 剥离 __xxx_chk → xxx，方便 pending 逻辑复用基函数名匹配
static inline const char* stripChkWrapper(const char* name, char* buf, size_t bufsize) {
    if (name[0] != '_' || name[1] != '_') return name;
    size_t len = __builtin_strlen(name);
    if (len <= 6) return name;
    if (name[len-4] != '_' || name[len-3] != 'c' || name[len-2] != 'h' || name[len-1] != 'k')
        return name;
    size_t base_len = len - 6;
    if (base_len >= bufsize) return name;
    __builtin_memcpy(buf, name + 2, base_len);
    buf[base_len] = '\0';
    return buf;
}

// 分析已知函数并设置 pending + 增强参数输出
static inline void setupPendingAndEnhance(vm_context* uc, FuncCallTraceCtx* ctx,
                                   const char* func_name, StringBuilder* sb,
                                   uint64_t x[8]) {
    PendingCall& p = ctx->pending;
    p.type = PEND_NONE;
    p.path[0] = '\0';

    // __strlen_chk → strlen, __memcpy_chk → memcpy 等
    char base_buf[64];
    const char* base = stripChkWrapper(func_name, base_buf, sizeof(base_buf));

    if (str_eq(base, "open")) {
        p.type = PEND_FD_OPEN;
        safe_read_cstr(uc, x[0], p.path, sizeof(p.path));
    }
    else if (str_eq(base, "openat")) {
        p.type = PEND_FD_OPEN;
        safe_read_cstr(uc, x[1], p.path, sizeof(p.path));
    }
    else if (str_eq(base, "close")) {
        int32_t fd = (int32_t)x[0];
        if (ctx->fdTracker) ctx->fdTracker->remove(fd);
        p.type = PEND_RET_INT;
    }
    else if (str_eq(base, "read") || str_eq(base, "pread64")) {
        p.type = PEND_READ_BUF;
        p.arg0 = x[1]; // buf
    }
    else if (str_eq(base, "write") || str_eq(base, "pwrite64")) {
        p.type = PEND_RET_INT;
    }
    else if (str_eq(func_name, "__system_property_get")) {
        p.type = PEND_PROP_GET;
        p.arg0 = x[1]; // value buf
    }
    else if (str_eq(base, "sprintf")) {
        p.type = PEND_SPRINTF;
        p.arg0 = x[0]; // output buf
    }
    else if (str_eq(base, "snprintf") || str_eq(base, "vsnprintf")) {
        p.type = PEND_SPRINTF;
        p.arg0 = x[0]; // output buf
    }
    else if (str_eq(base, "fgets")) {
        p.type = PEND_FGETS;
        p.arg0 = x[0]; // buf
    }
    else if (str_eq(base, "getenv")) {
        p.type = PEND_GETENV;
    }
    else if (str_eq(base, "dlsym")) {
        p.type = PEND_DLSYM;
    }
    else if (str_eq(base, "dlopen")) {
        p.type = PEND_DLOPEN;
    }
    else if (str_eq(base, "fopen")) {
        p.type = PEND_FOPEN;
    }
    else if (str_eq(base, "malloc") || str_eq(base, "calloc")
          || str_eq(base, "realloc") || str_eq(base, "memalign")
          || str_eq(base, "mmap") || str_eq(base, "mmap64")) {
        p.type = PEND_RET_HEX;
    }
    else if (str_eq(base, "strdup") || str_eq(base, "strtok")
          || str_eq(base, "strerror") || str_eq(base, "dlerror")) {
        p.type = PEND_RET_STR;
    }
    else if (str_eq(base, "strstr") || str_eq(base, "strchr")
          || str_eq(base, "strrchr")) {
        p.type = PEND_RET_HEX;
    }
    else if (str_eq(base, "strcmp") || str_eq(base, "strncmp")
          || str_eq(base, "memcmp") || str_eq(base, "access")
          || str_eq(base, "stat") || str_eq(base, "fstat")
          || str_eq(base, "atoi") || str_eq(base, "atol")
          || str_eq(base, "strlen")) {
        p.type = PEND_RET_INT;
    }
    else if (str_eq(func_name, "__system_property_find")
          || str_eq(base, "dladdr")) {
        p.type = PEND_RET_HEX;
    }

    // write/pwrite64：调用前 buf 有效，立即追加 dump
    if (str_eq(base, "write") || str_eq(base, "pwrite64")) {
        size_t count = (size_t)x[2];
        if (count > 0)
            safeDumpBuf(uc, sb, x[1], count);
    }
}

// 已知签名函数的增强格式化：fd 参数带路径关联
static inline void formatKnownCall(vm_context* uc, FuncCallTraceCtx* ctx,
                            const FuncSig* sig, const char* displayName,
                            StringBuilder* sb, uint64_t x[8]) {
    appendString(sb, "\n  [CALL] ");
    appendString(sb, displayName);
    appendChar(sb, '(');

    for (int i = 0; i < sig->param_count; i++) {
        if (i > 0) appendStringN(sb, ", ", 2);

        // fd 类型参数特殊处理：追加路径
        if (sig->params[i].type == PT_FD) {
            formatFdWithPath(sb, (int32_t)x[i], ctx->fdTracker);
        } else {
            formatParam(uc, sb, sig->params[i], x[i], true);
        }
    }
    appendChar(sb, ')');
}

// 由 external_jump / try_fast_inline 分两次回调：
//   is_return=false: 调用前，打印 [CALL] func(args)
//   is_return=true:  调用后，打印 => 返回值
// pre_x0_x7: 调用前的 X0-X7（入参），ret_x0: 调用后的 X0（返回值）。
// 由 trace_code 在 capstone 认出外部调用指令时直接调用（trace 层自己判定，不经 VCPU）：
//   is_return=false: 调用前，address=外部目标, pre_x0_x7=入参 → 打印 [CALL] name(args)
//   is_return=true:  返回时（下一条指令 addr+4），ret_x0=X0 → 补 => 返回值
void funcCallTraceEmit(void* fctCtxV, vm_context* uc,
                       uint64_t address, const char* symName,
                       const uint64_t* pre_x0_x7, uint64_t ret_x0,
                       bool is_return) {
    auto* masterOrSelf = (FuncCallTraceCtx*)fctCtxV;
    auto* ctx = (masterOrSelf->dwBuf == nullptr && tls_trace_active_dwbuf != nullptr)
        ? getOrCreatePerThreadFCT(masterOrSelf)
        : masterOrSelf;

    if (!ctx->dwBuf) return;

    if (!ctx->sbInit) {
        initStringBuilder(&ctx->sb);
        ctx->sbInit = true;
    }
    StringBuilder* sb = &ctx->sb;
    resetStringBuilder(sb);

    if (!is_return) {
        // ---- 调用前：打印 [CALL] func(args) ----
        uint64_t x[8];
        if (pre_x0_x7) memcpy(x, pre_x0_x7, 8 * sizeof(uint64_t));
        else memset(x, 0, sizeof(x));

        if (symName && isKnownSigLib(address)) {
            // libc/libdl/liblog — 走签名表格式化 + pending 返回值增强
            const FuncSig* sig = findFuncSig(symName);
            if (sig) {
                formatKnownCall(uc, ctx, sig, symName, sb, x);
                setupPendingAndEnhance(uc, ctx, symName, sb, x);
            } else {
                appendString(sb, "\n  [CALL] ");
                appendString(sb, symName);
                appendChar(sb, '(');
                char hex[17];
                for (int i = 0; i < 4; i++) {
                    if (i > 0) appendStringN(sb, ", ", 2);
                    appendStringN(sb, "0x", 2);
                    int hexLen = uint64ToHex(x[i], hex);
                    appendStringN(sb, hex, hexLen);
                    appendPtrAnnotation(sb, x[i]);
                }
                appendChar(sb, ')');
            }
        } else {
            // 无符号或非签名库 — 解析 SO 名 + 偏移，或 demangle 已有符号
            appendString(sb, "\n  [CALL] ");
            if (symName) {
                const std::string& pretty = demangleSymbol(symName);
                appendStringN(sb, pretty.data(), pretty.size());
            } else {
                LibraryInfo soInfo = findModuleByAddressExact(address);
                if (!soInfo.name.empty()) {
                    appendStringN(sb, soInfo.name.data(), soInfo.name.size());
                    appendStringN(sb, "+0x", 3);
                    char hex[17];
                    int hexLen = uint64ToHex(address - soInfo.base_address, hex);
                    appendStringN(sb, hex, hexLen);
                } else {
                    appendStringN(sb, "sub_", 4);
                    char hex[17];
                    int hexLen = uint64ToHex(address, hex);
                    appendStringN(sb, hex, hexLen);
                }
            }
            appendChar(sb, '(');
            char hex[17];
            for (int i = 0; i < 4; i++) {
                if (i > 0) appendStringN(sb, ", ", 2);
                appendStringN(sb, "0x", 2);
                int hexLen = uint64ToHex(x[i], hex);
                appendStringN(sb, hex, hexLen);
                appendPtrAnnotation(sb, x[i]);
            }
            appendChar(sb, ')');
        }
        if (sb->length > 0) {
            ctx->dwBuf->write(sb->buffer, sb->length);
        }
    } else {
        // ---- 调用后：打印 => 返回值 ----
        bool had_pending = ctx->pending.type != PEND_NONE;
        if (had_pending) {
            flushPendingReturn(uc, ctx);
        } else {
            // 未知函数 / 无特殊处理的函数：输出通用返回值
            char hex[17];
            appendStringN(sb, "    => 0x", 9);
            int hexLen = uint64ToHex(ret_x0, hex);
            appendStringN(sb, hex, hexLen);
            ctx->dwBuf->write(sb->buffer, sb->length);
        }
    }
}

// ============================================================
// 公开 API
// ============================================================

void* addFuncCallTrace(vm_context** vmContext, FdTracker* shared_fd_tracker, DirectWriteBuf* dwBuf) {
    auto* ctx = new FuncCallTraceCtx();
    ctx->vmCtx = *vmContext;
    ctx->fdTracker = shared_fd_tracker;
    ctx->dwBuf = dwBuf;
    // [CALL] 检测已全部在 trace_code 里用 capstone 做（认出调用指令 → 解析目标 → 打印参数），
    // 由 trace_code 直接调 funcCallTraceEmit。trace 不再往 VCPU 结构体挂任何回调。
    return ctx;
}

void freeFuncCallTraceCtx(void* ptr) {
    auto* ctx = (FuncCallTraceCtx*)ptr;
    if (!ctx) return;

    tls_fct_clones.erase(ctx);

    // 清理所有 per-thread 克隆
    while (__sync_lock_test_and_set(&g_fct_clone_lock, 1)) { __builtin_arm_wfe(); }
    for (auto it = g_fct_all_clones.begin(); it != g_fct_all_clones.end(); ) {
        auto* clone = *it;
        if (clone->master == ctx) {
            if (clone->sbInit) freeStringBuilder(&clone->sb);
            delete clone;
            it = g_fct_all_clones.erase(it);
        } else {
            ++it;
        }
    }
    __sync_lock_release(&g_fct_clone_lock);

    if (ctx->sbInit) freeStringBuilder(&ctx->sb);
    delete ctx;
}
