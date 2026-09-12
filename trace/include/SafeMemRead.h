#ifndef UNICRONTEST_SAFEMEMREAD_H
#define UNICRONTEST_SAFEMEMREAD_H

//
// identity mapping 下的安全内存读取工具。
//
// 【为什么不用 mincore + memcpy】
// mincore 只检查页是否驻留物理内存（resident），不检查读权限。
// PROT_NONE 页和 execute-only（--xp）页能通过 mincore 检查，
// 但 memcpy 读取时触发 SIGSEGV（SEGV_ACCERR）。
// Scudo allocator 的 guard page（---p）和 linker 的 RELRO 段都会命中这个问题。
//
// 【为什么用 process_vm_readv】
// process_vm_readv(getpid(), ...) 通过内核路径读取自身进程内存：
//   - 权限不足返回 -1 + EFAULT，不触发信号
//   - 未映射地址同样返回 -1，不崩溃
//   - 开销：一次 syscall（~200ns），比 uc_mem_read（softmmu 翻译）快几十倍
//
// 用于替代所有 trace 层的 uc_mem_read 调用 —
// identity mapping 保证 guest 地址 == host 地址，无需走 Unicorn 的 softmmu。
//

#include "ARM64Emulator.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

static inline long cached_page_size() {
    static long ps = sysconf(_SC_PAGESIZE);
    return ps;
}

static inline pid_t cached_pid() {
    static pid_t pid = getpid();
    return pid;
}

// 检查 host 页是否驻留物理内存。
// 注意：只检查驻留，不检查读权限 — 不能用于判断是否可安全 memcpy。
static inline bool host_page_resident(uint64_t addr) {
    long ps = cached_page_size();
    uint64_t page = addr & ~(uint64_t)(ps - 1);
    unsigned char vec;
    return mincore((void*)page, (size_t)ps, &vec) == 0;
}

// identity mapping 直接读 host 内存，跳过 Unicorn softmmu。
// 用 process_vm_readv 安全读取 — 权限不足或未映射时返回 false 而不是 SIGSEGV。
static inline bool safe_host_read(uint64_t addr, void* buf, size_t len) {
    if (addr == 0 || addr < 0x1000 || len == 0) return false;
    struct iovec local  = { buf, len };
    struct iovec remote = { (void*)addr, len };
    ssize_t n = process_vm_readv(cached_pid(), &local, 1, &remote, 1, 0);
    return n == (ssize_t)len;
}

// identity mapping 安全读 C 字符串。返回读到的字节数（不含 \0），失败返回 -1。
// 逐页读取到页边界，避免单次 process_vm_readv 跨越未映射页导致整体失败。
static inline ssize_t safe_read_cstr(vm_context* uc, uint64_t addr, char* buf, size_t maxlen) {
    (void)uc;
    if (addr == 0 || addr < 0x1000 || maxlen == 0) return -1;
    // 逐块读取（每次到页边界），用 process_vm_readv 保证安全
    size_t i = 0;
    long ps = cached_page_size();
    while (i < maxlen) {
        size_t page_remain = (size_t)(ps - ((addr + i) & (ps - 1)));
        size_t chunk = maxlen - i;
        if (chunk > page_remain) chunk = page_remain;
        char tmp[4096];
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        if (!safe_host_read(addr + i, tmp, chunk)) break;
        for (size_t j = 0; j < chunk; j++) {
            buf[i] = tmp[j];
            if (tmp[j] == 0) return (ssize_t)i;
            i++;
        }
    }
    if (i > 0) { buf[i < maxlen ? i : maxlen - 1] = '\0'; return (ssize_t)i; }
    return -1;
}

// 检查地址处的内容是否看起来像 C 字符串。
static inline bool safe_looks_like_cstr(vm_context* uc, uint64_t addr, int min_printable) {
    (void)uc;
    if (addr == 0 || addr < 0x1000) return false;
    char buf[256];
    size_t to_read = sizeof(buf);
    if (!safe_host_read(addr, buf, to_read)) {
        // 首页失败直接返回
        return false;
    }
    int count = 0;
    for (size_t i = 0; i < to_read; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c == 0) break;
        if (c >= 32 && c <= 126)
            count++;
        else
            return false;
    }
    return count >= min_printable;
}

// 从 host 内存读 C 字符串，直接追加到 StringBuilder，无固定 buffer 限制。
// 逐页读取，遇到 \0 或 SAFETY_CAP(4096) 停止。返回是否读到了至少 1 字节。
#include "StringUtil.h"
static inline bool appendCStr(vm_context* uc, StringBuilder* sb, uint64_t addr) {
    (void)uc;
    if (addr == 0 || addr < 0x1000) return false;
    long ps = cached_page_size();
    size_t total = 0;
    static constexpr size_t SAFETY_CAP = 4096;
    while (total < SAFETY_CAP) {
        size_t page_remain = (size_t)(ps - ((addr + total) & (ps - 1)));
        char tmp[4096];
        size_t chunk = page_remain < sizeof(tmp) ? page_remain : sizeof(tmp);
        if (chunk > SAFETY_CAP - total) chunk = SAFETY_CAP - total;
        if (!safe_host_read(addr + total, tmp, chunk)) break;
        for (size_t j = 0; j < chunk; j++) {
            if (tmp[j] == '\0') return true;
            appendChar(sb, tmp[j]);
            total++;
        }
    }
    return total > 0;
}

#endif //UNICRONTEST_SAFEMEMREAD_H
