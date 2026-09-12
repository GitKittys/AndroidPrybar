//
// Created by ASUS on 2025-11-30.
//

#ifndef UNICRONTEST_SVC_HANDLER_H
#define UNICRONTEST_SVC_HANDLER_H

#include "ARM64Emulator.h"
#include "Utils.h"
#include <unordered_map>
#include <string>

// SVC pending — 等待下一条指令捕获返回值
enum SvcPendingType {
    SVC_PEND_NONE = 0,
    SVC_PEND_FD_OPEN,       // openat → 返回 fd
    SVC_PEND_READ_BUF,      // read → dump buf
    SVC_PEND_GENERIC,       // 通用：只输出 X0 返回值
};

struct SvcPending {
    SvcPendingType type = SVC_PEND_NONE;
    uint64_t buf_addr = 0;      // read 的 buf
    char path[256];              // openat 的路径
};

// fd 追踪表（[CALL] 和 [SVC] 共享）
struct FdTracker {
    std::unordered_map<int, std::string> table;

    void add(int fd, const char* path) {
        if (fd >= 0 && path && path[0])
            table[fd] = path;
    }
    void remove(int fd) { table.erase(fd); }
    const char* lookup(int fd) const {
        auto it = table.find(fd);
        return (it != table.end()) ? it->second.c_str() : nullptr;
    }
};

void svc_handle(vm_context *uc, uint64_t nr, StringBuilder *sb,
                FdTracker* fd_tracker, SvcPending* pending);

void svc_flush_pending(StringBuilder* sb, uint64_t x0,
                       FdTracker* fd_tracker, SvcPending* pending);

#endif //UNICRONTEST_SVC_HANDLER_H
