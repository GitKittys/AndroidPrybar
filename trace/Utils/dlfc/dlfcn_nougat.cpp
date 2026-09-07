// Copyright (c) 2016 avs333
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
//		of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
//		to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//		copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
//		The above copyright notice and this permission notice shall be included in all
//		copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// 		AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

//fork from https://github.com/avs333/Nougat_dlfunctions
//do some modify
//support all cpu abi such as x86, x86_64
//support filename search if filename is not start with '/'

#include "dlfcn_nougat.h"
#include <sys/system_properties.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <dlfcn.h>
#include <android/log.h>
#include "lsp_elf_util.h"
#include "logging.h"
//#include <Log.h>
//#include <logging.h>


#define ANDROID_K 19
#define ANDROID_L 21
#define ANDROID_L2 22
#define ANDROID_M 23
#define ANDROID_N 24
#define ANDROID_N2 25


//Android 8.0
#define ANDROID_O 26


//Android 8.1
#define ANDROID_O2 27

//Android 9.0
#define ANDROID_P 28


//Android 10.0
#define ANDROID_Q 29

//Android 11.0
#define ANDROID_R 30



#ifdef ZHENXI_BUILD_TYPE_NOLOG

#define log_info(...)   ((void)0);
#define log_err(...)   ((void)0);
//#define log_dbg(...)


#else

#define TAG_NAME "com.zhenxi"

#define log_info(fmt, args...) __android_log_print(ANDROID_LOG_INFO, TAG_NAME, (const char *) fmt, ##args)
#define log_err(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, TAG_NAME, (const char *) fmt, ##args)
//#define log_dbg log_info

#endif


#ifdef __LP64__
#define Elf_Ehdr Elf64_Ehdr
#define Elf_Shdr Elf64_Shdr
#define Elf_Sym  Elf64_Sym
#else
#define Elf_Ehdr Elf32_Ehdr
#define Elf_Shdr Elf32_Shdr
#define Elf_Sym  Elf32_Sym
#endif


//struct ctx {
//    void *load_addr;
//    void *dynstr;
//    void *dynsym;
//    int nsyms;
//    off_t bias;
//};
struct ctx {
    void *load_addr;    // 加载基地址

    // 动态符号表相关
    Elf_Sym *dynsym;   // 动态符号表
    char *dynstr;      // 动态字符串表
    int nsyms;         // 动态符号数量

    // 完整符号表相关
    Elf_Sym *symtab;   // 完整符号表
    char *strtab;      // 完整字符串表
    int symtab_count;  // 完整符号数量

    off_t bias;        // 重定位偏移

    // 其他可能需要的信息
    void *rel_plt;     // PLT重定位表
    void *rel_dyn;     // DYNAMIC重定位表
    size_t plt_count;
    size_t dyn_count;
};

static vector<string> Split(const string &str, const string &pattern);

//extern "C" {

int fake_dlclose(void *handle) {
    if (handle) {
        struct ctx *ctx = (struct ctx *) handle;
        if (ctx->dynsym) free(ctx->dynsym);    /* we're saving dynsym and dynstr */
        if (ctx->dynstr) free(ctx->dynstr);    /* from library file just in case */
        free(ctx);
    }
    return 0;
}

#define fatal(fmt, args...) do { log_err(fmt,##args); goto err_exit; } while(0)

/* flags are ignored */
void *fake_dlopen_with_path(const char *libpath, int flags) {
    //log_info("fake_dlopen_with_path path  %s", libpath);
    FILE *maps;
    char buff[256];
    struct ctx *ctx = 0;
    off_t load_addr, size;
    int k, fd = -1, found = 0;
    char *shoff;
    auto *elf = (Elf_Ehdr *) MAP_FAILED;


    //maps = fopen("/proc/self/abcd", "r");
    //if(maps == NULL){
        maps = fopen("/proc/self/maps", "r");
    //}

    if (!maps) {
        log_err("open maps error  ");
        if (fd >= 0) close(fd);
        if (elf != MAP_FAILED) munmap(elf, size);
        fake_dlclose(ctx);
    }

    const vector<string> &vector = Split(libpath, "|");
    if (vector.size() == 2) {
        while (!found && fgets(buff, sizeof(buff), maps)) {
            if (strstr(buff, "r-xp") ) {

                //如果包含| 并且规则匹配
                if (strstr(buff, vector.at(0).c_str()) && strstr(buff, vector.at(1).c_str())) {
//c0940000-c09b7000 r-xp 00000000 fc:08 1229975                            /data/app/com.kejian.one-Ym849UWJxpgSsC27ctZ8CA==/lib/arm/libTest.so
                    auto *pString = new string(buff);

                    string path = pString->substr(pString->find("/data/app/"));

                    //去掉回车 坑！！！！！
                    size_t n = path.find_last_not_of("\r\n\t");
                    if (n != string::npos){
                        path.erase(n + 1, path.size() - n);
                    }

                    libpath = path.c_str();
                    found = 1;
                }
            }
        }
    } else {
        while (!found && fgets(buff, sizeof(buff), maps)) {

            if ((strstr(buff, "r-xp") || strstr(buff, "r--p")) && strstr(buff, libpath)) {

                found = 1;

                break;

            }

        }
    }

    fclose(maps);

    if (!found) log_err("%s runtime not found libpath ", libpath);


    if (sscanf(buff, "%lx", &load_addr) != 1) fatal("failed to read load address for %s", libpath);



    /* Now, mmap the same library once again */

    fd = open(libpath, O_RDONLY);
    if (fd < 0) fatal("fake_dlopen_with_path open error  %s  %d", libpath, fd);
//    else
//        log_info("打开文件成功 %s", libpath);


    size = lseek(fd, 0, SEEK_END);
    if (size <= 0) fatal("lseek() failed for %s", libpath);



    elf = (Elf_Ehdr *) mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    fd = -1;

    if (elf == MAP_FAILED) fatal("mmap() failed for %s", libpath);


    ctx = (struct ctx *) calloc(1, sizeof(struct ctx));
    if (!ctx) fatal("no memory for %s", libpath);



    ctx->load_addr = (void *) load_addr;
    shoff = ((char *) elf) + elf->e_shoff;

    for (k = 0; k < elf->e_shnum; k++, shoff += elf->e_shentsize) {

        Elf_Shdr *sh = (Elf_Shdr *) shoff;
        //log_dbg("%s: k=%d shdr=%p type=%x", __func__, k, sh, sh->sh_type);

        switch (sh->sh_type) {

            case SHT_DYNSYM:
                if (ctx->dynsym) fatal("%s: duplicate DYNSYM sections", libpath); /* .dynsym */
                ctx->dynsym = static_cast<Elf64_Sym *>(malloc(sh->sh_size));
                if (!ctx->dynsym) fatal("%s: no memory for .dynsym", libpath);
                memcpy(ctx->dynsym, ((char *) elf) + sh->sh_offset, sh->sh_size);
                ctx->nsyms = (sh->sh_size / sizeof(Elf_Sym));
                break;

            case SHT_STRTAB:
                if (ctx->dynstr) break;    /* .dynstr is guaranteed to be the first STRTAB */
                ctx->dynstr = static_cast<char *>(malloc(sh->sh_size));
                if (!ctx->dynstr) fatal("%s: no memory for .dynstr", libpath);
                memcpy(ctx->dynstr, ((char *) elf) + sh->sh_offset, sh->sh_size);
                break;

            case SHT_PROGBITS:
                if (!ctx->dynstr || !ctx->dynsym) break;
                /* won't even bother checking against the section name */
                ctx->bias = (off_t) sh->sh_addr - (off_t) sh->sh_offset;
                k = elf->e_shnum;  /* exit for */
                break;
        }
    }

    munmap(elf, size);
    elf = 0;

    if (!ctx->dynstr || !ctx->dynsym) fatal("dynamic sections not found in %s", libpath);

#undef fatal

    //log_dbg("%s: ok, dynsym = %p, dynstr = %p", libpath, ctx->dynsym, ctx->dynstr);

    return ctx;

    err_exit:
    if (fd >= 0) close(fd);
    if (elf != MAP_FAILED) munmap(elf, size);
    fake_dlclose(ctx);
    return 0;
}


#if defined(__LP64__)
static const char *const kSystemLibDir = "/system/lib64/";
static const char *const kOdmLibDir = "/odm/lib64/";
static const char *const kVendorLibDir = "/vendor/lib64/";
#else
static const char *const kSystemLibDir = "/system/lib/";
static const char *const kOdmLibDir = "/odm/lib/";
static const char *const kVendorLibDir = "/vendor/lib/";


#endif




void *fake_dlopen(const char *filename, int flags) {
    //log_info("start fake_dlopen");
    if ((strlen(filename) > 0 && filename[0] == '/') || (strstr(filename, "|") != nullptr)) {
        return fake_dlopen_with_path(filename, flags);
    } else {

        //尝试对系统So路径进行拼接
        char buf[512] = {0};
        void *handle = NULL;


        memset(buf, 0, sizeof(buf));
        ///system/lib/com.kejian.one|libTest.so
        strcpy(buf, kSystemLibDir);
        strcat(buf, filename);
        //  log_err("路径变为 %s", buf);
        handle = fake_dlopen_with_path(buf, flags);
        if (handle) {
            return handle;
        }

        //odm
        memset(buf, 0, sizeof(buf));
        strcpy(buf, kOdmLibDir);
        strcat(buf, filename);
        handle = fake_dlopen_with_path(buf, flags);
        if (handle) {
            return handle;
        }

        //vendor
        memset(buf, 0, sizeof(buf));
        strcpy(buf, kVendorLibDir);
        strcat(buf, filename);
        handle = fake_dlopen_with_path(buf, flags);
        if (handle) {
            return handle;
        }

        return fake_dlopen_with_path(filename, flags);
    }


}



static void *find_in_symtab(Elf_Sym *symtab, char *strtab,
                            int nsyms, void *load_addr,
                            off_t bias, const char *name) {
    for (int k = 0; k < nsyms; k++, symtab++) {
        if (!symtab->st_name) continue;

        char *symname = strtab + symtab->st_name;
        if (strcmp(symname, name) != 0) {
            continue;
        }

        // 检查符号类型和绑定属性
        unsigned char type = ELF_ST_TYPE(symtab->st_info);
        unsigned char bind = ELF_ST_BIND(symtab->st_info);

        // 过滤掉局部和非函数/对象符号
        if (bind == STB_LOCAL) continue;
        if (type != STT_FUNC && type != STT_OBJECT) continue;

        return (char *)load_addr + symtab->st_value - bias;
    }
    return NULL;
}
void *fake_dlsym(void *handle, const char *name) {
    if (!name || !handle) return NULL;

    auto *ctx = (struct ctx *)handle;
    if (!ctx || !ctx->load_addr) return NULL;

    // 尝试从动态符号表查找
    if (ctx->dynsym && ctx->dynstr) {
        void *result = find_in_symtab(ctx->dynsym, ctx->dynstr,
                                      ctx->nsyms, ctx->load_addr,
                                      ctx->bias, name);
        if (result) return result;
    }

    // 如果动态符号表找不到，尝试完整符号表（如果有）
    if (ctx->symtab && ctx->strtab && ctx->symtab_count > 0) {
        return find_in_symtab(ctx->symtab, ctx->strtab,
                              ctx->symtab_count, ctx->load_addr,
                              ctx->bias, name);
    }

    return NULL;
}
//void *fake_dlsym(void *handle, const char *name) {
//
//    if(name == NULL){
////        log_err("com.zhenxi fake_dlsym name nullptr  ");
//        return NULL;
//    }
//    if(handle== NULL){
////        log_err("com.zhenxi fake_dlsym handle nullptr %s ", name);
//        return NULL;
//    }
//
//    auto *ctx = (struct ctx *) handle;
////    ctx->load_addr
//    if(ctx== NULL){
////        log_err("com.zhenxi fake_dlsym lib handle ->ctx nullptr  %s ", name);
//        return NULL;
//    }
//
//    //指向函数符号的
//    auto *sym = (Elf_Sym *) ctx->dynsym;
//    if(sym == NULL){
////        log_err("com.zhenxi fake_dlsym sym nullptr %s ", name);
//        return NULL;
//    }
//    //指向字符串表的指针
//    char *strings = (char *) ctx->dynstr;
//    if(strings == NULL){
////        log_err("com.zhenxi fake_dlsym strings table nullptr %s ",name);
//        return NULL;
//    }
//
//    int k;
//    for (k = 0; k < ctx->nsyms; k++, sym++){
//
//        char *dysym = strings + sym->st_name;
//        LOGD(" %s ", (strings + sym->st_name))
////        if(dysym!=nullptr&&(strstr(dysym, name))){
//            LOGD("jni->%lx",  (char *) ctx->load_addr + sym->st_value - ctx->bias)
////        }
//
//        if (dysym!=nullptr&&(strcmp(dysym, name) == 0)) {
//            void *ret = (char *) ctx->load_addr + sym->st_value - ctx->bias;
//            //log_info("com.zhenxi fake_dlsym get dlsym sucess %s ", name);
//
//            return ret;
//        }
//    }
//    //log_err("com.zhenxi fake_dlsym get dlsym fail %s ,start get debug dlsym", name);
//
//    return NULL;
//}

const char *fake_dlerror() {
    return nullptr;
}

static vector<string> Split(const string &str, const string &pattern){
    vector<string> retList ;
    if (pattern.empty())
        return retList;
    size_t start = 0, index = str.find_first_of(pattern, 0);
    while (index != std::string::npos) {
        if (start != index)
            retList.push_back(str.substr(start, index - start));
        start = index + 1;
        index = str.find_first_of(pattern, start);
    }
    if (!str.substr(start).empty())
        retList.push_back(str.substr(start));
    return retList;
}