//
// Created by Administrator on 2024/9/23.
// Comprehensive JNI Trace for Unicorn VM
//

#include "ARM64Emulator.h"
#include "logging.h"
#include <unordered_map>
#include "LibraryUtils.h"
#include "SymbolTable.h"
#include <jni.h>
#include <dlfcn.h>
#include <iostream>
#include <sstream>
#include <cstdint>
#include "dlfcn_nougat.h"
#include <iomanip>
#include <mutex>
#include <inttypes.h>
#include "DirectWriteBuf.h"

// EastTrace.cpp 导出的 per-thread 活跃 DirectWriteBuf
extern thread_local DirectWriteBuf* tls_trace_active_dwbuf;

// ===========================================================================
//  Types & Globals
// ===========================================================================
typedef void (*JNITraceFunc)(vm_context*, JNIEnv* env, uintptr_t address);

static std::unordered_map<uintptr_t, JNITraceFunc> jniTraceTable;
static std::mutex mtx;
static JavaVM* javaVm = nullptr;

// libart.so 代码段地址范围缓存，用于 block hook 快速过滤
static uint64_t artCodeStart = 0;
static uint64_t artCodeEnd   = 0;
static uint64_t last_jni_addr = 0;  // 去重: EastTrace 提前调用后不再重复触发

// ===========================================================================
//  解耦: JNI 调用不再由 handler 驱动 —— 两段式
// ===========================================================================
// 入口 block hook（traceJNI）在 JNI 函数入口触发：此刻寄存器=入参、local ref 有效，
//   handler 以 g_jni_at_return=false 跑「第一段」——解析参数写进 g_jni_oss 行缓冲、不打印。
// 真实 JNI 由 hook_block 自然 external_jump 执行完 → PC=LR。
// trace_code 执行到返回地址 g_jni_pending_ret 那一行 → jni_on_return：handler 以
//   g_jni_at_return=true 跑「第二段」——读 X0=返回值拼上、打印。
// 全程无寄存器回填、无额外 JNI local frame；引用纪律与旧版（handler 内 helper 自管理）一致。
// 关键时机：参数在入口段解析（引用有效），绝不在返回段解析（那时参数 ref 可能已被删）。
thread_local uint64_t      g_jni_pending_ret = 0;         // JNI 返回地址（0=无飞行中调用），trace_code 廉价比较
thread_local bool          g_jni_at_return   = false;     // false=入口段(解析参数) true=返回段(读返回值)
static thread_local std::ostringstream g_jni_oss;         // 跨两段的行缓冲
static thread_local JNITraceFunc  s_jni_handler = nullptr;
static thread_local vm_context* s_jni_ctx     = nullptr;
static thread_local uintptr_t     s_jni_addr    = 0;
static thread_local JNIEnv*       s_jni_env      = nullptr;
static thread_local uint64_t      s_jni_lr       = 0;     // 入口快照 LR（format_lr 用，返回段 LR 已被破坏）

// ===========================================================================
//  Method / Field Info Cache
//  Populated by GetMethodID / GetStaticMethodID / GetFieldID / GetStaticFieldID
// ===========================================================================
struct MethodInfo {
    std::string className;
    std::string methodName;
    std::string signature;
};
static std::unordered_map<uintptr_t, MethodInfo> g_method_cache;
static std::mutex g_method_cache_lock;

struct FieldInfo {
    std::string className;
    std::string fieldName;
    std::string typeSig;
};
static std::unordered_map<uintptr_t, FieldInfo> g_field_cache;
static std::mutex g_field_cache_lock;

static void cacheMethod(jmethodID mid, const std::string& cls,
                         const std::string& name, const std::string& sig) {
    std::lock_guard<std::mutex> lk(g_method_cache_lock);
    g_method_cache[(uintptr_t)mid] = {cls, name, sig};
}

static MethodInfo getCachedMethod(jmethodID mid) {
    std::lock_guard<std::mutex> lk(g_method_cache_lock);
    auto it = g_method_cache.find((uintptr_t)mid);
    if (it != g_method_cache.end()) return it->second;
    return {"", "", ""};
}

static void cacheField(jfieldID fid, const std::string& cls,
                        const std::string& name, const std::string& sig) {
    std::lock_guard<std::mutex> lk(g_field_cache_lock);
    g_field_cache[(uintptr_t)fid] = {cls, name, sig};
}

static FieldInfo getCachedField(jfieldID fid) {
    std::lock_guard<std::mutex> lk(g_field_cache_lock);
    auto it = g_field_cache.find((uintptr_t)fid);
    if (it != g_field_cache.end()) return it->second;
    return {"", "?", ""};
}

void* proxy_Jni(vm_context* ctx, void* jni_addr, ...);

// ===========================================================================
//  JNI 异常隔离 — trace 自己的日志 JNI 调用不能踩到 app 的 pending exception
//  ART 几乎所有 JNI 入口都 CHECK(!IsExceptionPending())，
//  如果 app 正常抛了异常（如 SecurityException），trace 再调 FindClass 就会 abort。
//  用 RAII 在 helper 入口保存+清除，退出时原样还原。
// ===========================================================================
struct JniExceptionGuard {
    vm_context* ctx;
    JNIEnv* env;
    jthrowable saved;

    JniExceptionGuard(vm_context* c, JNIEnv* e) : ctx(c), env(e), saved(nullptr) {
        saved = (jthrowable)proxy_Jni(ctx, (void*)env->functions->ExceptionOccurred, env);
        if (saved) proxy_Jni(ctx, (void*)env->functions->ExceptionClear, env);
    }

    ~JniExceptionGuard() {
        if ((jboolean)(uintptr_t)proxy_Jni(ctx, (void*)env->functions->ExceptionCheck, env))
            proxy_Jni(ctx, (void*)env->functions->ExceptionClear, env);
        if (saved) {
            proxy_Jni(ctx, (void*)env->functions->Throw, env, saved);
            proxy_Jni(ctx, (void*)env->functions->DeleteLocalRef, env, saved);
        }
    }

    JniExceptionGuard(const JniExceptionGuard&) = delete;
    JniExceptionGuard& operator=(const JniExceptionGuard&) = delete;
};

static inline void jni_delete_local_ref(vm_context* ctx, JNIEnv* env, jobject ref) {
    if (ref) proxy_Jni(ctx, (void*)env->functions->DeleteLocalRef, env, ref);
}

struct JniLocalFrame {
    vm_context* ctx; JNIEnv* env; bool active;
    JniLocalFrame(vm_context* c, JNIEnv* e, jint cap) : ctx(c), env(e),
        active((jint)(intptr_t)proxy_Jni(c, (void*)e->functions->PushLocalFrame, e, cap) == 0) {}
    ~JniLocalFrame() { if (active) proxy_Jni(ctx, (void*)env->functions->PopLocalFrame, env, nullptr); }
    JniLocalFrame(const JniLocalFrame&) = delete;
    JniLocalFrame& operator=(const JniLocalFrame&) = delete;
};

// 如果想在vm中调用jni函数，获取某些jni的值，那可以用这个，必须要切换到host栈才能调用jni。
void* proxy_Jni(vm_context* ctx, void* jni_addr, ...) {
    u_int64_t jni_args[8] = {0};
    va_list ap;
    va_start(ap, jni_addr);
    for (unsigned long & jni_arg : jni_args) {
        jni_arg = va_arg(ap, uint64_t);
    }
    va_end(ap);

    u_int64_t vm_sp, jni_ret;
    vc_reg_read(ctx, VC_REG_SP, &vm_sp);

    asm volatile(
        "mov x19, sp\n"
        "mov sp, %[vm_sp]\n"
        "sub sp, sp, #0x100\n"
        "mov x9, %[jni_args]\n"
        "ldp x0, x1, [x9, #0]\n"
        "ldp x2, x3, [x9, #16]\n"
        "ldp x4, x5, [x9, #32]\n"
        "ldp x6, x7, [x9, #48]\n"
        "mov x17, %[jni_addr]\n"
        "blr x17\n"
        "mov %[jni_ret], x0\n"
        "mov sp, x19\n"
        : [jni_ret] "=&r"(jni_ret)
        : [vm_sp] "r"(vm_sp),
          [jni_addr] "r"(jni_addr),
          [jni_args] "r"(jni_args)
        : "memory",
          "x0","x1","x2","x3","x4","x5","x6","x7",
          "x9","x17","x19","x30"
    );
    return (void*)jni_ret;
}

// 通过jclass获取class的name
static std::string getJClassName(vm_context* ctx, JNIEnv* env, jclass clazz) {
    if (!clazz) return "null";
    JniExceptionGuard _guard(ctx, env);

    jclass classClass = (jclass)proxy_Jni(ctx, (void*)env->functions->FindClass,
                                          env, "java/lang/Class");
    if (!classClass) return "?";

    jmethodID mid = (jmethodID)proxy_Jni(ctx, (void*)env->functions->GetMethodID,
                                          env, classClass, "getName",
                                          "()Ljava/lang/String;");
    if (!mid) { jni_delete_local_ref(ctx, env, classClass); return "?"; }

    jstring nameStr = (jstring)proxy_Jni(ctx, (void*)env->functions->CallObjectMethod,
                                          env, clazz, mid);
    if (!nameStr) { jni_delete_local_ref(ctx, env, classClass); return "?"; }

    const char* utf = (const char*)proxy_Jni(ctx, (void*)env->functions->GetStringUTFChars,
                                              env, nameStr, nullptr);
    if (!utf) {
        jni_delete_local_ref(ctx, env, nameStr);
        jni_delete_local_ref(ctx, env, classClass);
        return "?";
    }

    std::string result(utf);
    proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars, env, nameStr, utf);
    jni_delete_local_ref(ctx, env, nameStr);
    jni_delete_local_ref(ctx, env, classClass);
    return result;
}

// dot → slash for JNI internal format
static std::string dotToSlash(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c == '.') c = '/';
    return r;
}

// ===========================================================================
//  Helper: format caller address  (unidbg-style)
// ===========================================================================
static std::string format_lr(vm_context* ctx) {
    // 两段式下 format_lr 在「返回段」调用，此刻 uc 的 LR 已被 JNI 调用破坏。
    // 用入口 hook 快照的 s_jni_lr（= JNI 的调用者返回地址），与旧版在入口读 LR 语义一致。
    uint64_t lr = s_jni_lr;

    Dl_info info{};
    dladdr((void*)lr, &info);

    const char* so = info.dli_fname ? info.dli_fname : "unknown";
    const char* p = strrchr(so, '/');
    so = p ? p + 1 : so;

    std::ostringstream oss;
    oss << " @ [" << so << "]0x" << std::hex << (lr - (uint64_t)info.dli_fbase);
    return oss.str();
}

static std::string format_addr(uint64_t addr) {
    if (!addr) return "null";
    Dl_info info{};
    std::ostringstream oss;
    oss << "0x" << std::hex << addr;
    if (dladdr((void*)addr, &info) && info.dli_fbase) {
        const char* so = info.dli_fname;
        if (so) {
            const char* p = strrchr(so, '/');
            so = p ? p + 1 : so;
        } else {
            so = "?";
        }
        oss << "[" << so << "]0x" << (addr - (uint64_t)info.dli_fbase);
    }
    return oss.str();
}

// ===========================================================================
//  Logging
// ===========================================================================
static std::mutex g_log_lock;

static void logf(vm_context* ctx, const char* fmt, ...) {
    (void)ctx;
    DirectWriteBuf* dwb = tls_trace_active_dwbuf;
    if (!dwb) return;
    char buf[2048];
    int off = 0;
    buf[off++] = '\n';
    memcpy(buf + off, "    >>> ", 8);
    off += 8;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - off - 2, fmt, ap);
    va_end(ap);
    if (n > 0) off += (n < (int)(sizeof(buf) - off - 2)) ? n : (int)(sizeof(buf) - off - 2);
    dwb->write(buf, off);
}

// ===========================================================================
//  Helper: read C-string from shared VM/host memory
// ===========================================================================
static std::string read_cstr(uint64_t addr, size_t maxlen = 512) {
    if (!addr) return "null";
    return std::string((const char*)addr);
}

// ===========================================================================
//  Helper: call real JNI function via emulator
// ===========================================================================
// 解耦后：真实 JNI 调用由 hook_block 自然 external_jump 执行，trace 层不再驱动。
// 返回值在返回地址那一行由 jni_on_return 读 X0；此处留空 no-op，78 个 handler 无需改动
// （它们仍在原位置调它，只是不再做任何事）。
static inline void CallJniFunction(vm_context* ctx, uintptr_t jniPtr) {
    (void)ctx; (void)jniPtr;
}

// ===========================================================================
//  print_jstring — format a jstring for logging
// ===========================================================================
static void print_jstring(vm_context* ctx, JNIEnv* env,
                           std::ostringstream& oss, jstring js) {
    if (!js) { oss << "null"; return; }
    JniExceptionGuard _guard(ctx, env);
    const char* utf = (const char*)proxy_Jni(ctx, (void*)env->functions->GetStringUTFChars,
                                              env, js, nullptr);
    if (utf) {
        oss << "\"" << utf << "\"";
        proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars, env, js, utf);
    } else {
        oss << "jstring@0x" << std::hex << (uintptr_t)js << std::dec;
    }
}

// ===========================================================================
//  print_jobject_safe — format a jobject (class_name@hash)
// ===========================================================================
static void print_jobject_safe(vm_context* ctx, JNIEnv* env,
                                std::ostringstream& oss, jobject obj) {
    if (!obj) { oss << "null"; return; }
    JniExceptionGuard _guard(ctx, env);

    LOGD("print_jobject_safe: obj=%p", obj);
    jclass cls = (jclass)proxy_Jni(ctx, (void*)env->functions->GetObjectClass, env, obj);
    if (!cls) {
        oss << "object@0x" << std::hex << (uintptr_t)obj << std::dec;
        return;
    }

    std::string name = getJClassName(ctx, env, cls);

    // try to get hashCode for nicer formatting
    jmethodID mid_hash = (jmethodID)proxy_Jni(ctx, (void*)env->functions->GetMethodID,
                                                env, cls, "hashCode", "()I");
    if (mid_hash) {
        jint hash = (jint)(uintptr_t)proxy_Jni(ctx, (void*)env->functions->CallIntMethod,
                                                 env, obj, mid_hash);
        oss << name << "@" << std::hex << (uint32_t)hash << std::dec;
    } else {
        oss << name << "@" << std::hex << (uint32_t)(uintptr_t)obj << std::dec;
    }
    jni_delete_local_ref(ctx, env, cls);
}

// helper: print jobject or jstring smartly (checks if it's a String instance)
static void print_jobject_or_string(vm_context* ctx, JNIEnv* env,
                                     std::ostringstream& oss, jobject obj) {
    if (!obj) { oss << "null"; return; }
    JniExceptionGuard _guard(ctx, env);
    jclass strCls = (jclass)proxy_Jni(ctx, (void*)env->functions->FindClass,
                                       env, "java/lang/String");
    jboolean isStr = strCls ? (jboolean)(uintptr_t)proxy_Jni(
        ctx, (void*)env->functions->IsInstanceOf, env, obj, strCls) : JNI_FALSE;
    jni_delete_local_ref(ctx, env, strCls);
    if (isStr) {
        print_jstring(ctx, env, oss, (jstring)obj);
    } else {
        print_jobject_safe(ctx, env, oss, obj);
    }
}

// ===========================================================================
//  Reflection-based jmethodID / jfieldID resolution
//  When the cache misses (methodID obtained before trace started),
//  use ToReflectedMethod/Field to resolve name and signature.
// ===========================================================================

// Helper: get Java class name for a type descriptor (e.g., "int" -> "I", "java.lang.String" -> "Ljava/lang/String;")
static std::string classNameToSig(const std::string& name) {
    if (name == "void")    return "V";
    if (name == "boolean") return "Z";
    if (name == "byte")    return "B";
    if (name == "char")    return "C";
    if (name == "short")   return "S";
    if (name == "int")     return "I";
    if (name == "long")    return "J";
    if (name == "float")   return "F";
    if (name == "double")  return "D";
    // array type from Class.getName(): "[Ljava/lang/String;" or "[I"
    if (!name.empty() && name[0] == '[') {
        std::string r = name;
        for (char& c : r) if (c == '.') c = '/';
        return r;
    }
    // array type from PrettyMethod: "byte[]", "int[][]", "java.lang.String[]"
    if (name.size() >= 3 && name.substr(name.size() - 2) == "[]") {
        // Count array dimensions and strip "[]" suffixes
        std::string base = name;
        std::string prefix;
        while (base.size() >= 2 && base.substr(base.size() - 2) == "[]") {
            prefix += "[";
            base = base.substr(0, base.size() - 2);
        }
        return prefix + classNameToSig(base);
    }
    // object type
    std::string r = "L";
    for (char c : name) r += (c == '.') ? '/' : c;
    r += ";";
    return r;
}

// ===========================================================================
//  ART Internal: PrettyMethod / PrettyField — direct libart.so symbol call
//  jmethodID == ArtMethod*, jfieldID == ArtField* in ART
//  PrettyMethod returns e.g. "java.lang.String com.example.Foo.bar(int, long)"
//  PrettyField  returns e.g. "int com.example.Foo.count"
// ===========================================================================
using ArtPrettyMethod_t = std::string (*)(void* /*ArtMethod* this*/, bool /*with_signature*/);
using ArtPrettyField_t  = std::string (*)(void* /*ArtField*  this*/, bool /*with_type*/);

static ArtPrettyMethod_t g_ArtPrettyMethod = nullptr;
static ArtPrettyField_t  g_ArtPrettyField  = nullptr;
static bool g_art_symbols_initialized = false;

static void initArtSymbols() {
    if (g_art_symbols_initialized) return;
    g_art_symbols_initialized = true;

    // libart.so on Android 10+ lives in /apex/com.android.runtime/
    void* h = dlopen("libart.so", RTLD_NOW);
    if (!h) {
        LOGE("initArtSymbols: dlopen libart.so failed: %s", dlerror());
        return;
    }

    // art::ArtMethod::PrettyMethod(bool) — member function, ARM64: x0=this, w1=bool
    g_ArtPrettyMethod = (ArtPrettyMethod_t)dlsym(h,
        "_ZN3art9ArtMethod12PrettyMethodEb");

    // art::ArtField::PrettyField(bool) — member function
    g_ArtPrettyField = (ArtPrettyField_t)dlsym(h,
        "_ZN3art8ArtField11PrettyFieldEb");

    if (!g_ArtPrettyMethod)
        LOGW("initArtSymbols: PrettyMethod symbol not found (Android 15 stripped?)");
    if (!g_ArtPrettyField)
        LOGW("initArtSymbols: PrettyField symbol not found (Android 15 stripped?)");
}

/**
 * parsePrettyMethod — parse PrettyMethod(true) output into MethodInfo
 * Input:  "java.lang.String com.example.Foo.bar(int, java.lang.String)"
 *         "void com.example.Foo.<init>(int)"
 * Output: MethodInfo { "com/example/Foo", "bar", "(ILjava/lang/String;)Ljava/lang/String;" }
 */
static MethodInfo parsePrettyMethod(const std::string& pretty) {
    MethodInfo mi;
    if (pretty.empty()) return mi;

    // Format: "ReturnType full.class.name.method(ParamType1, ParamType2)"
    size_t parenOpen = pretty.find('(');
    if (parenOpen == std::string::npos) return mi;

    // Everything before '(' is "RetType class.method"
    std::string beforeParen = pretty.substr(0, parenOpen);

    // Find first space to separate return type from "class.method"
    size_t spacePos = beforeParen.find(' ');
    if (spacePos == std::string::npos) return mi;

    std::string retType = beforeParen.substr(0, spacePos);
    std::string classAndMethod = beforeParen.substr(spacePos + 1);

    // Last '.' separates class from method name
    size_t lastDot = classAndMethod.rfind('.');
    if (lastDot == std::string::npos) return mi;

    mi.className = classAndMethod.substr(0, lastDot);
    for (char& c : mi.className) if (c == '.') c = '/';
    mi.methodName = classAndMethod.substr(lastDot + 1);

    // Extract parameters between '(' and ')'
    size_t parenClose = pretty.rfind(')');
    std::string params;
    if (parenClose != std::string::npos && parenClose > parenOpen + 1) {
        params = pretty.substr(parenOpen + 1, parenClose - parenOpen - 1);
    }

    // Build JNI signature: "(paramSigs)retSig"
    std::string sig = "(";
    if (!params.empty()) {
        size_t start = 0;
        while (start < params.size()) {
            // Skip whitespace
            while (start < params.size() && params[start] == ' ') start++;
            if (start >= params.size()) break;

            size_t comma = params.find(',', start);
            std::string paramType;
            if (comma == std::string::npos) {
                paramType = params.substr(start);
                start = params.size();
            } else {
                paramType = params.substr(start, comma - start);
                start = comma + 1;
            }
            // Trim trailing whitespace
            while (!paramType.empty() && paramType.back() == ' ') paramType.pop_back();
            if (!paramType.empty()) {
                sig += classNameToSig(paramType);
            }
        }
    }
    sig += ")";
    sig += classNameToSig(retType);
    mi.signature = sig;

    return mi;
}

/**
 * parsePrettyField — parse PrettyField(true) output into FieldInfo
 * Input:  "int com.example.Foo.count"
 * Output: FieldInfo { "com/example/Foo", "count", "I" }
 */
static FieldInfo parsePrettyField(const std::string& pretty) {
    FieldInfo fi;
    fi.fieldName = "?";
    fi.typeSig = "?";
    if (pretty.empty()) return fi;

    // Format: "FieldType full.class.name.fieldName"
    size_t spacePos = pretty.find(' ');
    if (spacePos == std::string::npos) return fi;

    std::string fieldType = pretty.substr(0, spacePos);
    std::string rest = pretty.substr(spacePos + 1);

    size_t lastDot = rest.rfind('.');
    if (lastDot == std::string::npos) return fi;

    fi.className = rest.substr(0, lastDot);
    for (char& c : fi.className) if (c == '.') c = '/';
    fi.fieldName = rest.substr(lastDot + 1);
    fi.typeSig = classNameToSig(fieldType);

    return fi;
}

/**
 * resolveMethodByArt — 直接调用 ArtMethod::PrettyMethod() 解析方法信息
 * jmethodID 在 ART 中就是 ArtMethod* 指针，可以直接作为 this 传入
 * 比反射路径快得多：零 JNI 调用，纯 native 函数调用
 */
static MethodInfo resolveMethodByArt(jmethodID mid) {
    MethodInfo mi;
    if (!mid) return mi;

    initArtSymbols();
    if (!g_ArtPrettyMethod) return mi;

    try {
        std::string pretty = g_ArtPrettyMethod((void*)mid, true);
        if (!pretty.empty()) {
            mi = parsePrettyMethod(pretty);
            if (!mi.methodName.empty()) {
                cacheMethod(mid, mi.className, mi.methodName, mi.signature);
            }
        }
    } catch (...) {
        // ArtMethod* invalid or PrettyMethod crashed — return empty
    }
    return mi;
}

/**
 * resolveFieldByArt — 直接调用 ArtField::PrettyField() 解析字段信息
 * jfieldID 在 ART 中就是 ArtField* 指针
 */
static FieldInfo resolveFieldByArt(jfieldID fid) {
    FieldInfo fi;
    fi.fieldName = "?";
    fi.typeSig = "?";
    if (!fid) return fi;

    initArtSymbols();
    if (!g_ArtPrettyField) return fi;

    try {
        std::string pretty = g_ArtPrettyField((void*)fid, true);
        if (!pretty.empty()) {
            fi = parsePrettyField(pretty);
            if (fi.fieldName != "?") {
                cacheField(fid, fi.className, fi.fieldName, fi.typeSig);
            }
        }
    } catch (...) {
        // ArtField* invalid or PrettyField crashed — return empty
    }
    return fi;
}

/**
 * resolveMethodByReflection — 通过 ToReflectedMethod 反查 jmethodID
 * @param ctx        unicorn vm context
 * @param env        JNI env
 * @param clazz      declaring class (for instance methods pass GetObjectClass result)
 * @param mid        the jmethodID to resolve
 * @param isStatic   whether it's a static method
 * @return MethodInfo with name and signature, empty on failure
 */
static MethodInfo resolveMethodByReflection(
    vm_context* ctx, JNIEnv* env,
    jclass clazz, jmethodID mid, bool isStatic)
{
    MethodInfo result;
    if (!ctx || !env || !mid || !clazz) return result;
    JniExceptionGuard _guard(ctx, env);
    JniLocalFrame _frame(ctx, env, 32);
    if (!_frame.active) return result;

    // ToReflectedMethod -> Method or Constructor object
    jobject reflected = (jobject)proxy_Jni(
        ctx, (void*)env->functions->ToReflectedMethod,
        env, clazz, mid, (jboolean)isStatic
    );
    if (!reflected) return result;

    // Find java.lang.reflect.Method and Constructor classes
    jclass clsMethod = (jclass)proxy_Jni(
        ctx, (void*)env->functions->FindClass,
        env, "java/lang/reflect/Method"
    );
    jclass clsCtor = (jclass)proxy_Jni(
        ctx, (void*)env->functions->FindClass,
        env, "java/lang/reflect/Constructor"
    );

    // Check if it's a Method
    jboolean isMethod = clsMethod ? (jboolean)(uintptr_t)proxy_Jni(
        ctx, (void*)env->functions->IsInstanceOf, env, reflected, clsMethod
    ) : JNI_FALSE;

    if (isMethod) {
        // --- Get method name via Method.getName() ---
        jmethodID mid_getName = (jmethodID)proxy_Jni(
            ctx, (void*)env->functions->GetMethodID,
            env, clsMethod, "getName", "()Ljava/lang/String;"
        );
        if (mid_getName) {
            jstring nameStr = (jstring)proxy_Jni(
                ctx, (void*)env->functions->CallObjectMethod,
                env, reflected, mid_getName
            );
            if (nameStr) {
                const char* utf = (const char*)proxy_Jni(
                    ctx, (void*)env->functions->GetStringUTFChars,
                    env, nameStr, nullptr
                );
                if (utf) {
                    result.methodName = utf;
                    proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                              env, nameStr, utf);
                }
            }
        }

        // --- Build signature from getParameterTypes() + getReturnType() ---
        // getParameterTypes() -> Class[]
        jmethodID mid_getParams = (jmethodID)proxy_Jni(
            ctx, (void*)env->functions->GetMethodID,
            env, clsMethod, "getParameterTypes", "()[Ljava/lang/Class;"
        );
        // getReturnType() -> Class
        jmethodID mid_getReturn = (jmethodID)proxy_Jni(
            ctx, (void*)env->functions->GetMethodID,
            env, clsMethod, "getReturnType", "()Ljava/lang/Class;"
        );

        // Class.getName()
        jclass clsClass = (jclass)proxy_Jni(
            ctx, (void*)env->functions->FindClass,
            env, "java/lang/Class"
        );
        jmethodID mid_clsGetName = clsClass ? (jmethodID)proxy_Jni(
            ctx, (void*)env->functions->GetMethodID,
            env, clsClass, "getName", "()Ljava/lang/String;"
        ) : nullptr;

        std::string sig = "(";

        if (mid_getParams && mid_clsGetName) {
            jobjectArray params = (jobjectArray)proxy_Jni(
                ctx, (void*)env->functions->CallObjectMethod,
                env, reflected, mid_getParams
            );
            if (params) {
                jsize count = (jsize)(intptr_t)proxy_Jni(
                    ctx, (void*)env->functions->GetArrayLength,
                    env, params
                );
                for (jsize i = 0; i < count; i++) {
                    jobject paramCls = (jobject)proxy_Jni(
                        ctx, (void*)env->functions->GetObjectArrayElement,
                        env, params, i
                    );
                    if (!paramCls) continue;
                    jstring pName = (jstring)proxy_Jni(
                        ctx, (void*)env->functions->CallObjectMethod,
                        env, paramCls, mid_clsGetName
                    );
                    if (pName) {
                        const char* pUtf = (const char*)proxy_Jni(
                            ctx, (void*)env->functions->GetStringUTFChars,
                            env, pName, nullptr
                        );
                        if (pUtf) {
                            sig += classNameToSig(pUtf);
                            proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                                      env, pName, pUtf);
                        }
                    }
                }
            }
        }
        sig += ")";

        // Return type
        if (mid_getReturn && mid_clsGetName) {
            jobject retCls = (jobject)proxy_Jni(
                ctx, (void*)env->functions->CallObjectMethod,
                env, reflected, mid_getReturn
            );
            if (retCls) {
                jstring rName = (jstring)proxy_Jni(
                    ctx, (void*)env->functions->CallObjectMethod,
                    env, retCls, mid_clsGetName
                );
                if (rName) {
                    const char* rUtf = (const char*)proxy_Jni(
                        ctx, (void*)env->functions->GetStringUTFChars,
                        env, rName, nullptr
                    );
                    if (rUtf) {
                        sig += classNameToSig(rUtf);
                        proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                                  env, rName, rUtf);
                    }
                }
            }
        }

        result.signature = sig;
        result.className = dotToSlash(getJClassName(ctx, env, clazz));

    } else {
        // --- Constructor ---
        jboolean isCtor = clsCtor ? (jboolean)(uintptr_t)proxy_Jni(
            ctx, (void*)env->functions->IsInstanceOf, env, reflected, clsCtor
        ) : JNI_FALSE;

        if (isCtor) {
            result.methodName = "<init>";

            jmethodID mid_getParams = (jmethodID)proxy_Jni(
                ctx, (void*)env->functions->GetMethodID,
                env, clsCtor, "getParameterTypes", "()[Ljava/lang/Class;"
            );
            jclass clsClass = (jclass)proxy_Jni(
                ctx, (void*)env->functions->FindClass,
                env, "java/lang/Class"
            );
            jmethodID mid_clsGetName = clsClass ? (jmethodID)proxy_Jni(
                ctx, (void*)env->functions->GetMethodID,
                env, clsClass, "getName", "()Ljava/lang/String;"
            ) : nullptr;

            std::string sig = "(";
            if (mid_getParams && mid_clsGetName) {
                jobjectArray params = (jobjectArray)proxy_Jni(
                    ctx, (void*)env->functions->CallObjectMethod,
                    env, reflected, mid_getParams
                );
                if (params) {
                    jsize count = (jsize)(intptr_t)proxy_Jni(
                        ctx, (void*)env->functions->GetArrayLength,
                        env, params
                    );
                    for (jsize i = 0; i < count; i++) {
                        jobject paramCls = (jobject)proxy_Jni(
                            ctx, (void*)env->functions->GetObjectArrayElement,
                            env, params, i
                        );
                        if (!paramCls) continue;
                        jstring pName = (jstring)proxy_Jni(
                            ctx, (void*)env->functions->CallObjectMethod,
                            env, paramCls, mid_clsGetName
                        );
                        if (pName) {
                            const char* pUtf = (const char*)proxy_Jni(
                                ctx, (void*)env->functions->GetStringUTFChars,
                                env, pName, nullptr
                            );
                            if (pUtf) {
                                sig += classNameToSig(pUtf);
                                proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                                          env, pName, pUtf);
                            }
                        }
                    }
                }
            }
            sig += ")V";
            result.signature = sig;
            result.className = dotToSlash(getJClassName(ctx, env, clazz));
        }
    }

    // Cache the result for future lookups
    if (!result.methodName.empty()) {
        cacheMethod(mid, result.className, result.methodName, result.signature);
    }

    return result;
}

/**
 * resolveFieldByReflection — 通过 ToReflectedField 反查 jfieldID
 */
static FieldInfo resolveFieldByReflection(
    vm_context* ctx, JNIEnv* env,
    jclass clazz, jfieldID fid, bool isStatic)
{
    FieldInfo result;
    if (!ctx || !env || !fid || !clazz) return result;
    JniExceptionGuard _guard(ctx, env);
    JniLocalFrame _frame(ctx, env, 16);
    if (!_frame.active) return result;

    jobject reflected = (jobject)proxy_Jni(
        ctx, (void*)env->functions->ToReflectedField,
        env, clazz, fid, (jboolean)isStatic
    );
    if (!reflected) return result;

    jclass clsField = (jclass)proxy_Jni(
        ctx, (void*)env->functions->FindClass,
        env, "java/lang/reflect/Field"
    );
    if (!clsField) return result;

    // Field.getName()
    jmethodID mid_getName = (jmethodID)proxy_Jni(
        ctx, (void*)env->functions->GetMethodID,
        env, clsField, "getName", "()Ljava/lang/String;"
    );
    if (mid_getName) {
        jstring nameStr = (jstring)proxy_Jni(
            ctx, (void*)env->functions->CallObjectMethod,
            env, reflected, mid_getName
        );
        if (nameStr) {
            const char* utf = (const char*)proxy_Jni(
                ctx, (void*)env->functions->GetStringUTFChars,
                env, nameStr, nullptr
            );
            if (utf) {
                result.fieldName = utf;
                proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                          env, nameStr, utf);
            }
        }
    }

    // Field.getType() -> Class, then Class.getName() -> type sig
    jmethodID mid_getType = (jmethodID)proxy_Jni(
        ctx, (void*)env->functions->GetMethodID,
        env, clsField, "getType", "()Ljava/lang/Class;"
    );
    if (mid_getType) {
        jobject typeCls = (jobject)proxy_Jni(
            ctx, (void*)env->functions->CallObjectMethod,
            env, reflected, mid_getType
        );
        if (typeCls) {
            jclass clsClass = (jclass)proxy_Jni(
                ctx, (void*)env->functions->FindClass,
                env, "java/lang/Class"
            );
            jmethodID mid_clsGetName = clsClass ? (jmethodID)proxy_Jni(
                ctx, (void*)env->functions->GetMethodID,
                env, clsClass, "getName", "()Ljava/lang/String;"
            ) : nullptr;

            if (mid_clsGetName) {
                jstring tName = (jstring)proxy_Jni(
                    ctx, (void*)env->functions->CallObjectMethod,
                    env, typeCls, mid_clsGetName
                );
                if (tName) {
                    const char* tUtf = (const char*)proxy_Jni(
                        ctx, (void*)env->functions->GetStringUTFChars,
                        env, tName, nullptr
                    );
                    if (tUtf) {
                        result.typeSig = classNameToSig(tUtf);
                        proxy_Jni(ctx, (void*)env->functions->ReleaseStringUTFChars,
                                  env, tName, tUtf);
                    }
                }
            }
        }
    }

    result.className = dotToSlash(getJClassName(ctx, env, clazz));

    // Cache for future
    if (!result.fieldName.empty()) {
        cacheField(fid, result.className, result.fieldName, result.typeSig);
    }

    return result;
}

/**
 * getMethodInfoResolved — 获取方法信息
 * 优先级: 1. 缓存  2. ART PrettyMethod (零JNI开销)  3. 反射 (兜底)
 */
static MethodInfo getMethodInfoResolved(
    vm_context* ctx, JNIEnv* env,
    uint64_t objOrClass, jmethodID mid, bool isStatic)
{
    // 1. 先查缓存
    MethodInfo mi = getCachedMethod(mid);
    if (!mi.methodName.empty()) return mi;

    // 2. 尝试 ART 直读 (jmethodID == ArtMethod*, 直接调用 PrettyMethod)
    mi = resolveMethodByArt(mid);
    if (!mi.methodName.empty()) return mi;

    // 3. 兜底: 反射查询 (Android 15 符号被 strip 时走这条路)
    JniExceptionGuard _guard(ctx, env);
    jclass clazz;
    bool clazzNeedsDelete = false;
    if (isStatic) {
        clazz = (jclass)objOrClass;
    } else {
        clazz = (jclass)proxy_Jni(
            ctx, (void*)env->functions->GetObjectClass,
            env, (jobject)objOrClass
        );
        clazzNeedsDelete = true;
    }
    if (!clazz) return mi;

    mi = resolveMethodByReflection(ctx, env, clazz, mid, isStatic);
    if (clazzNeedsDelete) jni_delete_local_ref(ctx, env, clazz);
    return mi;
}

/**
 * getFieldInfoResolved — 获取字段信息
 * 优先级: 1. 缓存  2. ART PrettyField (零JNI开销)  3. 反射 (兜底)
 */
static FieldInfo getFieldInfoResolved(
    vm_context* ctx, JNIEnv* env,
    uint64_t objOrClass, jfieldID fid, bool isStatic)
{
    // 1. 先查缓存
    FieldInfo fi = getCachedField(fid);
    if (fi.fieldName != "?") return fi;

    // 2. 尝试 ART 直读 (jfieldID == ArtField*, 直接调用 PrettyField)
    fi = resolveFieldByArt(fid);
    if (fi.fieldName != "?") return fi;

    // 3. 兜底: 反射查询
    JniExceptionGuard _guard(ctx, env);
    jclass clazz;
    bool clazzNeedsDelete = false;
    if (isStatic) {
        clazz = (jclass)objOrClass;
    } else {
        clazz = (jclass)proxy_Jni(
            ctx, (void*)env->functions->GetObjectClass,
            env, (jobject)objOrClass
        );
        clazzNeedsDelete = true;
    }
    if (!clazz) return fi;

    fi = resolveFieldByReflection(ctx, env, clazz, fid, isStatic);
    if (clazzNeedsDelete) jni_delete_local_ref(ctx, env, clazz);
    return fi;
}

// ===========================================================================
//  TRACE HANDLERS
// ===========================================================================

// ----- FindClass -----
static void Trace_FindClass(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string name = read_cstr(x1);
        oss << "JNIEnv->FindClass(\"" << name << "\")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- GetSuperclass -----
static void Trace_GetSuperclass(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string cls = getJClassName(ctx, env, (jclass)x1);
        oss << "JNIEnv->GetSuperclass(" << dotToSlash(cls) << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    std::string superCls = ret ? getJClassName(ctx, env, (jclass)ret) : "null";
    oss << " => " << dotToSlash(superCls) << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- GetObjectClass -----
static void Trace_GetObjectClass(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetObjectClass(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    std::string cls = ret ? getJClassName(ctx, env, (jclass)ret) : "null";
    oss << " => " << dotToSlash(cls) << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- IsAssignableFrom -----
static void Trace_IsAssignableFrom(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        std::string cls1 = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        std::string cls2 = dotToSlash(getJClassName(ctx, env, (jclass)x2));
        oss << "JNIEnv->IsAssignableFrom(" << cls1 << ", " << cls2 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (ret ? "true" : "false") << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- IsInstanceOf -----
static void Trace_IsInstanceOf(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        oss << "JNIEnv->IsInstanceOf(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ", " << dotToSlash(getJClassName(ctx, env, (jclass)x2)) << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (ret ? "true" : "false");
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- IsSameObject -----
static void Trace_IsSameObject(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        oss << "JNIEnv->IsSameObject(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ", ";
        if (x2) print_jobject_safe(ctx, env, oss, (jobject)x2);
        else oss << "null";
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (ret ? "true" : "false");
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- NewGlobalRef -----
static void Trace_NewGlobalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->NewGlobalRef(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- DeleteGlobalRef -----
static void Trace_DeleteGlobalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->DeleteGlobalRef(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- NewLocalRef -----
static void Trace_NewLocalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->NewLocalRef(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- DeleteLocalRef -----
static void Trace_DeleteLocalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->DeleteLocalRef(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- NewWeakGlobalRef -----
static void Trace_NewWeakGlobalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->NewWeakGlobalRef(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- DeleteWeakGlobalRef -----
static void Trace_DeleteWeakGlobalRef(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->DeleteWeakGlobalRef(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  GetMethodID / GetStaticMethodID  (cache the result)
// ===========================================================================
static void Trace_GetMethodID(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    // 桥接：cacheMethod 需要入口段的 cls/name/sig 与返回段的 ret，用函数级 thread_local 携带。
    static thread_local std::string s_cls, s_name, s_sig;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        s_cls  = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        s_name = read_cstr(x2);
        s_sig  = read_cstr(x3);
        oss << "JNIEnv->GetMethodID(" << s_cls << "." << s_name << s_sig << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    if (ret) cacheMethod((jmethodID)ret, s_cls, s_name, s_sig);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStaticMethodID(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    static thread_local std::string s_cls, s_name, s_sig;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        s_cls  = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        s_name = read_cstr(x2);
        s_sig  = read_cstr(x3);
        oss << "JNIEnv->GetStaticMethodID(" << s_cls << "." << s_name << s_sig << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    if (ret) cacheMethod((jmethodID)ret, s_cls, s_name, s_sig);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  GetFieldID / GetStaticFieldID  (cache the result)
// ===========================================================================
static void Trace_GetFieldID(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    static thread_local std::string s_cls, s_name, s_sig;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        s_cls  = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        s_name = read_cstr(x2);
        s_sig  = read_cstr(x3);
        oss << "JNIEnv->GetFieldID(" << s_cls << "." << s_name << " " << s_sig << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    if (ret) cacheField((jfieldID)ret, s_cls, s_name, s_sig);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStaticFieldID(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    static thread_local std::string s_cls, s_name, s_sig;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        s_cls  = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        s_name = read_cstr(x2);
        s_sig  = read_cstr(x3);
        oss << "JNIEnv->GetStaticFieldID(" << s_cls << "." << s_name << " " << s_sig << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    if (ret) cacheField((jfieldID)ret, s_cls, s_name, s_sig);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  RegisterNatives
// ===========================================================================
static void Trace_RegisterNatives(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    // 多行输出：全部参数在入口段有效，直接打印；返回段无输出。
    if (g_jni_at_return) return;
    uint64_t x1 = 0, x2 = 0, x3 = 0;
    vc_reg_read(ctx, VC_REG_X1, &x1);
    vc_reg_read(ctx, VC_REG_X2, &x2);
    vc_reg_read(ctx, VC_REG_X3, &x3);

    std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
    int nMethods = (int)x3;

    logf(ctx, "JNIEnv->RegisterNatives(%s, %s, %d)%s",
         cls.c_str(), format_addr(x2).c_str(), nMethods, format_lr(ctx).c_str());

    // Print each registered native method
    struct JNINativeMethod_layout {
        uint64_t name;
        uint64_t signature;
        uint64_t fnPtr;
    };

    auto* methods = (JNINativeMethod_layout*)x2;
    for (int i = 0; i < nMethods; i++) {
        std::string mName = read_cstr(methods[i].name);
        std::string mSig  = read_cstr(methods[i].signature);
        logf(ctx, "  RegisterNative(%s, %s%s, RX@%s)",
             cls.c_str(), mName.c_str(), mSig.c_str(),
             format_addr(methods[i].fnPtr).c_str());
    }
}

// ===========================================================================
//  art::ArtMethod::RegisterNative — ART 内部单方法注册
// ===========================================================================
// 某些 SO 绕过 env->RegisterNatives，直接 dlsym 拿 art 内部的
// ArtMethod::RegisterNative(const void*) 来逐个注册 native 方法。
// 签名: void ArtMethod::RegisterNative(const void* native_method)
// ARM64 ABI: x0=ArtMethod*, x1=native_method_ptr
static void Trace_Art_RegisterNative(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    (void)env;
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x0 = 0, x1 = 0;
        vc_reg_read(ctx, VC_REG_X0, &x0);
        vc_reg_read(ctx, VC_REG_X1, &x1);

        std::string cls = "?", mName = "?", mSig = "";
        if (g_ArtPrettyMethod && x0) {
            std::string pretty = g_ArtPrettyMethod((void*)x0, true);
            MethodInfo mi = parsePrettyMethod(pretty);
            if (!mi.className.empty()) {
                cls = mi.className;
                mName = mi.methodName;
                mSig = mi.signature;
            }
        }
        oss << "RegisterNative(" << cls << ", " << mName << mSig
            << ", RX@" << format_addr(x1) << ") [via art::ArtMethod]";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  String operations
// ===========================================================================
static void Trace_NewStringUTF(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string s = read_cstr(x1);
        oss << "JNIEnv->NewStringUTF(\"" << s << "\")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringUTFChars(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetStringUTFChars(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ReleaseStringUTFChars(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->ReleaseStringUTFChars(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringLength(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetStringLength(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret;
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringUTFLength(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetStringUTFLength(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret;
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_NewString(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        oss << "JNIEnv->NewString(unichar_ptr, " << (int)x2 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jstring(ctx, env, oss, (jstring)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringChars(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetStringChars(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ReleaseStringChars(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->ReleaseStringChars(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Object creation
// ===========================================================================
static void Trace_NewObjectV(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, false);
        oss << "JNIEnv->NewObjectV(class " << cls << ", " << mi.methodName << mi.signature << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_AllocObject(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        oss << "JNIEnv->AllocObject(" << cls << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_safe(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Call*MethodV  (instance methods)
// ===========================================================================
// Helper: format method signature return type for display
static std::string sigReturnType(const std::string& sig) {
    auto pos = sig.rfind(')');
    if (pos == std::string::npos || pos + 1 >= sig.size()) return "";
    return sig.substr(pos + 1);
}

// Helper: try to format va_list args from cached method signature
// va_list on ARM64 is complex, so we read from x3 register which points to the va_list struct.
// For safety, we only print args for simple signatures (primitives and strings).
static void print_va_args_from_sig(vm_context* ctx, JNIEnv* env,
                                    std::ostringstream& oss, const std::string& sig,
                                    uint64_t va_ptr) {
    if (sig.empty() || !va_ptr) return;
    JniExceptionGuard _guard(ctx, env);

    auto paren = sig.find('(');
    if (paren == std::string::npos) return;
    const char* p = sig.c_str() + paren + 1;

    // ARM64 va_list struct layout:
    //   void* __stack;       // +0
    //   void* __gr_top;      // +8
    //   void* __vr_top;      // +16
    //   int   __gr_offs;     // +24
    //   int   __vr_offs;     // +28
    struct arm64_va_list {
        void* stack;
        void* gr_top;
        void* vr_top;
        int   gr_offs;
        int   vr_offs;
    };

    arm64_va_list* va = (arm64_va_list*)va_ptr;
    // General purpose register save area: gr_top + gr_offs gives next GP reg
    // Each GP reg slot is 8 bytes, gr_offs starts negative and goes toward 0
    int gr_offs = va->gr_offs;
    char* gr_top = (char*)va->gr_top;
    char* stack = (char*)va->stack;

    auto next_gp_arg = [&]() -> uint64_t {
        if (gr_offs < 0) {
            uint64_t val = *(uint64_t*)(gr_top + gr_offs);
            gr_offs += 8;
            return val;
        }
        // overflow to stack
        uint64_t val = *(uint64_t*)stack;
        stack += 8;
        return val;
    };

    bool first = true;
    while (*p && *p != ')') {
        if (!first) oss << ", ";
        first = false;

        switch (*p) {
            case 'Z': { jint v = (jint)next_gp_arg(); oss << (v ? "true" : "false"); p++; break; }
            case 'B': { jint v = (jint)next_gp_arg(); oss << (int)(int8_t)v; p++; break; }
            case 'C': { jint v = (jint)next_gp_arg(); oss << "'" << (char)(v & 0xFFFF) << "'"; p++; break; }
            case 'S': { jint v = (jint)next_gp_arg(); oss << (int16_t)v; p++; break; }
            case 'I': { jint v = (jint)next_gp_arg(); oss << v; p++; break; }
            case 'J': { jlong v = (jlong)next_gp_arg(); oss << (long long)v; p++; break; }
            case 'F': { next_gp_arg(); oss << "float"; p++; break; } // FP regs, skip
            case 'D': { next_gp_arg(); oss << "double"; p++; break; } // FP regs, skip
            case 'L': {
                uint64_t obj = next_gp_arg();
                // skip type descriptor
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                // check if it's a string
                if (obj) {
                    jclass strCls = (jclass)proxy_Jni(ctx, (void*)env->functions->FindClass,
                                                       env, "java/lang/String");
                    jboolean isStr = strCls ? (jboolean)(uintptr_t)proxy_Jni(
                        ctx, (void*)env->functions->IsInstanceOf, env, (jobject)obj, strCls) : JNI_FALSE;
                    jni_delete_local_ref(ctx, env, strCls);
                    if (isStr) {
                        print_jstring(ctx, env, oss, (jstring)obj);
                    } else {
                        print_jobject_safe(ctx, env, oss, (jobject)obj);
                    }
                } else {
                    oss << "null";
                }
                break;
            }
            case '[': {
                uint64_t arr = next_gp_arg();
                p++;
                while (*p == '[') p++;
                if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
                else if (*p) p++;
                if (arr) print_jobject_safe(ctx, env, oss, (jobject)arr);
                else oss << "null";
                break;
            }
            default: next_gp_arg(); oss << "?"; p++; break;
        }
    }
}

// Helper macro to generate instance method call trace handlers
#define DEFINE_CALL_METHOD_V(JNI_NAME, RET_FMT)                               \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0;                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, false); \
        oss << "JNIEnv->" #JNI_NAME "(";                                       \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << mi.methodName << "(";                                   \
        if (!mi.signature.empty() && x3) {                                     \
            print_va_args_from_sig(ctx, env, oss, mi.signature, x3);           \
        }                                                                      \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    RET_FMT;                                                                   \
    oss << ")" << format_lr(ctx);                                              \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

// Object return: print as jobject
DEFINE_CALL_METHOD_V(CallObjectMethodV,
    oss << " => ";
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null"
)

// Boolean return
DEFINE_CALL_METHOD_V(CallBooleanMethodV,
    oss << " => " << ((ret & 1) ? "true" : "false")
)

// Byte return
DEFINE_CALL_METHOD_V(CallByteMethodV,
    oss << " => 0x" << std::hex << (ret & 0xFF) << std::dec
)

// Char return
DEFINE_CALL_METHOD_V(CallCharMethodV,
    oss << " => '" << (char)(ret & 0xFFFF) << "'"
)

// Short return
DEFINE_CALL_METHOD_V(CallShortMethodV,
    oss << " => " << (int16_t)(ret & 0xFFFF)
)

// Int return
DEFINE_CALL_METHOD_V(CallIntMethodV,
    oss << " => 0x" << std::hex << (uint32_t)ret << std::dec
)

// Long return (x0 holds 64-bit result)
DEFINE_CALL_METHOD_V(CallLongMethodV,
    oss << " => 0x" << std::hex << ret << std::dec
)

// Float return (stored in x0 as bits)
DEFINE_CALL_METHOD_V(CallFloatMethodV,
    float fv; memcpy(&fv, &ret, sizeof(float));
    oss << " => " << fv
)

// Double return
DEFINE_CALL_METHOD_V(CallDoubleMethodV,
    double dv; memcpy(&dv, &ret, sizeof(double));
    oss << " => " << dv
)

// Void — no return value
static void Trace_CallVoidMethodV(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, false);
        oss << "JNIEnv->CallVoidMethodV(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ", " << mi.methodName << "(";
        if (!mi.signature.empty() && x3) {
            print_va_args_from_sig(ctx, env, oss, mi.signature, x3);
        }
        oss << "))";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  CallStatic*MethodV  (static methods)
// ===========================================================================
#define DEFINE_CALL_STATIC_METHOD_V(JNI_NAME, RET_FMT)                        \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0;                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));     \
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, true); \
        oss << "JNIEnv->" #JNI_NAME "(class " << cls                          \
            << ", " << mi.methodName << "(";                                   \
        if (!mi.signature.empty() && x3) {                                     \
            print_va_args_from_sig(ctx, env, oss, mi.signature, x3);           \
        }                                                                      \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    RET_FMT;                                                                   \
    oss << ")" << format_lr(ctx);                                              \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_CALL_STATIC_METHOD_V(CallStaticObjectMethodV,
    oss << " => ";
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null"
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticBooleanMethodV,
    oss << " => " << ((ret & 1) ? "true" : "false")
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticByteMethodV,
    oss << " => 0x" << std::hex << (ret & 0xFF) << std::dec
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticCharMethodV,
    oss << " => '" << (char)(ret & 0xFFFF) << "'"
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticShortMethodV,
    oss << " => " << (int16_t)(ret & 0xFFFF)
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticIntMethodV,
    oss << " => 0x" << std::hex << (uint32_t)ret << std::dec
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticLongMethodV,
    oss << " => 0x" << std::hex << ret << std::dec
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticFloatMethodV,
    float fv; memcpy(&fv, &ret, sizeof(float));
    oss << " => " << fv
)

DEFINE_CALL_STATIC_METHOD_V(CallStaticDoubleMethodV,
    double dv; memcpy(&dv, &ret, sizeof(double));
    oss << " => " << dv
)

// Static Void
static void Trace_CallStaticVoidMethodV(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, true);
        oss << "JNIEnv->CallStaticVoidMethodV(class " << cls
            << ", " << mi.methodName << "(";
        if (!mi.signature.empty() && x3) {
            print_va_args_from_sig(ctx, env, oss, mi.signature, x3);
        }
        oss << "))";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Instance field access: Get*Field
// ===========================================================================
#define DEFINE_GET_FIELD(JNI_NAME, FMT)                                        \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0;                                              \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        FieldInfo fi = getFieldInfoResolved(ctx, env, x1, (jfieldID)x2, false);\
        oss << "JNIEnv->" #JNI_NAME "(";                                       \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << fi.fieldName << " " << fi.typeSig;                      \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    oss << " => ";                                                             \
    FMT;                                                                       \
    oss << ")" << format_lr(ctx);                                              \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_GET_FIELD(GetObjectField,
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null"
)

DEFINE_GET_FIELD(GetBooleanField,
    oss << ((ret & 1) ? "true" : "false")
)

DEFINE_GET_FIELD(GetByteField,
    oss << "0x" << std::hex << (ret & 0xFF) << std::dec
)

DEFINE_GET_FIELD(GetCharField,
    oss << "'" << (char)(ret & 0xFFFF) << "'"
)

DEFINE_GET_FIELD(GetShortField,
    oss << (int16_t)(ret & 0xFFFF)
)

DEFINE_GET_FIELD(GetIntField,
    oss << "0x" << std::hex << (uint32_t)ret << std::dec
)

DEFINE_GET_FIELD(GetLongField,
    oss << "0x" << std::hex << ret << std::dec
)

DEFINE_GET_FIELD(GetFloatField,
    float fv; memcpy(&fv, &ret, sizeof(float));
    oss << fv
)

DEFINE_GET_FIELD(GetDoubleField,
    double dv; memcpy(&dv, &ret, sizeof(double));
    oss << dv
)

// ===========================================================================
//  Instance field access: Set*Field
// ===========================================================================
#define DEFINE_SET_FIELD(JNI_NAME, VAL_FMT)                                    \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0;                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        FieldInfo fi = getFieldInfoResolved(ctx, env, x1, (jfieldID)x2, false);\
        oss << "JNIEnv->" #JNI_NAME "(";                                       \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << fi.fieldName << " " << fi.typeSig << ", ";              \
        VAL_FMT;                                                               \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_SET_FIELD(SetObjectField,
    print_jobject_or_string(ctx, env, oss, (jobject)x3)
)

DEFINE_SET_FIELD(SetBooleanField,
    oss << ((x3 & 1) ? "true" : "false")
)

DEFINE_SET_FIELD(SetByteField,
    oss << "0x" << std::hex << (x3 & 0xFF) << std::dec
)

DEFINE_SET_FIELD(SetCharField,
    oss << "'" << (char)(x3 & 0xFFFF) << "'"
)

DEFINE_SET_FIELD(SetShortField,
    oss << (int16_t)(x3 & 0xFFFF)
)

DEFINE_SET_FIELD(SetIntField,
    oss << "0x" << std::hex << (uint32_t)x3 << std::dec
)

DEFINE_SET_FIELD(SetLongField,
    oss << "0x" << std::hex << x3 << std::dec
)

DEFINE_SET_FIELD(SetFloatField,
    float fv; memcpy(&fv, &x3, sizeof(float));
    oss << fv
)

DEFINE_SET_FIELD(SetDoubleField,
    double dv; memcpy(&dv, &x3, sizeof(double));
    oss << dv
)

// ===========================================================================
//  Static field access: GetStatic*Field
// ===========================================================================
#define DEFINE_GET_STATIC_FIELD(JNI_NAME, FMT)                                 \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0;                                              \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));     \
        FieldInfo fi = getFieldInfoResolved(ctx, env, x1, (jfieldID)x2, true); \
        oss << "JNIEnv->" #JNI_NAME "(class " << cls                          \
            << ", " << fi.fieldName << " " << fi.typeSig;                      \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    oss << " => ";                                                             \
    FMT;                                                                       \
    oss << ")" << format_lr(ctx);                                              \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_GET_STATIC_FIELD(GetStaticObjectField,
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null"
)

DEFINE_GET_STATIC_FIELD(GetStaticBooleanField,
    oss << ((ret & 1) ? "true" : "false")
)

DEFINE_GET_STATIC_FIELD(GetStaticByteField,
    oss << "0x" << std::hex << (ret & 0xFF) << std::dec
)

DEFINE_GET_STATIC_FIELD(GetStaticCharField,
    oss << "'" << (char)(ret & 0xFFFF) << "'"
)

DEFINE_GET_STATIC_FIELD(GetStaticShortField,
    oss << (int16_t)(ret & 0xFFFF)
)

DEFINE_GET_STATIC_FIELD(GetStaticIntField,
    oss << "0x" << std::hex << (uint32_t)ret << std::dec
)

DEFINE_GET_STATIC_FIELD(GetStaticLongField,
    oss << "0x" << std::hex << ret << std::dec
)

DEFINE_GET_STATIC_FIELD(GetStaticFloatField,
    float fv; memcpy(&fv, &ret, sizeof(float));
    oss << fv
)

DEFINE_GET_STATIC_FIELD(GetStaticDoubleField,
    double dv; memcpy(&dv, &ret, sizeof(double));
    oss << dv
)

// ===========================================================================
//  Static field access: SetStatic*Field
// ===========================================================================
#define DEFINE_SET_STATIC_FIELD(JNI_NAME, VAL_FMT)                             \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) {\
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0;                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));     \
        FieldInfo fi = getFieldInfoResolved(ctx, env, x1, (jfieldID)x2, true); \
        oss << "JNIEnv->" #JNI_NAME "(class " << cls                          \
            << ", " << fi.fieldName << " " << fi.typeSig << ", ";              \
        VAL_FMT;                                                               \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_SET_STATIC_FIELD(SetStaticObjectField,
    print_jobject_or_string(ctx, env, oss, (jobject)x3)
)

DEFINE_SET_STATIC_FIELD(SetStaticBooleanField,
    oss << ((x3 & 1) ? "true" : "false")
)

DEFINE_SET_STATIC_FIELD(SetStaticByteField,
    oss << "0x" << std::hex << (x3 & 0xFF) << std::dec
)

DEFINE_SET_STATIC_FIELD(SetStaticCharField,
    oss << "'" << (char)(x3 & 0xFFFF) << "'"
)

DEFINE_SET_STATIC_FIELD(SetStaticShortField,
    oss << (int16_t)(x3 & 0xFFFF)
)

DEFINE_SET_STATIC_FIELD(SetStaticIntField,
    oss << "0x" << std::hex << (uint32_t)x3 << std::dec
)

DEFINE_SET_STATIC_FIELD(SetStaticLongField,
    oss << "0x" << std::hex << x3 << std::dec
)

DEFINE_SET_STATIC_FIELD(SetStaticFloatField,
    float fv; memcpy(&fv, &x3, sizeof(float));
    oss << fv
)

DEFINE_SET_STATIC_FIELD(SetStaticDoubleField,
    double dv; memcpy(&dv, &x3, sizeof(double));
    oss << dv
)

// ===========================================================================
//  Array operations
// ===========================================================================
static void Trace_GetArrayLength(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetArrayLength(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << ")";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_NewObjectArray(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x2));
        oss << "JNIEnv->NewObjectArray(" << (int)x1 << ", " << cls << ", ";
        if (x3) print_jobject_or_string(ctx, env, oss, (jobject)x3);
        else oss << "null";
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_safe(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetObjectArrayElement(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        oss << "JNIEnv->GetObjectArrayElement(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ", " << (int)x2 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_SetObjectArrayElement(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        oss << "JNIEnv->SetObjectArrayElement(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ", " << (int)x2 << ", ";
        if (x3) print_jobject_or_string(ctx, env, oss, (jobject)x3);
        else oss << "null";
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ----- NewXxxArray -----
#define DEFINE_NEW_ARRAY(TYPE_NAME)                                            \
static void Trace_New##TYPE_NAME##Array(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0;                                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        oss << "JNIEnv->New" #TYPE_NAME "Array(" << (int)x1 << ")";           \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);         \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_NEW_ARRAY(Boolean)
DEFINE_NEW_ARRAY(Byte)
DEFINE_NEW_ARRAY(Char)
DEFINE_NEW_ARRAY(Short)
DEFINE_NEW_ARRAY(Int)
DEFINE_NEW_ARRAY(Long)
DEFINE_NEW_ARRAY(Float)
DEFINE_NEW_ARRAY(Double)

// ----- Get/SetXxxArrayRegion -----
#define DEFINE_GET_ARRAY_REGION(TYPE_NAME)                                      \
static void Trace_Get##TYPE_NAME##ArrayRegion(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0, x4 = 0;                              \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        vc_reg_read(ctx, VC_REG_X4, &x4);           \
        oss << "JNIEnv->Get" #TYPE_NAME "ArrayRegion(";                        \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << (int)x2 << ", " << (int)x3 << ", " << format_addr(x4) << ")";\
        return;                                                                \
    }                                                                          \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

#define DEFINE_SET_ARRAY_REGION(TYPE_NAME)                                      \
static void Trace_Set##TYPE_NAME##ArrayRegion(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0, x4 = 0;                              \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        vc_reg_read(ctx, VC_REG_X4, &x4);           \
        oss << "JNIEnv->Set" #TYPE_NAME "ArrayRegion(";                        \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << (int)x2 << ", " << (int)x3 << ", " << format_addr(x4) << ")";\
        return;                                                                \
    }                                                                          \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_GET_ARRAY_REGION(Boolean)
DEFINE_GET_ARRAY_REGION(Byte)
DEFINE_GET_ARRAY_REGION(Char)
DEFINE_GET_ARRAY_REGION(Short)
DEFINE_GET_ARRAY_REGION(Int)
DEFINE_GET_ARRAY_REGION(Long)
DEFINE_GET_ARRAY_REGION(Float)
DEFINE_GET_ARRAY_REGION(Double)

DEFINE_SET_ARRAY_REGION(Boolean)
DEFINE_SET_ARRAY_REGION(Byte)
DEFINE_SET_ARRAY_REGION(Char)
DEFINE_SET_ARRAY_REGION(Short)
DEFINE_SET_ARRAY_REGION(Int)
DEFINE_SET_ARRAY_REGION(Long)
DEFINE_SET_ARRAY_REGION(Float)
DEFINE_SET_ARRAY_REGION(Double)

// ----- Get/ReleaseXxxArrayElements -----
#define DEFINE_GET_ARRAY_ELEMENTS(TYPE_NAME)                                    \
static void Trace_Get##TYPE_NAME##ArrayElements(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0;                                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        oss << "JNIEnv->Get" #TYPE_NAME "ArrayElements(";                      \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    oss << " => 0x" << std::hex << ret << std::dec;                            \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

#define DEFINE_RELEASE_ARRAY_ELEMENTS(TYPE_NAME)                                \
static void Trace_Release##TYPE_NAME##ArrayElements(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0;                                      \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        oss << "JNIEnv->Release" #TYPE_NAME "ArrayElements(";                  \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        const char* modeStr = (x3 == 0) ? "0" : ((x3 == 1) ? "JNI_COMMIT" : "JNI_ABORT"); \
        oss << ", 0x" << std::hex << x2 << std::dec << ", " << modeStr << ")"; \
        return;                                                                \
    }                                                                          \
    oss << format_lr(ctx);                                                     \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_GET_ARRAY_ELEMENTS(Boolean)
DEFINE_GET_ARRAY_ELEMENTS(Byte)
DEFINE_GET_ARRAY_ELEMENTS(Char)
DEFINE_GET_ARRAY_ELEMENTS(Short)
DEFINE_GET_ARRAY_ELEMENTS(Int)
DEFINE_GET_ARRAY_ELEMENTS(Long)
DEFINE_GET_ARRAY_ELEMENTS(Float)
DEFINE_GET_ARRAY_ELEMENTS(Double)

DEFINE_RELEASE_ARRAY_ELEMENTS(Boolean)
DEFINE_RELEASE_ARRAY_ELEMENTS(Byte)
DEFINE_RELEASE_ARRAY_ELEMENTS(Char)
DEFINE_RELEASE_ARRAY_ELEMENTS(Short)
DEFINE_RELEASE_ARRAY_ELEMENTS(Int)
DEFINE_RELEASE_ARRAY_ELEMENTS(Long)
DEFINE_RELEASE_ARRAY_ELEMENTS(Float)
DEFINE_RELEASE_ARRAY_ELEMENTS(Double)

// ===========================================================================
//  Exception operations
// ===========================================================================
static void Trace_ExceptionCheck(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->ExceptionCheck()";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (ret ? "true" : "false") << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ExceptionClear(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->ExceptionClear()";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ExceptionOccurred(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->ExceptionOccurred()";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_safe(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ExceptionDescribe(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->ExceptionDescribe()";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_Throw(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->Throw(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ThrowNew(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        std::string msg = read_cstr(x2);
        oss << "JNIEnv->ThrowNew(" << cls << ", \"" << msg << "\")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Reflection
// ===========================================================================
static void Trace_ToReflectedMethod(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x2, (bool)x3);
        oss << "JNIEnv->ToReflectedMethod(" << cls << ", " << mi.methodName << mi.signature
            << ", " << (x3 ? "true" : "false") << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ToReflectedField(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        FieldInfo fi = getFieldInfoResolved(ctx, env, x1, (jfieldID)x2, (bool)x3);
        oss << "JNIEnv->ToReflectedField(" << cls << ", " << fi.fieldName << " " << fi.typeSig
            << ", " << (x3 ? "true" : "false") << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_FromReflectedMethod(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->FromReflectedMethod(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) {
        MethodInfo mi = resolveMethodByArt((jmethodID)ret);
        if (!mi.methodName.empty())
            oss << mi.className << "." << mi.methodName << mi.signature;
        else
            oss << "jmethodID@0x" << std::hex << ret << std::dec;
    } else {
        oss << "null";
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_FromReflectedField(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->FromReflectedField(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) {
        FieldInfo fi = resolveFieldByArt((jfieldID)ret);
        if (fi.fieldName != "?")
            oss << fi.className << "." << fi.fieldName << " " << fi.typeSig;
        else
            oss << "jfieldID@0x" << std::hex << ret << std::dec;
    } else {
        oss << "null";
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Monitor
// ===========================================================================
static void Trace_MonitorEnter(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->MonitorEnter(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_MonitorExit(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->MonitorExit(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Misc
// ===========================================================================
static void Trace_GetJavaVM(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->GetJavaVM()";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetVersion(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->GetVersion()";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_DefineClass(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string name = read_cstr(x1);
        oss << "JNIEnv->DefineClass(" << name << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_EnsureLocalCapacity(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->EnsureLocalCapacity(" << (int)x1 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_PushLocalFrame(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->PushLocalFrame(" << (int)x1 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_PopLocalFrame(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->PopLocalFrame(";
        if (x1) print_jobject_or_string(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_or_string(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_NewDirectByteBuffer(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        oss << "JNIEnv->NewDirectByteBuffer(addr=0x" << std::hex << x1
            << std::dec << ", capacity=" << (int64_t)x2 << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => ";
    if (ret) print_jobject_safe(ctx, env, oss, (jobject)ret);
    else oss << "null";
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetDirectBufferAddress(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetDirectBufferAddress(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetDirectBufferCapacity(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetDirectBufferCapacity(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int64_t)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetObjectRefType(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetObjectRefType(";
        if (x1) print_jobject_safe(ctx, env, oss, (jobject)x1);
        else oss << "null";
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    const char* types[] = {"Invalid","Local","Global","WeakGlobal"};
    const char* t = (ret < 4) ? types[ret] : "?";
    oss << " => " << t << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  CallNonvirtual*MethodV — call parent class method (super calls)
// ===========================================================================
#define DEFINE_CALL_NONVIRTUAL_METHOD_V(JNI_NAME, RET_FMT)                      \
static void Trace_##JNI_NAME(vm_context* ctx, JNIEnv* env, uintptr_t addr) { \
    std::ostringstream& oss = g_jni_oss;                                       \
    if (!g_jni_at_return) {                                                    \
        oss.str(""); oss.clear();                                             \
        uint64_t x1 = 0, x2 = 0, x3 = 0, x4 = 0;                             \
        vc_reg_read(ctx, VC_REG_X1, &x1);           \
        vc_reg_read(ctx, VC_REG_X2, &x2);           \
        vc_reg_read(ctx, VC_REG_X3, &x3);           \
        vc_reg_read(ctx, VC_REG_X4, &x4);           \
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x2));     \
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x3, false); \
        oss << "JNIEnv->" #JNI_NAME "(";                                       \
        print_jobject_safe(ctx, env, oss, (jobject)x1);                        \
        oss << ", " << cls << ", " << mi.methodName << "(";                    \
        print_va_args_from_sig(ctx, env, oss, mi.signature, x4);               \
        oss << ")";                                                            \
        return;                                                                \
    }                                                                          \
    uint64_t ret = 0;                                                          \
    vc_reg_read(ctx, VC_REG_X0, &ret);                              \
    oss << " => ";                                                             \
    RET_FMT;                                                                   \
    oss << ")" << format_lr(ctx);                                              \
    logf(ctx, "%s", oss.str().c_str());                                        \
}

DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualObjectMethodV,
    print_jobject_or_string(ctx, env, oss, (jobject)ret))
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualBooleanMethodV,
    oss << ((ret) ? "true" : "false"))
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualByteMethodV,
    oss << "0x" << std::hex << (ret & 0xFF) << std::dec)
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualCharMethodV,
    oss << "'" << (char)(ret & 0xFFFF) << "'")
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualShortMethodV,
    oss << (int16_t)ret)
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualIntMethodV,
    oss << "0x" << std::hex << (uint32_t)ret << std::dec)
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualLongMethodV,
    oss << "0x" << std::hex << ret << std::dec)
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualFloatMethodV,
    { float fv; uint32_t tmp = (uint32_t)ret; memcpy(&fv, &tmp, 4); oss << fv; })
DEFINE_CALL_NONVIRTUAL_METHOD_V(CallNonvirtualDoubleMethodV,
    { double dv; memcpy(&dv, &ret, 8); oss << dv; })

static void Trace_CallNonvirtualVoidMethodV(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0, x4 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        vc_reg_read(ctx, VC_REG_X4, &x4);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x2));
        MethodInfo mi = getMethodInfoResolved(ctx, env, x1, (jmethodID)x3, false);
        oss << "JNIEnv->CallNonvirtualVoidMethodV(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ", " << cls << ", " << mi.methodName << "(";
        print_va_args_from_sig(ctx, env, oss, mi.signature, x4);
        oss << "))";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  String critical / region operations
// ===========================================================================
static void Trace_GetStringCritical(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetStringCritical(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ReleaseStringCritical(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->ReleaseStringCritical(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringRegion(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        oss << "JNIEnv->GetStringRegion(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ", start=" << (int)x2 << ", len=" << (int)x3 << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_GetStringUTFRegion(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        oss << "JNIEnv->GetStringUTFRegion(";
        print_jstring(ctx, env, oss, (jstring)x1);
        oss << ", start=" << (int)x2 << ", len=" << (int)x3 << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Array critical operations
// ===========================================================================
static void Trace_GetPrimitiveArrayCritical(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        oss << "JNIEnv->GetPrimitiveArrayCritical(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        oss << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => 0x" << std::hex << ret << std::dec << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_ReleasePrimitiveArrayCritical(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        vc_reg_read(ctx, VC_REG_X2, &x2);
        vc_reg_read(ctx, VC_REG_X3, &x3);
        oss << "JNIEnv->ReleasePrimitiveArrayCritical(";
        print_jobject_safe(ctx, env, oss, (jobject)x1);
        const char* modeStr = (x3 == 0) ? "0" : ((x3 == 1) ? "JNI_COMMIT" : "JNI_ABORT");
        oss << ", 0x" << std::hex << x2 << std::dec << ", " << modeStr << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  FatalError, UnregisterNatives, ExceptionFatalError
// ===========================================================================
static void Trace_FatalError(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    // FatalError 不返回（进程终止），入口段直接打印，无返回段。
    if (g_jni_at_return) return;
    std::ostringstream& oss = g_jni_oss;
    oss.str(""); oss.clear();
    uint64_t x1 = 0;
    vc_reg_read(ctx, VC_REG_X1, &x1);
    std::string msg = read_cstr(x1);
    oss << "JNIEnv->FatalError(\"" << msg << "\")" << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

static void Trace_UnregisterNatives(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        uint64_t x1 = 0;
        vc_reg_read(ctx, VC_REG_X1, &x1);
        std::string cls = dotToSlash(getJClassName(ctx, env, (jclass)x1));
        oss << "JNIEnv->UnregisterNatives(" << cls << ")";
        return;
    }
    uint64_t ret = 0;
    vc_reg_read(ctx, VC_REG_X0, &ret);
    oss << " => " << (int)ret << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  Fallback: generic handler for any unhandled JNI function
// ===========================================================================
static void Trace_GenericFallback(vm_context* ctx, JNIEnv* env, uintptr_t addr) {
    std::ostringstream& oss = g_jni_oss;
    if (!g_jni_at_return) {
        oss.str(""); oss.clear();
        oss << "JNIEnv->Unknown(addr=0x" << std::hex << (uint64_t)addr << std::dec << ")";
        return;
    }
    oss << format_lr(ctx);
    logf(ctx, "%s", oss.str().c_str());
}

// ===========================================================================
//  TABLE INIT — register every JNI function address → trace handler
// ===========================================================================
#define REG(FIELD, HANDLER) \
    jniTraceTable[(uintptr_t)f->FIELD] = (JNITraceFunc)HANDLER

void tableInit(JNIEnv* env) {
    auto f = env->functions;

    // -- Version --
    REG(GetVersion,             Trace_GetVersion);

    // -- Class operations --
    REG(DefineClass,            Trace_DefineClass);
    REG(FindClass,              Trace_FindClass);
    REG(GetSuperclass,          Trace_GetSuperclass);
    REG(IsAssignableFrom,       Trace_IsAssignableFrom);
    REG(GetObjectClass,         Trace_GetObjectClass);
    REG(IsInstanceOf,           Trace_IsInstanceOf);
    REG(IsSameObject,           Trace_IsSameObject);

    // -- Exceptions --
    REG(Throw,                  Trace_Throw);
    REG(ThrowNew,               Trace_ThrowNew);
    REG(ExceptionOccurred,      Trace_ExceptionOccurred);
    REG(ExceptionDescribe,      Trace_ExceptionDescribe);
    REG(ExceptionClear,         Trace_ExceptionClear);
    REG(ExceptionCheck,         Trace_ExceptionCheck);

    // -- References --
    REG(NewGlobalRef,           Trace_NewGlobalRef);
    REG(DeleteGlobalRef,        Trace_DeleteGlobalRef);
    REG(NewLocalRef,            Trace_NewLocalRef);
    REG(DeleteLocalRef,         Trace_DeleteLocalRef);
    REG(NewWeakGlobalRef,       Trace_NewWeakGlobalRef);
    REG(DeleteWeakGlobalRef,    Trace_DeleteWeakGlobalRef);
    REG(EnsureLocalCapacity,    Trace_EnsureLocalCapacity);
    REG(PushLocalFrame,         Trace_PushLocalFrame);
    REG(PopLocalFrame,          Trace_PopLocalFrame);

    // -- Object creation --
    REG(AllocObject,            Trace_AllocObject);
    REG(NewObjectV,             Trace_NewObjectV);

    // -- Method IDs --
    REG(GetMethodID,            Trace_GetMethodID);
    REG(GetStaticMethodID,      Trace_GetStaticMethodID);

    // -- Call instance methods (V variants) --
    REG(CallObjectMethodV,      Trace_CallObjectMethodV);
    REG(CallBooleanMethodV,     Trace_CallBooleanMethodV);
    REG(CallByteMethodV,        Trace_CallByteMethodV);
    REG(CallCharMethodV,        Trace_CallCharMethodV);
    REG(CallShortMethodV,       Trace_CallShortMethodV);
    REG(CallIntMethodV,         Trace_CallIntMethodV);
    REG(CallLongMethodV,        Trace_CallLongMethodV);
    REG(CallFloatMethodV,       Trace_CallFloatMethodV);
    REG(CallDoubleMethodV,      Trace_CallDoubleMethodV);
    REG(CallVoidMethodV,        Trace_CallVoidMethodV);

    // -- Call static methods (V variants) --
    REG(CallStaticObjectMethodV,  Trace_CallStaticObjectMethodV);
    REG(CallStaticBooleanMethodV, Trace_CallStaticBooleanMethodV);
    REG(CallStaticByteMethodV,    Trace_CallStaticByteMethodV);
    REG(CallStaticCharMethodV,    Trace_CallStaticCharMethodV);
    REG(CallStaticShortMethodV,   Trace_CallStaticShortMethodV);
    REG(CallStaticIntMethodV,     Trace_CallStaticIntMethodV);
    REG(CallStaticLongMethodV,    Trace_CallStaticLongMethodV);
    REG(CallStaticFloatMethodV,   Trace_CallStaticFloatMethodV);
    REG(CallStaticDoubleMethodV,  Trace_CallStaticDoubleMethodV);
    REG(CallStaticVoidMethodV,    Trace_CallStaticVoidMethodV);

    // -- Call nonvirtual methods (V variants, super calls) --
    REG(CallNonvirtualObjectMethodV,  Trace_CallNonvirtualObjectMethodV);
    REG(CallNonvirtualBooleanMethodV, Trace_CallNonvirtualBooleanMethodV);
    REG(CallNonvirtualByteMethodV,    Trace_CallNonvirtualByteMethodV);
    REG(CallNonvirtualCharMethodV,    Trace_CallNonvirtualCharMethodV);
    REG(CallNonvirtualShortMethodV,   Trace_CallNonvirtualShortMethodV);
    REG(CallNonvirtualIntMethodV,     Trace_CallNonvirtualIntMethodV);
    REG(CallNonvirtualLongMethodV,    Trace_CallNonvirtualLongMethodV);
    REG(CallNonvirtualFloatMethodV,   Trace_CallNonvirtualFloatMethodV);
    REG(CallNonvirtualDoubleMethodV,  Trace_CallNonvirtualDoubleMethodV);
    REG(CallNonvirtualVoidMethodV,    Trace_CallNonvirtualVoidMethodV);

    // -- Field IDs --
    REG(GetFieldID,             Trace_GetFieldID);
    REG(GetStaticFieldID,       Trace_GetStaticFieldID);

    // -- Instance field access --
    REG(GetObjectField,         Trace_GetObjectField);
    REG(GetBooleanField,        Trace_GetBooleanField);
    REG(GetByteField,           Trace_GetByteField);
    REG(GetCharField,           Trace_GetCharField);
    REG(GetShortField,          Trace_GetShortField);
    REG(GetIntField,            Trace_GetIntField);
    REG(GetLongField,           Trace_GetLongField);
    REG(GetFloatField,          Trace_GetFloatField);
    REG(GetDoubleField,         Trace_GetDoubleField);

    REG(SetObjectField,         Trace_SetObjectField);
    REG(SetBooleanField,        Trace_SetBooleanField);
    REG(SetByteField,           Trace_SetByteField);
    REG(SetCharField,           Trace_SetCharField);
    REG(SetShortField,          Trace_SetShortField);
    REG(SetIntField,            Trace_SetIntField);
    REG(SetLongField,           Trace_SetLongField);
    REG(SetFloatField,          Trace_SetFloatField);
    REG(SetDoubleField,         Trace_SetDoubleField);

    // -- Static field access --
    REG(GetStaticObjectField,   Trace_GetStaticObjectField);
    REG(GetStaticBooleanField,  Trace_GetStaticBooleanField);
    REG(GetStaticByteField,     Trace_GetStaticByteField);
    REG(GetStaticCharField,     Trace_GetStaticCharField);
    REG(GetStaticShortField,    Trace_GetStaticShortField);
    REG(GetStaticIntField,      Trace_GetStaticIntField);
    REG(GetStaticLongField,     Trace_GetStaticLongField);
    REG(GetStaticFloatField,    Trace_GetStaticFloatField);
    REG(GetStaticDoubleField,   Trace_GetStaticDoubleField);

    REG(SetStaticObjectField,   Trace_SetStaticObjectField);
    REG(SetStaticBooleanField,  Trace_SetStaticBooleanField);
    REG(SetStaticByteField,     Trace_SetStaticByteField);
    REG(SetStaticCharField,     Trace_SetStaticCharField);
    REG(SetStaticShortField,    Trace_SetStaticShortField);
    REG(SetStaticIntField,      Trace_SetStaticIntField);
    REG(SetStaticLongField,     Trace_SetStaticLongField);
    REG(SetStaticFloatField,    Trace_SetStaticFloatField);
    REG(SetStaticDoubleField,   Trace_SetStaticDoubleField);

    // -- String operations --
    REG(NewString,              Trace_NewString);
    REG(GetStringLength,        Trace_GetStringLength);
    REG(GetStringChars,         Trace_GetStringChars);
    REG(ReleaseStringChars,     Trace_ReleaseStringChars);
    REG(NewStringUTF,           Trace_NewStringUTF);
    REG(GetStringUTFLength,     Trace_GetStringUTFLength);
    REG(GetStringUTFChars,      Trace_GetStringUTFChars);
    REG(ReleaseStringUTFChars,  Trace_ReleaseStringUTFChars);
    REG(GetStringCritical,      Trace_GetStringCritical);
    REG(ReleaseStringCritical,  Trace_ReleaseStringCritical);
    REG(GetStringRegion,        Trace_GetStringRegion);
    REG(GetStringUTFRegion,     Trace_GetStringUTFRegion);

    // -- Array operations --
    REG(GetArrayLength,         Trace_GetArrayLength);
    REG(NewObjectArray,         Trace_NewObjectArray);
    REG(GetObjectArrayElement,  Trace_GetObjectArrayElement);
    REG(SetObjectArrayElement,  Trace_SetObjectArrayElement);

    REG(NewBooleanArray,        Trace_NewBooleanArray);
    REG(NewByteArray,           Trace_NewByteArray);
    REG(NewCharArray,           Trace_NewCharArray);
    REG(NewShortArray,          Trace_NewShortArray);
    REG(NewIntArray,            Trace_NewIntArray);
    REG(NewLongArray,           Trace_NewLongArray);
    REG(NewFloatArray,          Trace_NewFloatArray);
    REG(NewDoubleArray,         Trace_NewDoubleArray);

    REG(GetBooleanArrayElements,    Trace_GetBooleanArrayElements);
    REG(GetByteArrayElements,       Trace_GetByteArrayElements);
    REG(GetCharArrayElements,       Trace_GetCharArrayElements);
    REG(GetShortArrayElements,      Trace_GetShortArrayElements);
    REG(GetIntArrayElements,        Trace_GetIntArrayElements);
    REG(GetLongArrayElements,       Trace_GetLongArrayElements);
    REG(GetFloatArrayElements,      Trace_GetFloatArrayElements);
    REG(GetDoubleArrayElements,     Trace_GetDoubleArrayElements);

    REG(ReleaseBooleanArrayElements,    Trace_ReleaseBooleanArrayElements);
    REG(ReleaseByteArrayElements,       Trace_ReleaseByteArrayElements);
    REG(ReleaseCharArrayElements,       Trace_ReleaseCharArrayElements);
    REG(ReleaseShortArrayElements,      Trace_ReleaseShortArrayElements);
    REG(ReleaseIntArrayElements,        Trace_ReleaseIntArrayElements);
    REG(ReleaseLongArrayElements,       Trace_ReleaseLongArrayElements);
    REG(ReleaseFloatArrayElements,      Trace_ReleaseFloatArrayElements);
    REG(ReleaseDoubleArrayElements,     Trace_ReleaseDoubleArrayElements);

    REG(GetBooleanArrayRegion,  Trace_GetBooleanArrayRegion);
    REG(GetByteArrayRegion,     Trace_GetByteArrayRegion);
    REG(GetCharArrayRegion,     Trace_GetCharArrayRegion);
    REG(GetShortArrayRegion,    Trace_GetShortArrayRegion);
    REG(GetIntArrayRegion,      Trace_GetIntArrayRegion);
    REG(GetLongArrayRegion,     Trace_GetLongArrayRegion);
    REG(GetFloatArrayRegion,    Trace_GetFloatArrayRegion);
    REG(GetDoubleArrayRegion,   Trace_GetDoubleArrayRegion);

    REG(SetBooleanArrayRegion,  Trace_SetBooleanArrayRegion);
    REG(SetByteArrayRegion,     Trace_SetByteArrayRegion);
    REG(SetCharArrayRegion,     Trace_SetCharArrayRegion);
    REG(SetShortArrayRegion,    Trace_SetShortArrayRegion);
    REG(SetIntArrayRegion,      Trace_SetIntArrayRegion);
    REG(SetLongArrayRegion,     Trace_SetLongArrayRegion);
    REG(SetFloatArrayRegion,    Trace_SetFloatArrayRegion);
    REG(SetDoubleArrayRegion,   Trace_SetDoubleArrayRegion);

    // -- RegisterNatives --
    REG(RegisterNatives,        Trace_RegisterNatives);

    // -- Monitor --
    REG(MonitorEnter,           Trace_MonitorEnter);
    REG(MonitorExit,            Trace_MonitorExit);

    // -- GetJavaVM --
    REG(GetJavaVM,              Trace_GetJavaVM);

    // -- Reflection --
    REG(ToReflectedMethod,      Trace_ToReflectedMethod);
    REG(ToReflectedField,       Trace_ToReflectedField);
    REG(FromReflectedMethod,    Trace_FromReflectedMethod);
    REG(FromReflectedField,     Trace_FromReflectedField);

    // -- Direct byte buffer --
    REG(NewDirectByteBuffer,    Trace_NewDirectByteBuffer);
    REG(GetDirectBufferAddress, Trace_GetDirectBufferAddress);
    REG(GetDirectBufferCapacity,Trace_GetDirectBufferCapacity);

    // -- Object ref type --
    REG(GetObjectRefType,       Trace_GetObjectRefType);

    // -- Array critical --
    REG(GetPrimitiveArrayCritical,    Trace_GetPrimitiveArrayCritical);
    REG(ReleasePrimitiveArrayCritical,Trace_ReleasePrimitiveArrayCritical);

    // -- FatalError / UnregisterNatives --
    REG(FatalError,             Trace_FatalError);
    REG(UnregisterNatives,      Trace_UnregisterNatives);

    // -- ART 内部 RegisterNative（绕过 JNI 函数表的注册路径） --
    initArtSymbols();
    static const char* registerNativeCandidates[] = {
        "_ZN3art9ArtMethod14RegisterNativeEPKv",
        "_ZN3art9ArtMethod14RegisterNativeEPKvb",
    };
    for (auto sym : registerNativeCandidates) {
        uint64_t fn = resolveSymbolInSo("/libart.so", sym);
        if (fn) {
            jniTraceTable[fn] = (JNITraceFunc)Trace_Art_RegisterNative;
            LOGD("hooked art::ArtMethod::RegisterNative @ %p (%s)", (void*)fn, sym);
        }
    }
}
#undef REG

// ===========================================================================
//  Main dispatch
// ===========================================================================
static JNIEnv* env = nullptr;

// 建表（只做一次）：解析 libart 的 JNI 函数表 + libart 段范围。
// 必须在【非 hook 上下文】首次调用（trace() 顶层 setup）——里面要调 GetEnv/AttachCurrentThread，
// 在 Unicorn 回调里调会触发 ART 线程栈边界检查。建好后再调直接返回。
void jni_ensure_table() {
    if (jniTraceTable.empty()) {
        std::lock_guard<std::mutex> tableLock(mtx);
        if (jniTraceTable.empty()) {
            if (javaVm == nullptr) {
                uint64_t fnAddr = resolveSymbolInSo("/libart.so", "JNI_GetCreatedJavaVMs");
                if (!fnAddr) return;
                typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
                auto fn = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(fnAddr);
                jsize success = 0;
                fn(&javaVm, 1, &success);
                // 拿到 VM 后【继续】把表建完 —— 旧代码在这里 return 是因为它由 per-block hook
                // 反复调用（第一次取 VM、第二次才建表）；现在是 setup 里一次性调，必须一趟走完。
                if (!success || javaVm == nullptr) return;
            }
            int detached = javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
            if (detached) {
                if (javaVm->AttachCurrentThread(&env, NULL) != 0) {
                    return;
                }
            }
            tableInit(env);
            // 用 dladdr 从任意一个 JNI 函数地址拿到 libart.so 基址，
            // 再用 findModuleByAddressExact 从 ELF PT_LOAD 段解析完整范围。
            if (artCodeStart == 0 && !jniTraceTable.empty()) {
                uint64_t sampleAddr = jniTraceTable.begin()->first;
                LibraryInfo artInfo = findModuleByAddressExact(sampleAddr);
                if (artInfo.segments_start != 0) {
                    artCodeStart = artInfo.segments_start;
                    artCodeEnd   = artInfo.segments_end;
                }
            }
        }
    }
}

// 【第一段 · 入口】trace_code 用 capstone 认出调用指令、且目标命中 jni_table 时调（调用点，
// 尚未跳转）：此刻寄存器 = 入参（X0=JNIEnv*）、local ref 有效。跑 handler 解析参数写进
// g_jni_oss、不打印；返回值到 retAddr 那条指令由 jni_on_return 补上。
// 返回地址必须由调用方算好传进来 —— 调用点 LR 还没被 bl/blr 设上，不能读 LR。
void jni_on_call(vm_context* ctx, uint64_t jniAddr, uint64_t retAddr) {
    auto it = jniTraceTable.find(jniAddr);
    if (it == jniTraceTable.end() || it->second == nullptr) return;
    uint64_t x0 = 0;
    vc_reg_read(ctx, VC_REG_X0, &x0);   // 调用点 X0 = JNIEnv*（第一个参数）
    s_jni_handler     = it->second;
    s_jni_ctx         = ctx;
    s_jni_addr        = jniAddr;
    s_jni_env         = (JNIEnv*)x0;
    s_jni_lr          = retAddr;
    g_jni_pending_ret = retAddr;
    g_jni_at_return   = false;
    it->second(ctx, (JNIEnv*)x0, jniAddr);   // 第一段：解析参数 → g_jni_oss
}

// trace_code 执行到 g_jni_pending_ret（JNI 返回地址）那一行时调。
// 此刻真实 JNI 已由 hook_block 执行完 → X0=返回值天然在寄存器里。以 g_jni_at_return=true 跑
// handler「第二段」：读 X0 拼上返回值 + 打印。参数已在入口段解析进 g_jni_oss，这里不碰参数寄存器。
void jni_on_return(vm_context* ctx, vm_context* uc) {
    (void)ctx; (void)uc;              // 用入口 hook 存的真实 vm；返回值/LR 由 handler 自行读 uc/快照
    JNITraceFunc h = s_jni_handler;
    vm_context* vmctx = s_jni_ctx;
    g_jni_pending_ret = 0;
    s_jni_handler = nullptr;
    if (!h) return;

    g_jni_at_return = true;
    h(vmctx, s_jni_env, s_jni_addr);   // 第二段：读 X0=返回值拼上、打印
}

void tryGetJavaVm();

// 注：原来在 libart JNI 地址上挂 UC_HOOK_BLOCK 来监视 JNI 的做法已废弃 ——
// 现在 JNI 调用完全由 trace_code 认出（capstone 判调用指令 + 查 jni_table），
// 入口走 jni_on_call、返回走 jni_on_return，不再有独立的 BLOCK hook、也不过转发桥。

bool isJniAddress(uint64_t address) {
    return !jniTraceTable.empty() && jniTraceTable.find(address) != jniTraceTable.end();
}


