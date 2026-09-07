#include "ARM64Emulator.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <asm/unistd.h>
#include "svc_handler.h"
#include "SafeMemRead.h"
#include "StringUtil.h"

// syscall 名称表,记录svc的名字，打印的时候会输出文件，

typedef struct {
    uint32_t nr;
    const char *name;       
} SyscallEntry;

static SyscallEntry syscall_table[] = {
    { __NR_io_setup, "io_setup" },
    { __NR_io_destroy, "io_destroy" },
    { __NR_io_submit, "io_submit" },
    { __NR_io_cancel, "io_cancel" },
    { __NR_io_getevents, "io_getevents" },
    { __NR_setxattr, "setxattr" },
    { __NR_getxattr, "getxattr" },
    { __NR_listxattr, "listxattr" },
    { __NR_removexattr, "removexattr" },
    { __NR_getcwd, "getcwd" },
    { __NR_eventfd2, "eventfd2" },
    { __NR_epoll_create1, "epoll_create1" },
    { __NR_epoll_ctl, "epoll_ctl" },
    { __NR_epoll_pwait, "epoll_pwait" },
    { __NR_dup, "dup" },
    { __NR_dup3, "dup3" },
    { __NR_inotify_init1, "inotify_init1" },
    { __NR_inotify_add_watch, "inotify_add_watch" },
    { __NR_inotify_rm_watch, "inotify_rm_watch" },
    { __NR_ioctl, "ioctl" },
    { __NR_flock, "flock" },
    { __NR_mknodat, "mknodat" },
    { __NR_mkdirat, "mkdirat" },
    { __NR_unlinkat, "unlinkat" },
    { __NR_symlinkat, "symlinkat" },
    { __NR_linkat, "linkat" },
    { __NR_renameat, "renameat" },
    { __NR_fallocate, "fallocate" },
    { __NR_faccessat, "faccessat" },
    { __NR_chdir, "chdir" },
    { __NR_fchdir, "fchdir" },
    { __NR_fchmod, "fchmod" },
    { __NR_fchmodat, "fchmodat" },
    { __NR_fchownat, "fchownat" },
    { __NR_fchown, "fchown" },
    { __NR_openat, "openat" },
    { __NR_close, "close" },
    { __NR_pipe2, "pipe2" },
    { __NR_getdents64, "getdents64" },
    { __NR_read, "read" },
    { __NR_write, "write" },
    { __NR_readv, "readv" },
    { __NR_writev, "writev" },
    { __NR_pread64, "pread64" },
    { __NR_pwrite64, "pwrite64" },
    { __NR_readlinkat, "readlinkat" },
    { __NR_sync, "sync" },
    { __NR_fsync, "fsync" },
    { __NR_fdatasync, "fdatasync" },
    { __NR_futex, "futex" },
    { __NR_nanosleep, "nanosleep" },
    { __NR_exit, "exit" },
    { __NR_exit_group, "exit_group" },
    { __NR_set_tid_address, "set_tid_address" },
    { __NR_kill, "kill" },
    { __NR_tkill, "tkill" },
    { __NR_tgkill, "tgkill" },
    { __NR_rt_sigaction, "rt_sigaction" },
    { __NR_rt_sigprocmask, "rt_sigprocmask" },
    { __NR_rt_sigreturn, "rt_sigreturn" },
    { __NR_clock_settime, "clock_settime" },
    { __NR_clock_gettime, "clock_gettime" },
    { __NR_clock_getres, "clock_getres" },
    { __NR_clock_nanosleep, "clock_nanosleep" },
    { __NR_ptrace, "ptrace" },
    { __NR_sched_yield, "sched_yield" },
    { __NR_uname, "uname" },
    { __NR_getrlimit, "getrlimit" },
    { __NR_setrlimit, "setrlimit" },
    { __NR_prctl, "prctl" },
    { __NR_getcpu, "getcpu" },
    { __NR_gettimeofday, "gettimeofday" },
    { __NR_getpid, "getpid" },
    { __NR_getppid, "getppid" },
    { __NR_getuid, "getuid" },
    { __NR_geteuid, "geteuid" },
    { __NR_getgid, "getgid" },
    { __NR_getegid, "getegid" },
    { __NR_gettid, "gettid" },
    { __NR_sysinfo, "sysinfo" },
    { __NR_socket, "socket" },
    { __NR_socketpair, "socketpair" },
    { __NR_bind, "bind" },
    { __NR_listen, "listen" },
    { __NR_accept, "accept" },
    { __NR_connect, "connect" },
    { __NR_getsockname, "getsockname" },
    { __NR_getpeername, "getpeername" },
    { __NR_sendto, "sendto" },
    { __NR_recvfrom, "recvfrom" },
    { __NR_setsockopt, "setsockopt" },
    { __NR_getsockopt, "getsockopt" },
    { __NR_shutdown, "shutdown" },
    { __NR_sendmsg, "sendmsg" },
    { __NR_recvmsg, "recvmsg" },
    { __NR_brk, "brk" },
    { __NR_munmap, "munmap" },
    { __NR_mremap, "mremap" },
    { __NR_clone, "clone" },
    { __NR_execve, "execve" },
    { __NR_mprotect, "mprotect" },
    { __NR_madvise, "madvise" },
    { __NR_mincore, "mincore" },
    { __NR_mlock, "mlock" },
    { __NR_munlock, "munlock" },
    { __NR_getrandom, "getrandom" },
    { __NR_memfd_create, "memfd_create" },
    { __NR_seccomp, "seccomp" },
    { __NR_statx, "statx" },
    { __NR_clone3, "clone3" },
    { __NR_close_range, "close_range" },
    { __NR_openat2, "openat2" },
    { __NR_faccessat2, "faccessat2" },
    { __NR_prlimit64, "prlimit64" },
    { __NR_wait4, "wait4" },
    { __NR_accept4, "accept4" },
    { __NR_lseek, "lseek" },
    { __NR_fcntl, "fcntl" },
    { __NR_ftruncate, "ftruncate" },
    { __NR_ppoll, "ppoll" },
    { __NR_newfstatat, "newfstatat" },
    { __NR_set_robust_list, "set_robust_list" },
    { __NR_sigaltstack, "sigaltstack" },
    { __NR_sched_getaffinity, "sched_getaffinity" },
    { __NR_sched_setaffinity, "sched_setaffinity" },
    { __NR_rt_sigtimedwait, "rt_sigtimedwait" },
    { __NR_setitimer, "setitimer" },
    { __NR_getitimer, "getitimer" },
    { __NR_timerfd_create, "timerfd_create" },
    { __NR_timerfd_settime, "timerfd_settime" },
    { __NR_timerfd_gettime, "timerfd_gettime" },
    { __NR_signalfd4, "signalfd4" },
    { __NR_epoll_pwait2, "epoll_pwait2" },
    { __NR_unlinkat, "unlinkat" },
    { __NR_renameat2, "renameat2" },
    { __NR_sendfile, "sendfile" },
    { __NR_truncate, "truncate" },
    { __NR_mkdirat, "mkdirat" },
    { __NR_umask, "umask" },
    { __NR_getrusage, "getrusage" },
    { __NR_times, "times" },
    { __NR_getgroups, "getgroups" },
    { __NR_setgroups, "setgroups" },
    { __NR_setsid, "setsid" },
    { __NR_getpgid, "getpgid" },
    { __NR_setpgid, "setpgid" },
    { __NR_getsid, "getsid" },
    { __NR_setreuid, "setreuid" },
    { __NR_setregid, "setregid" },
    { __NR_setresuid, "setresuid" },
    { __NR_setresgid, "setresgid" },
    { __NR_getresuid, "getresuid" },
    { __NR_getresgid, "getresgid" },
    { __NR_mmap, "mmap" },
};

static size_t syscall_table_count = sizeof(syscall_table) / sizeof(syscall_table[0]);

static const char *syscall_name(uint32_t nr) {
    for (size_t i = 0; i < syscall_table_count; i++)
        if (syscall_table[i].nr == nr)
            return syscall_table[i].name;
    return nullptr;
}

// ============================================================
// 辅助：输出 fd 参数（带路径关联）
// ============================================================

static void appendFd(StringBuilder* sb, int32_t fd, FdTracker* tracker) {
    char num[24];
    if (fd == -100) {
        appendStringN(sb, "AT_FDCWD", 8);
        return;
    }
    if (fd < 0) {
        appendChar(sb, '-');
        fd = -fd;
    }
    int len = uint64ToDec((uint64_t)fd, num);
    appendStringN(sb, num, len);
    if (tracker) {
        const char* path = tracker->lookup(fd);
        if (path) {
            appendStringN(sb, " ->\"", 4);
            appendString(sb, path);
            appendChar(sb, '"');
        }
    }
}

static void appendHexVal(StringBuilder* sb, uint64_t val) {
    appendStringN(sb, "0x", 2);
    char hex[17];
    int len = uint64ToHex(val, hex);
    appendStringN(sb, hex, len);
}

static void appendDecVal(StringBuilder* sb, uint64_t val) {
    char num[24];
    int len = uint64ToDec(val, num);
    appendStringN(sb, num, len);
}

static void appendSignedDecVal(StringBuilder* sb, int64_t val) {
    if (val < 0) {
        appendChar(sb, '-');
        val = -val;
    }
    appendDecVal(sb, (uint64_t)val);
}

// ============================================================
// 安全 buffer dump（同 FuncCallTrace 的 safeDumpBuf）
// ============================================================

static void svcDumpBuf(vm_context* uc, StringBuilder* sb, uint64_t addr, size_t len) {
    if (addr == 0 || addr < 0x1000 || len == 0) return;
    static constexpr size_t MAX_DUMP = 128;
    size_t dump_len = len > MAX_DUMP ? MAX_DUMP : len;
    uint8_t tmp[MAX_DUMP];
    ssize_t got = 0;

    if (safe_host_read(addr, tmp, dump_len)) {
        got = (ssize_t)dump_len;
    }
    if (got <= 0) return;

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
            if (c == '\n') appendStringN(sb, "\\n", 2);
            else if (c == '\r') appendStringN(sb, "\\r", 2);
            else if (c == '\t') appendStringN(sb, "\\t", 2);
            else if (c == '"') appendStringN(sb, "\\\"", 2);
            else if (c >= 32 && c <= 126) appendChar(sb, (char)c);
            else appendChar(sb, '.');
        }
        if (len > MAX_DUMP) appendStringN(sb, "...", 3);
        appendStringN(sb, "\"\n", 2);
    } else {
        char hex[3];
        size_t hex_limit = got > 32 ? 32 : (size_t)got;
        for (size_t i = 0; i < hex_limit; i++) {
            if (i > 0) appendChar(sb, ' ');
            byteToHex(tmp[i], hex);
            appendStringN(sb, hex, 2);
        }
        if (len > 32) appendStringN(sb, " ...", 4);
        appendChar(sb, '\n');
    }
}

// ============================================================
// SVC 格式化输出
// ============================================================

void svc_handle(vm_context *uc, uint64_t nr, StringBuilder *sb,
                FdTracker* fd_tracker, SvcPending* pending) {
    uint64_t x[6];
    for (int i = 0; i < 6; i++)
        vc_reg_read_fast(uc, (vc_reg)(VC_REG_X0 + i), &x[i]);

    const char *name = syscall_name((uint32_t)nr);

    appendStringN(sb, "\n  [SVC] ", 9);
    if (name) {
        appendString(sb, name);
    } else {
        appendStringN(sb, "syscall_", 8);
        appendDecVal(sb, nr);
    }
    appendChar(sb, '(');

    // 默认 pending
    if (pending) {
        pending->type = SVC_PEND_GENERIC;
        pending->path[0] = '\0';
    }

    switch ((uint32_t)nr) {
        case __NR_openat:
        case __NR_openat2: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf[256];
            const char* p = "(bad ptr)";
            ssize_t plen = safe_read_cstr(uc, x[1], path_buf, sizeof(path_buf));
            if (plen >= 0) p = path_buf;
            appendString(sb, p);
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[2]);
            if (pending) {
                pending->type = SVC_PEND_FD_OPEN;
                if (plen >= 0) {
                    size_t cp = (size_t)plen < sizeof(pending->path) - 1 ? (size_t)plen : sizeof(pending->path) - 1;
                    memcpy(pending->path, path_buf, cp);
                    pending->path[cp] = '\0';
                } else {
                    pending->path[0] = '\0';
                }
            }
            break;
        }
        case __NR_close: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            if (fd_tracker) fd_tracker->remove((int)x[0]);
            break;
        }
        case __NR_read:
        case __NR_pread64: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            if (pending) {
                pending->type = SVC_PEND_READ_BUF;
                pending->buf_addr = x[1];
            }
            break;
        }
        case __NR_write:
        case __NR_pwrite64: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            appendStringN(sb, ")\n", 2);
            svcDumpBuf(uc, sb, x[1], (size_t)x[2]);
            return;
        }
        case __NR_readlinkat: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf[256];
            ssize_t plen = safe_read_cstr(uc, x[1], path_buf, sizeof(path_buf));
            appendString(sb, plen >= 0 ? path_buf : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[2]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[3]);
            if (pending) {
                pending->type = SVC_PEND_READ_BUF;
                pending->buf_addr = x[2];
            }
            break;
        }
        case __NR_faccessat:
        case __NR_faccessat2: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf[256];
            ssize_t plen = safe_read_cstr(uc, x[1], path_buf, sizeof(path_buf));
            appendString(sb, plen >= 0 ? path_buf : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_fchmodat:
        case __NR_fchownat: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf[256];
            ssize_t plen = safe_read_cstr(uc, x[1], path_buf, sizeof(path_buf));
            appendString(sb, plen >= 0 ? path_buf : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_statx: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf[256];
            ssize_t plen = safe_read_cstr(uc, x[1], path_buf, sizeof(path_buf));
            appendString(sb, plen >= 0 ? path_buf : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[2]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[3]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[4]);
            break;
        }
        case __NR_ioctl: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_mprotect: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_munmap: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            break;
        }
        case __NR_madvise: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_prctl: {
            appendDecVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[3]);
            break;
        }
        case __NR_socket: {
            appendDecVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_connect: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_sendto: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[3]);
            appendStringN(sb, ")\n", 2);
            svcDumpBuf(uc, sb, x[1], (size_t)x[2]);
            return;
        }
        case __NR_recvfrom: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            if (pending) {
                pending->type = SVC_PEND_READ_BUF;
                pending->buf_addr = x[1];
            }
            break;
        }
        case __NR_tgkill: {
            appendDecVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", sig=", 6);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_kill:
        case __NR_tkill: {
            appendDecVal(sb, x[0]);
            appendStringN(sb, ", sig=", 6);
            appendDecVal(sb, x[1]);
            break;
        }
        case __NR_rt_sigaction: {
            appendStringN(sb, "sig=", 4);
            appendDecVal(sb, x[0]);
            appendStringN(sb, ", act=", 6);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", oldact=", 9);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_clone:
        case __NR_clone3: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            break;
        }
        case __NR_execve: {
            appendChar(sb, '"');
            char path_buf[256];
            ssize_t plen = safe_read_cstr(uc, x[0], path_buf, sizeof(path_buf));
            appendString(sb, plen >= 0 ? path_buf : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_getcwd: {
            if (pending) {
                pending->type = SVC_PEND_READ_BUF;
                pending->buf_addr = x[0];
            }
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            break;
        }
        case __NR_getrandom: {
            if (pending) {
                pending->type = SVC_PEND_READ_BUF;
                pending->buf_addr = x[0];
            }
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_lseek: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendSignedDecVal(sb, (int64_t)x[1]);
            appendStringN(sb, ", ", 2);
            switch ((int)x[2]) {
                case 0: appendStringN(sb, "SEEK_SET", 8); break;
                case 1: appendStringN(sb, "SEEK_CUR", 8); break;
                case 2: appendStringN(sb, "SEEK_END", 8); break;
                default: appendDecVal(sb, x[2]); break;
            }
            break;
        }
        case __NR_mmap: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            // prot flags
            uint64_t prot = x[2];
            if (prot == 0) {
                appendStringN(sb, "NONE", 4);
            } else {
                bool first = true;
                if (prot & 1) { appendStringN(sb, "R", 1); first = false; }
                if (prot & 2) { if (!first) appendChar(sb, '|'); appendStringN(sb, "W", 1); first = false; }
                if (prot & 4) { if (!first) appendChar(sb, '|'); appendStringN(sb, "X", 1); }
            }
            appendStringN(sb, ", ", 2);
            // map flags
            uint64_t flags = x[3];
            bool has = false;
            if (flags & 0x01) { appendStringN(sb, "SHARED", 6); has = true; }
            if (flags & 0x02) { if (has) appendChar(sb, '|'); appendStringN(sb, "PRIVATE", 7); has = true; }
            if (flags & 0x10) { if (has) appendChar(sb, '|'); appendStringN(sb, "FIXED", 5); has = true; }
            if (flags & 0x20) { if (has) appendChar(sb, '|'); appendStringN(sb, "ANON", 4); has = true; }
            if (!has) appendHexVal(sb, flags);
            appendStringN(sb, ", ", 2);
            if ((int32_t)x[4] == -1) appendStringN(sb, "-1", 2);
            else appendFd(sb, (int32_t)x[4], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[5]);
            break;
        }
        case __NR_fcntl: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            switch ((int)x[1]) {
                case 0: appendStringN(sb, "F_DUPFD", 7); break;
                case 1: appendStringN(sb, "F_GETFD", 7); break;
                case 2: appendStringN(sb, "F_SETFD", 7); break;
                case 3: appendStringN(sb, "F_GETFL", 7); break;
                case 4: appendStringN(sb, "F_SETFL", 7); break;
                case 5: appendStringN(sb, "F_GETLK", 7); break;
                case 6: appendStringN(sb, "F_SETLK", 7); break;
                case 7: appendStringN(sb, "F_SETLKW", 8); break;
                default: appendDecVal(sb, x[1]); break;
            }
            if (x[1] == 2 || x[1] == 4) {
                appendStringN(sb, ", ", 2);
                appendHexVal(sb, x[2]);
            }
            break;
        }
        case __NR_ftruncate: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            break;
        }
        case __NR_newfstatat: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", \"", 3);
            char path_buf2[256];
            ssize_t plen2 = safe_read_cstr(uc, x[1], path_buf2, sizeof(path_buf2));
            appendString(sb, plen2 >= 0 ? path_buf2 : "(bad ptr)");
            appendStringN(sb, "\", ", 3);
            appendHexVal(sb, x[2]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[3]);
            break;
        }
        case __NR_ppoll: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[2]);
            break;
        }
        case __NR_futex: {
            appendHexVal(sb, x[0]);
            appendStringN(sb, ", ", 2);
            int op = (int)x[1] & 0x7f;
            switch (op) {
                case 0: appendStringN(sb, "WAIT", 4); break;
                case 1: appendStringN(sb, "WAKE", 4); break;
                case 9: appendStringN(sb, "WAIT_BITSET", 11); break;
                case 10: appendStringN(sb, "WAKE_BITSET", 11); break;
                default: appendDecVal(sb, x[1]); break;
            }
            if (x[1] & 0x80) appendStringN(sb, "|PRIVATE", 8);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            break;
        }
        case __NR_getdents64: {
            appendFd(sb, (int32_t)x[0], fd_tracker);
            appendStringN(sb, ", ", 2);
            appendHexVal(sb, x[1]);
            appendStringN(sb, ", ", 2);
            appendDecVal(sb, x[2]);
            break;
        }
        default: {
            // 通用：输出前 4 个参数 hex
            for (int i = 0; i < 4; i++) {
                if (i > 0) appendStringN(sb, ", ", 2);
                appendHexVal(sb, x[i]);
            }
            break;
        }
    }

    appendStringN(sb, ")\n", 2);
}

void svc_flush_pending(StringBuilder* sb, uint64_t x0,
                       FdTracker* fd_tracker, SvcPending* pending) {
    if (!pending || pending->type == SVC_PEND_NONE) return;

    char num[24];

    switch (pending->type) {
        case SVC_PEND_FD_OPEN: {
            int32_t fd = (int32_t)x0;
            appendStringN(sb, "    => fd=", 10);
            if (fd < 0) {
                appendChar(sb, '-');
                int len = uint64ToDec((uint64_t)(-fd), num);
                appendStringN(sb, num, len);
                appendStringN(sb, " (error)\n", 9);
            } else {
                int len = uint64ToDec((uint64_t)fd, num);
                appendStringN(sb, num, len);
                appendChar(sb, '\n');
                if (fd_tracker && pending->path[0])
                    fd_tracker->add(fd, pending->path);
            }
            break;
        }
        case SVC_PEND_READ_BUF: {
            int64_t bytes_read = (int64_t)x0;
            appendStringN(sb, "    => ", 7);
            if (bytes_read < 0) {
                appendStringN(sb, "-1 (error)\n", 11);
            } else {
                int len = uint64ToDec((uint64_t)bytes_read, num);
                appendStringN(sb, num, len);
                appendStringN(sb, " bytes\n", 7);
                // 注意：SVC 返回值已经在寄存器里了，但 buf 内容需要从内存读
                if (bytes_read > 0 && pending->buf_addr) {
                    size_t dump_len = (size_t)bytes_read > 128 ? 128 : (size_t)bytes_read;
                    uint8_t local_buf[128];
                    if (safe_host_read(pending->buf_addr, local_buf, dump_len)) {
                        const uint8_t* p = local_buf;
                        int printable = 0;
                        for (size_t i = 0; i < dump_len; i++) {
                            if ((p[i] >= 32 && p[i] <= 126) || p[i] == '\n' || p[i] == '\r' || p[i] == '\t')
                                printable++;
                        }
                        appendStringN(sb, "    [BUF] ", 10);
                        if (printable * 10 >= (int)dump_len * 7) {
                            appendChar(sb, '"');
                            for (size_t i = 0; i < dump_len; i++) {
                                uint8_t c = p[i];
                                if (c == '\n') appendStringN(sb, "\\n", 2);
                                else if (c == '\r') appendStringN(sb, "\\r", 2);
                                else if (c == '\t') appendStringN(sb, "\\t", 2);
                                else if (c == '"') appendStringN(sb, "\\\"", 2);
                                else if (c >= 32 && c <= 126) appendChar(sb, (char)c);
                                else appendChar(sb, '.');
                            }
                            if ((size_t)bytes_read > 128) appendStringN(sb, "...", 3);
                            appendStringN(sb, "\"\n", 2);
                        } else {
                            char hex[3];
                            size_t hex_limit = dump_len > 32 ? 32 : dump_len;
                            for (size_t i = 0; i < hex_limit; i++) {
                                if (i > 0) appendChar(sb, ' ');
                                byteToHex(p[i], hex);
                                appendStringN(sb, hex, 2);
                            }
                            if ((size_t)bytes_read > 32) appendStringN(sb, " ...", 4);
                            appendChar(sb, '\n');
                        }
                    }
                }
            }
            break;
        }
        case SVC_PEND_GENERIC: {
            appendStringN(sb, "    => ", 7);
            int64_t sval = (int64_t)x0;
            if (sval < 0) {
                appendChar(sb, '-');
                int len = uint64ToDec((uint64_t)(-sval), num);
                appendStringN(sb, num, len);
            } else {
                appendHexVal(sb, x0);
            }
            appendChar(sb, '\n');
            break;
        }
        default:
            break;
    }

    pending->type = SVC_PEND_NONE;
}
