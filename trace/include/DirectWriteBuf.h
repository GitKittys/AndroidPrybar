#ifndef UNICRONTEST_DIRECTWRITEBUF_H
#define UNICRONTEST_DIRECTWRITEBUF_H

#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include "lz4_block.h"

#if defined(TRACE_DEBUG)
static constexpr size_t DW_BUF_SIZE = 256;
#elif defined(TRACE_NO_CACHE)
// 无刷盘缓存：128 字节，比一行还小 → 几乎每行就 LZ4 压缩落盘。
// 被杀时基本只丢 StringBuilder 里那条还没落盘的当前行（架构硬下限）。
// 长行会被拆成多个 LZ4 帧（解码无害）。不含 TRACE_DEBUG 的 LOGD 噪音。
static constexpr size_t DW_BUF_SIZE = 128;
#else
static constexpr size_t DW_BUF_SIZE = 20 * 1024 * 1024;
#endif
static constexpr int    DW_DEFAULT_PORT = 9876;

static inline ssize_t inline_write(int fd, const void* buf, size_t count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)count;
    register long x8 __asm__("x8") = 64; // __NR_write
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline ssize_t inline_send(int fd, const void* buf, size_t count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)count;
    register long x3 __asm__("x3") = 0x4000; // MSG_NOSIGNAL
    register long x4 __asm__("x4") = 0;
    register long x5 __asm__("x5") = 0;
    register long x8 __asm__("x8") = 206; // __NR_sendto
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

static inline int inline_fsync(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = 82; // __NR_fsync
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return (int)x0;
}

static inline void send_all(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        ssize_t r = inline_send(fd, p, len);
        if (r <= 0) break;
        p += r;
        len -= r;
    }
}

static inline void write_all(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        ssize_t r = inline_write(fd, p, len);
        if (r <= 0) break;
        p += r;
        len -= r;
    }
}

// TCP 模式多线程共享同一 socket，send 需要互斥
static std::mutex g_tcp_send_mutex;

struct DirectWriteBuf {
    char* bufs[2] = {nullptr, nullptr};
    char* comp_buf = nullptr;
    size_t pos = 0;
    int fd = -1;
    bool is_tcp = false;
    uint32_t tid = 0;
    int active = 0;

    // 后台 LZ4 压缩+写入线程（compressed 模式：文件和 TCP 共用）
    pthread_t bg_thread = 0;
    bool bg_running = false;
    std::mutex bg_mtx;
    std::condition_variable bg_cv;
    bool bg_pending = false;
    bool bg_shutdown = false;
    bool bg_idle = true;
    int bg_buf_idx = 0;
    size_t bg_size = 0;

    static void* bg_thread_func(void* arg) {
        ((DirectWriteBuf*)arg)->bg_worker();
        return nullptr;
    }

    void bg_worker() {
        while (true) {
            std::unique_lock<std::mutex> lk(bg_mtx);
            bg_cv.wait(lk, [this] { return bg_pending || bg_shutdown; });
            if (bg_shutdown && !bg_pending) break;

            int idx = bg_buf_idx;
            size_t size = bg_size;
            bg_pending = false;
            lk.unlock();

            int comp_size = lz4_compress_block(bufs[idx], comp_buf, (int)size,
                                               lz4_compress_bound((int)size));
            if (comp_size > 0) {
                uint32_t header[3] = {(uint32_t)comp_size, (uint32_t)size, tid};
                if (is_tcp) {
                    std::lock_guard<std::mutex> tcp_lk(g_tcp_send_mutex);
                    send_all(fd, header, 12);
                    send_all(fd, comp_buf, (size_t)comp_size);
                } else {
                    write_all(fd, header, 12);
                    write_all(fd, comp_buf, (size_t)comp_size);
                }
            }

            lk.lock();
            bg_idle = true;
            bg_cv.notify_all();
        }
    }

    // LZ4 压缩模式（单 buffer，同步压缩写入）— 本地文件用
    void init_lz4(int fileFd) {
        bufs[0] = (char*)mmap(nullptr, DW_BUF_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        size_t comp_sz = (size_t)lz4_compress_bound((int)DW_BUF_SIZE);
        comp_buf = (char*)mmap(nullptr, comp_sz, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pos = 0;
        active = 0;
        fd = fileFd;
        is_tcp = false;
        tid = (uint32_t)gettid();
    }

    // TCP 模式（双 buffer + 后台线程，LZ4 压缩 + 网络发送）
    void init_tcp(int clientFd) {
        bufs[0] = (char*)mmap(nullptr, DW_BUF_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        bufs[1] = (char*)mmap(nullptr, DW_BUF_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        size_t comp_sz = (size_t)lz4_compress_bound((int)DW_BUF_SIZE);
        comp_buf = (char*)mmap(nullptr, comp_sz, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pos = 0;
        active = 0;
        fd = clientFd;
        is_tcp = true;
        tid = (uint32_t)gettid();

        bg_pending = false;
        bg_shutdown = false;
        bg_idle = true;
        bg_running = true;
        pthread_create(&bg_thread, nullptr, bg_thread_func, this);
    }

    static int setup_tcp_server(const char* spec) {
        int port = DW_DEFAULT_PORT;
        if (spec && *spec) port = atoi(spec);
        if (port <= 0 || port > 65535) port = DW_DEFAULT_PORT;

        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) return -1;

        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sfd);
            return -1;
        }
        if (listen(sfd, 1) < 0) {
            close(sfd);
            return -1;
        }

        int cfd = accept(sfd, nullptr, nullptr);
        close(sfd);
        if (cfd < 0) return -1;

        int sndbuf = 4 * 1024 * 1024;
        setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        opt = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        return cfd;
    }

    void destroy() {
        if (bg_running) {
            if (pos > 0) flush();
            {
                std::unique_lock<std::mutex> lk(bg_mtx);
                bg_cv.wait(lk, [this] { return bg_idle; });
                bg_shutdown = true;
            }
            bg_cv.notify_all();
            pthread_join(bg_thread, nullptr);
            bg_running = false;

            for (int i = 0; i < 2; i++) {
                if (bufs[i] && bufs[i] != MAP_FAILED) munmap(bufs[i], DW_BUF_SIZE);
                bufs[i] = nullptr;
            }
            if (comp_buf && comp_buf != MAP_FAILED) {
                munmap(comp_buf, (size_t)lz4_compress_bound((int)DW_BUF_SIZE));
            }
            comp_buf = nullptr;
        } else if (bufs[0] && bufs[0] != MAP_FAILED) {
            if (pos > 0) flush();
            munmap(bufs[0], DW_BUF_SIZE);
            bufs[0] = nullptr;
            if (comp_buf && comp_buf != MAP_FAILED) {
                munmap(comp_buf, (size_t)lz4_compress_bound((int)DW_BUF_SIZE));
            }
            comp_buf = nullptr;
        }
    }

    static void send_end_frame(int fd) {
        uint32_t end_frame[3] = {0, 0, 0};
        std::lock_guard<std::mutex> lk(g_tcp_send_mutex);
        send_all(fd, end_frame, 12);
    }

    inline void flush() {
        if (pos == 0) return;
        if (bg_running) {
            std::unique_lock<std::mutex> lk(bg_mtx);
            bg_cv.wait(lk, [this] { return bg_idle; });
            bg_buf_idx = active;
            bg_size = pos;
            bg_idle = false;
            bg_pending = true;
            active = 1 - active;
            pos = 0;
            bg_cv.notify_one();
        } else {
            int comp_size = lz4_compress_block(bufs[0], comp_buf, (int)pos,
                                               lz4_compress_bound((int)pos));
            if (comp_size > 0) {
                uint32_t header[3] = {(uint32_t)comp_size, (uint32_t)pos, tid};
                write_all(fd, header, 12);
                write_all(fd, comp_buf, (size_t)comp_size);
            }
            pos = 0;
        }
    }

    // 崩溃/exit 信号处理器里调：把当前 buffer 同步 LZ4+write 直刷到 fd，**不走后台线程、不加锁、
    // 不分配**（尽量 async-signal-safe：只用预分配的 comp_buf + write 系统调用）。仅文件模式；TCP
    // 模式(bg_running)走后台线程+mutex，信号里不安全，直接跳过（TCP 本就实时流，崩溃丢的少）。
    // 尽力而为：抢救出崩溃前缓冲里那段 trace，比整段丢失强。
    inline void flushFromSignal() {
        if (bg_running || pos == 0 || fd < 0) return;
        int comp_size = lz4_compress_block(bufs[0], comp_buf, (int)pos,
                                           lz4_compress_bound((int)pos));
        if (comp_size > 0) {
            uint32_t header[3] = {(uint32_t)comp_size, (uint32_t)pos, tid};
            write_all(fd, header, 12);
            write_all(fd, comp_buf, (size_t)comp_size);
        }
        pos = 0;
    }

    // 崩溃/exit 信号里调：刷完后关掉文件 fd（数据其实 flush(write) 后就已落盘，close 只是释放句柄、
    // 语义上「关闭」）。仅文件模式；TCP 的 socket 归后台线程，不在信号里碰。
    inline void closeFromSignal() {
        if (bg_running || fd < 0) return;
        int f = fd; fd = -1;
        close(f);
    }

    // 手动 8 字节循环 memcpy，避免 PLT 调用（trace 行通常 80-200 字节）
    __attribute__((always_inline))
    static inline void inline_memcpy(char* __restrict dst, const char* __restrict src, size_t n) {
        while (n >= 8) {
            uint64_t v;
            __builtin_memcpy(&v, src, 8);
            __builtin_memcpy(dst, &v, 8);
            dst += 8; src += 8; n -= 8;
        }
        while (n--) *dst++ = *src++;
    }

    __attribute__((always_inline))
    inline void write(const char* data, size_t len) {
        char* cur = bufs[bg_running ? active : 0];
        if (pos + len <= DW_BUF_SIZE) {
            inline_memcpy(cur + pos, data, len);
            pos += len;
        } else {
            flush();
            cur = bufs[bg_running ? active : 0];
            if (len <= DW_BUF_SIZE) {
                inline_memcpy(cur, data, len);
                pos = len;
            } else {
                size_t off = 0;
                while (off < len) {
                    size_t chunk = len - off;
                    if (chunk > DW_BUF_SIZE) chunk = DW_BUF_SIZE;
                    cur = bufs[bg_running ? active : 0];
                    memcpy(cur, data + off, chunk);
                    pos = chunk;
                    flush();
                    off += chunk;
                }
            }
        }
    }
};

#endif //UNICRONTEST_DIRECTWRITEBUF_H
