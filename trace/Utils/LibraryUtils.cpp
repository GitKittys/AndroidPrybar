//
// Created by ASUS on 2024-07-20.
//
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <elf.h>
#include <link.h>
#include <cstring>

#include "LibraryUtils.h"
#include "logging.h"
// 宏定义用于判断字符串是否包含特定子字符串
#define CONTAINS_SUBSTRING(str, substr) ((str).find(substr) != std::string::npos)

static std::vector<LibraryInfo> libs;
// 获取加载的库列表（每个 maps 行一个条目，用于精确的 segment 级别内存映射）
std::vector<LibraryInfo> getLoadedLibraries() {
    std::vector<LibraryInfo> libraries;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        perror("Failed to open /proc/self/maps");
        return libraries;
    }

    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start, end;
        char perms[5], offset[16], dev[16], inode[16], pathname[1024];
        short perms_short = 0;

        pathname[0] = '\0';

        if (sscanf(line, "%lx-%lx %4s %15s %15s %15s %[^\n]",
                   &start, &end, perms, offset, dev, inode, pathname) < 7) {
            continue;
        }

        if (strchr(perms, 'r')) perms_short |= 1;
        if (strchr(perms, 'w')) perms_short |= 2;
        if (strchr(perms, 'x')) perms_short |= 4;

        size_t seg_size = end - start;
        libraries.push_back({
                                    pathname,
                                    static_cast<uint64_t>(start), // base_address
                                    static_cast<uint64_t>(start), // segments_start
                                    static_cast<uint64_t>(end),   // segments_end
                                    static_cast<size_t>(seg_size),
                                    perms_short
                            });
    }

    fclose(maps);
    return libraries;
}


std::vector<LibraryInfo> findModuleByName(const char* name){
    std::vector<LibraryInfo> libraryInfo;
    if(libs.empty()){
        libs = getLoadedLibraries();
    }
    for(const LibraryInfo &lib:libs){
        if(CONTAINS_SUBSTRING(lib.name,name)){
            libraryInfo.push_back(lib);
        }
    }
    return libraryInfo;
}

/**
 * 刷新 maps 缓存，用于新加载的库
 */
void refreshLibraryCache() {
    libs.clear();
    libs = getLoadedLibraries();
}

LibraryInfo findModuleByAddress(uint64_t address){
    if (libs.empty()) {
        libs = getLoadedLibraries();
    }

    for (const LibraryInfo& lib : libs) {
        // 只匹配真实的文件映射模块（路径以 '/' 开头），
        // 跳过匿名映射（[anon:...]、[heap]、[stack] 等）和无路径的映射。
        // 这样 malloc + mprotect(PROT_EXEC) 生成的动态代码不会被误识别为模块。
        if (lib.name.empty() || lib.name[0] != '/') {
            continue;
        }
        // 使用严格的左闭右开区间 [segments_start, segments_end)
        if (address >= lib.segments_start && address < lib.segments_end) {
            return lib;
        }
    }

    // 没找到，可能是新加载的库，刷新缓存重试一次
    refreshLibraryCache();

    for (const LibraryInfo& lib : libs) {
        if (lib.name.empty() || lib.name[0] != '/') {
            continue;
        }
        if (address >= lib.segments_start && address < lib.segments_end) {
            return lib;
        }
    }

    return {};
}

/**
 * 直接从内存中的 ELF Header / Program Header 解析模块范围。
 * 这样可以避开 dl_iterate_phdr，在 VM 往返多次后也不依赖 linker 的遍历状态。
 */
static bool resolveModuleRangeFromElf(uint64_t base_address,
                                      uint64_t* segments_start,
                                      uint64_t* segments_end) {
    if (base_address == 0 || segments_start == nullptr || segments_end == nullptr) {
        return false;
    }

    auto* elf_header = reinterpret_cast<const Elf64_Ehdr*>(base_address);
    if (memcmp(elf_header->e_ident, ELFMAG, SELFMAG) != 0) {
        return false;
    }
    if (elf_header->e_ident[EI_CLASS] != ELFCLASS64 || elf_header->e_phoff == 0 ||
        elf_header->e_phnum == 0) {
        return false;
    }
    // 防御篡改过的 ELF header：e_phnum 正常 SO 不超过 20，上限 128 兜底
    if (elf_header->e_phnum > 128 || elf_header->e_phoff > 0x10000) {
        return false;
    }

    auto* program_headers =
            reinterpret_cast<const Elf64_Phdr*>(base_address + elf_header->e_phoff);
    uint64_t module_start = UINT64_MAX;
    uint64_t module_end = 0;
    for (uint16_t index = 0; index < elf_header->e_phnum; ++index) {
        if (program_headers[index].p_type != PT_LOAD) {
            continue;
        }

        uint64_t segment_start = base_address + program_headers[index].p_vaddr;
        uint64_t segment_end = segment_start + program_headers[index].p_memsz;
        if (segment_start < module_start) {
            module_start = segment_start;
        }
        if (segment_end > module_end) {
            module_end = segment_end;
        }
    }

    if (module_start == UINT64_MAX || module_end <= module_start) {
        return false;
    }

    *segments_start = module_start;
    *segments_end = module_end;
    return true;
}

/**
 * 通过 ELF program headers 查找模块，获取准确的模块级别范围
 * 返回的 LibraryInfo 的 segments_start/segments_end 是整个模块所有 LOAD segment 的范围
 */
LibraryInfo findModuleByAddressExact(uint64_t address) {
    LibraryInfo fallbackInfo = findModuleByAddress(address);
    Dl_info dlInfo{};
    if (dladdr((void*)address, &dlInfo) && dlInfo.dli_fbase != nullptr) {
        uint64_t segmentsStart = 0;
        uint64_t segmentsEnd = 0;
        if (resolveModuleRangeFromElf((uint64_t)dlInfo.dli_fbase, &segmentsStart, &segmentsEnd)) {
            // 用 dladdr 拿真实 load bias，再用内存 ELF 解析 PT_LOAD 范围。
            // 这样既保留了准确的模块基址，又避开了二次执行时容易出问题的 dl_iterate_phdr。
            LibraryInfo info;
            info.name = dlInfo.dli_fname ? dlInfo.dli_fname : fallbackInfo.name;
            info.base_address = (uint64_t)dlInfo.dli_fbase;
            info.segments_start = segmentsStart;
            info.segments_end = segmentsEnd;
            info.size = segmentsEnd - segmentsStart;
            info.permissions = fallbackInfo.permissions ? fallbackInfo.permissions : 7;
            return info;
        }
    }

    if (fallbackInfo.base_address != 0 || fallbackInfo.size != 0) {
        LibraryInfo info;
        info.name = fallbackInfo.name;
        info.base_address = fallbackInfo.base_address;
        info.segments_start = fallbackInfo.segments_start;
        info.segments_end = fallbackInfo.segments_end;
        info.size = fallbackInfo.size;
        info.permissions = fallbackInfo.permissions;
        return info;
    }

    return {};
}


const char* get_permissions_string(short perms_short) {
    static char perms[4] = ""; // 初始化为没有权限
    if (perms_short & 1) perms[0] = 'R'; // 读权限
    if (perms_short & 2) perms[1] = 'W'; // 写权限
    if (perms_short & 4) perms[2] = 'X'; // 执行权限
    perms[3] = '\0'; // 结尾字符
    return perms;
}

/**
 * dladdr 只能找到 <= 给定地址的最近符号，无法判断地址是否真的在该函数内
 * 这个函数增加了验证：检查 dli_saddr（符号起始地址）与查询地址的距离
 * 如果距离超过阈值（默认4KB，一般函数不会超过这个大小），认为不是该符号
 */
std::string findFuncSymbolName(uint64_t address){
    Dl_info info;

    if (dladdr((void*)address, &info)) {
        if(info.dli_sname && info.dli_saddr){
            uint64_t sym_addr = (uint64_t)info.dli_saddr;
            // 只有当地址在符号起始地址的合理范围内才返回
            // 一般函数大小不超过 4KB，超过说明 dladdr 返回的是前一个不相关的符号
            if(address >= sym_addr && (address - sym_addr) < 0x1000){
                return {info.dli_sname};
            }
        }
    }
    return "";
}

/**
 * 改进版 dladdr 查询：
 * 1. 验证 dli_saddr 与查询地址的距离
 * 2. 如果距离过大，清空 dli_sname 防止误判
 */
Dl_info findAddressDlInfo(uint64_t address){
    Dl_info info;
    memset(&info, 0, sizeof(Dl_info));

    if (dladdr((void*)address, &info)) {
        // 验证符号的准确性
        if(info.dli_saddr){
            uint64_t sym_addr = (uint64_t)info.dli_saddr;
            // 如果查询地址距离符号起始地址超过合理范围，清空符号名
            // 说明这个地址不在该符号的函数体内，dladdr 只是找到了最近的前一个符号
            if(address < sym_addr || (address - sym_addr) >= 0x1000){
                info.dli_sname = nullptr;
                info.dli_saddr = nullptr;
            }
        }
        return info;
    }
    return info;
}

/**
 * 精确匹配：只在地址恰好是符号入口点时返回符号名
 * 适用于 hook_block 等需要判断 "这个地址是不是某个函数的起始" 的场景
 */
Dl_info findExactSymbolAtAddress(uint64_t address){
    Dl_info info;
    memset(&info, 0, sizeof(Dl_info));

    if (dladdr((void*)address, &info)) {
        // 只有 dli_saddr 完全等于查询地址才认为匹配
        if(info.dli_saddr && (uint64_t)info.dli_saddr == address){
            return info;
        }
        // 地址不是符号入口，清空符号信息
        info.dli_sname = nullptr;
        info.dli_saddr = nullptr;
        return info;
    }
    return info;
}

std::string getPathFileName(const char* path){
    if(path== nullptr){
        return "NULL";
    }
    //strrchr是返回字符最后一次出现的位置
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        last_slash += 1;
    } else {
        last_slash = path;
    }
    return last_slash;
}
