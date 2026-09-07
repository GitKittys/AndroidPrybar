#include <dlfcn_compat.h>
#include "dlfcn_nougat.h"
#include <sys/system_properties.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <logging.h>
#include <lsp_elf_util.h>



static int SDK_INT = -1;
//using namespace zhenxiHook::Elf;


int get_sdk_level() {
    if (SDK_INT > 0) {
        return SDK_INT;
    }
    char sdk[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.build.version.sdk", sdk);
    SDK_INT = atoi(sdk);
    return SDK_INT;
}


int dlclose_compat(void *handle) {
    if(handle== nullptr){
        return 1;
    }
    if (get_sdk_level() >= 24) {
        return fake_dlclose(handle);
    } else {
        return dlclose(handle);
    }
}

void *dlopen_compat(const char *filename, int flags) {
    if(filename== nullptr){
        return nullptr;
    }
    if (get_sdk_level() >= 24) {
        return fake_dlopen(filename, flags);
    } else {
        return dlopen(filename, flags);
    }
}

void *dlsym_compat(void *handle, const char *symbol) {

    void* resut = nullptr;

    if (get_sdk_level() >= 24) {
        resut =  fake_dlsym(handle, symbol);
    } else {
        resut =  dlsym(handle, symbol);
    }
    return resut;
}

const char *dlerror_compat() {
    if (get_sdk_level() >= 24) {
        return fake_dlerror();
    } else {
        return dlerror();
    }
}


void* getSymCompat(const char * filename, const char *symbol){
    void *phandle = dlopen_compat(filename, RTLD_NOW);
    if(phandle==nullptr){
        //LOG(ERROR)<< "getSymCompat get handle error "<< filename;
        return nullptr;
    }

    void *pSymbol = dlsym_compat(phandle, symbol);
    if(pSymbol!=nullptr){
        return pSymbol;
    }else{
        LOGI("getSymCompat get handle error , start get debug symbol %s",symbol);
    }
    //尝试解析Debug符号,栈里面,自动释放
    ZhenxiHook::Elf::ElfImg elfImg(filename);
    pSymbol = reinterpret_cast<void *>(elfImg.getSymAddress(symbol));
    if(pSymbol!= nullptr){
        LOGE("get debug symbol sucess!  %s",symbol);
    } else{
        LOGE("get debug symbol fail !  %s",symbol);
    }
    dlclose_compat(phandle);
    return pSymbol;
}




