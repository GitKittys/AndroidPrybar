#include <lsp_elf_util.h>
#include <jni.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <logging.h>
#include "dlfcn_nougat.h"
#include <sys/system_properties.h>
#include <cstdlib>
#include <dlfcn.h>
#include <android/log.h>
#include <logging.h>
#include <logging.h>



#ifndef DLFCN_COMPAT_H

#define DLFCN_COMPAT_H



void *dlopen_compat(const char *filename, int flags);

void *dlsym_compat(void *handle, const char *symbol);

int dlclose_compat(void *handle);

const char *dlerror_compat();

int get_sdk_level();

void * getSymCompat(const char *filename, const char *symbol);




#endif //DLFCN_COMPAT_H
