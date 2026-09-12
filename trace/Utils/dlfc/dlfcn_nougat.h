#ifndef DLFCN_NOUGAT_H
#define DLFCN_NOUGAT_H

//see implementation in https://tech.meituan.com/2017/07/20/android-remote-debug.html


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <lsp_elf_util.h>
#include <android/log.h>
#include <string>
#include <regex>

using namespace std;

extern "C" {


int fake_dlclose(void *handle);

void *fake_dlopen(const char *filename, int flags);

void *fake_dlsym(void *handle, const char *name);

const char *fake_dlerror();


}
#endif //DLFCN_NOUGAT_H


