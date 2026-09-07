//
// Created by ASUS on 2025-01-20.
//

#ifndef UNICRONTEST_STRINGUTIL_H
#define UNICRONTEST_STRINGUTIL_H

#include <cstdlib>
#include <cstring>

//表示动态字符串构建器
typedef struct {
    char *buffer;       // 用于存储字符串数据
    size_t length;      // 当前字符串的长度
    size_t capacity;    // 缓冲区的容量
} StringBuilder;
void initStringBuilder(StringBuilder *sb);
void freeStringBuilder(StringBuilder *sb);
void resetStringBuilder(StringBuilder *sb);  // 重置长度，保留 buffer 避免 realloc
void appendString(StringBuilder *sb, const char *str);
// 追加单个字符，避免 strlen 开销（热路径优化）
static inline void appendChar(StringBuilder *sb, char c) {
    if (__builtin_expect(sb->length + 2 > sb->capacity, false)) {
        sb->capacity *= 2;
        sb->buffer = (char *)realloc(sb->buffer, sb->capacity);
    }
    sb->buffer[sb->length++] = c;
}
static inline void appendStringN(StringBuilder *sb, const char *str, size_t len) {
    if (__builtin_expect(sb->length + len + 1 > sb->capacity, false)) {
        do { sb->capacity *= 2; } while (sb->length + len + 1 > sb->capacity);
        sb->buffer = (char *)realloc(sb->buffer, sb->capacity);
    }
    char* dst = sb->buffer + sb->length;
    if (__builtin_constant_p(len)) {
        // 编译期常量长度: __builtin_memcpy 直接内联为 store 指令，零 PLT 开销
        __builtin_memcpy(dst, str, len);
    } else {
        // 运行时变量长度: 手动循环避免 PLT 间接跳转
        // trace 场景下 len 通常 < 32，循环比 PLT call 更快
        const char* s = str;
        size_t n = len;
        while (n >= 8) {
            uint64_t v;
            __builtin_memcpy(&v, s, 8);
            __builtin_memcpy(dst, &v, 8);
            dst += 8; s += 8; n -= 8;
        }
        while (n--) *dst++ = *s++;
    }
    sb->length += len;
}
const char *toString(StringBuilder *sb);

// Fast uint64_t → hex string (no leading zeros). Returns length written.
static inline int uint64ToHex(uint64_t val, char* buf) {
    static const char hd[] = "0123456789abcdef";
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[16];
    int n = 0;
    while (val) { tmp[n++] = hd[val & 0xF]; val >>= 4; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

// Fast uint32_t → fixed 8-char hex with leading zeros
static inline void uint32ToHex8(uint32_t val, char* buf) {
    static const char hd[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) { buf[i] = hd[val & 0xF]; val >>= 4; }
    buf[8] = '\0';
}

// Fast uint64_t → hex with exact `width` digits (left-padded with '0'), returns width
static inline int uint64ToHexPadded(uint64_t val, char* buf, int width) {
    static const char hd[] = "0123456789abcdef";
    for (int i = width - 1; i >= 0; i--) { buf[i] = hd[val & 0xF]; val >>= 4; }
    buf[width] = '\0';
    return width;
}

// Fast uint64_t → decimal string (no leading zeros). Returns length written.
static inline int uint64ToDec(uint64_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[20];
    int n = 0;
    while (val) { tmp[n++] = '0' + (char)(val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

// Fast byte → 2 hex chars
static inline void byteToHex(uint8_t b, char* out) {
    static const char hd[] = "0123456789abcdef";
    out[0] = hd[b >> 4];
    out[1] = hd[b & 0xF];
}

#endif //UNICRONTEST_STRINGUTIL_H
