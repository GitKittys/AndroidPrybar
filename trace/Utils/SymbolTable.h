//
// Created by ASUS on 2026-03-16.
//

#ifndef UNICRONTEST_SYMBOLTABLE_H
#define UNICRONTEST_SYMBOLTABLE_H

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

struct SymEntry {
    uint64_t addr;       // 符号运行时绝对地址
    uint32_t size;       // ELF st_size（函数大小）
    std::string name;    // 符号名（拷贝存储，不依赖外部生命周期）
};

class SymbolTable {
    std::vector<SymEntry> entries; // 按 addr 排序
    bool loaded = false;

public:
    // 从磁盘 .so 文件解析 .dynsym（SHF_ALLOC + PT_LOAD 内），用 load_bias 换算运行时地址
    // so_base: dladdr 返回的 dli_fbase（已校验 ELF 魔数）
    bool load(uint64_t so_base);

    // 精确匹配：地址恰好是某个符号的入口点
    const char* findExact(uint64_t addr) const;

    // 范围匹配：地址在 [sym.addr, sym.addr+sym.size) 内
    const SymEntry* findContaining(uint64_t addr) const;

    // 按名称查找符号地址（线性扫描，用于初始化时预解析少量关键符号）
    uint64_t findAddrByName(const char* name) const;

    bool empty() const { return entries.empty(); }
    bool isLoaded() const { return loaded; }
    size_t count() const { return entries.size(); }
};

// ============================================================
// 全局符号缓存：按 SO base 缓存 SymbolTable，懒加载
// ============================================================

// 查找任意地址所属的符号（自动定位 SO 并懒加载符号表）
const SymEntry* lookupSymbol(uint64_t addr);

// 精确匹配：地址恰好是符号入口点时返回符号名，否则 nullptr
const char* lookupSymbolExact(uint64_t addr);

// 手动为指定 SO 建表（可在初始化时预加载）
SymbolTable* loadSymbolTableForSo(uint64_t so_base);

// 释放全局符号缓存，回收内存（trace 结束时调用）
void clearSymbolCache();

// 通过 dl_iterate_phdr 查找 SO 的 load address，再从 SymbolTable 解析符号
// so_name: 路径尾部匹配（如 "/libart.so"）
// sym_name: 符号名
// 返回运行时绝对地址，未找到返回 0
uint64_t resolveSymbolInSo(const char* so_name, const char* sym_name);

#endif //UNICRONTEST_SYMBOLTABLE_H
