//
// Created by ASUS on 2024-07-20.
//

#ifndef UNICRONTEST_LIBRARYUTILS_H
#define UNICRONTEST_LIBRARYUTILS_H
#include "string"
#include <dlfcn.h>
#include <vector>
enum MemoryRegionType {
    REGION_UNKNOWN,
    REGION_STACK,
    REGION_HEAP,
    REGION_SO_MAPPING,
    REGION_DATA,
    REGION_RODATA,
};

struct LibraryInfo {
    std::string name;
    uint64_t base_address;
    uint64_t segments_start;
    uint64_t segments_end;
    size_t size;
    short permissions; // 新增：权限信息

};
//struct LibraryInfo {
//    std::string path;
//    uintptr_t base;
//    size_t size;
//    short permissions;
//    std::vector<std::pair<uintptr_t, uintptr_t>> segments; // 所有段的范围
//};
std::vector<LibraryInfo> getLoadedLibraries();
std::vector<LibraryInfo> findModuleByName(const char* name);
void refreshLibraryCache();
LibraryInfo findModuleByAddress(uint64_t address);
LibraryInfo findModuleByAddressExact(uint64_t address);
std::string findFuncSymbolName(uint64_t address);
Dl_info findAddressDlInfo(uint64_t address);
Dl_info findExactSymbolAtAddress(uint64_t address);
const char* get_permissions_string(short perms_short);
std::string getPathFileName(const char* path);
#endif //UNICRONTEST_LIBRARYUTILS_H
