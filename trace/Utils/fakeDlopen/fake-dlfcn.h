#ifndef __FAKE_DL_FCN_H__
#define __FAKE_DL_FCN_H__

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(__arm__) || defined(__i386__)
#define LIB_DIR "lib"
#elif defined(__aarch64__) || defined(__x86_64__)
#define LIB_DIR "lib64"
#else
#error "Arch unknown, please port me"
#endif

#define SYSTEM_LIB_PATH         "/system/" LIB_DIR "/"
#define SYSTEM_VENDOR_LIB_PATH  "/system/vendor/" LIB_DIR "/"
#define VENDOR_LIB_PATH         "/vendor/" LIB_DIR "/"

void *fake_dlopen(const char *filename, int flags);
void *fake_dlsym(void *handle, const char *symbol);
int fake_dlclose(void *handle);

#if defined(__cplusplus)
}
#endif

#endif // __FAKE_DL_FCN_H__
