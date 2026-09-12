//
// Created by ASUS on 2025-01-20.
//


#include <cstring>
#include <cstdlib>
#include "StringUtil.h"
#include "logging.h"

#define INITIAL_CAPACITY 512  // 初始容量 — trace 每行约 100-200 字节，512 可覆盖大多数情况无需 realloc



// 初始化字符串构建器
void initStringBuilder(StringBuilder *sb) {
    sb->buffer = (char *)malloc(INITIAL_CAPACITY * sizeof(char));  // 分配初始内存
    sb->length = 0;
    sb->capacity = INITIAL_CAPACITY;
    sb->buffer[0] = '\0';  // 确保初始为空字符串
}

// 追加字符串 — 手动计算长度，避免 strlen/strcpy PLT 开销
void appendString(StringBuilder *sb, const char *str) {
    size_t str_len = 0;
    while (str[str_len]) str_len++;
    appendStringN(sb, str, str_len);
}

const char *toString(StringBuilder *sb) {
    if (sb->buffer) sb->buffer[sb->length] = '\0';
    return sb->buffer;
}

// 释放内存
void freeStringBuilder(StringBuilder *sb) {
    free(sb->buffer);  // 释放动态分配的内存
    sb->buffer = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

// 重置长度，保留已分配的 buffer 避免重复 malloc/realloc
void resetStringBuilder(StringBuilder *sb) {
    sb->length = 0;
    if (sb->buffer) {
        sb->buffer[0] = '\0';
    }
}