/**
我非常讨厌c++的写法，我更喜欢纯粹的c语言，但是，没办法，这部分让ai做的，dump这部分我完全没有插手
让它充分的利用我早已写好的vcpu的回调来做dump。
*/

//
// TraceUnidbgDump.cpp — 运行时完整采样，导出 Unidbg「中段执行」所需的 dump 数据包。
//
// 设计原则（严格遵守项目 trace / vcpu 分离约束）：
//   - 纯 trace 层，只用 ARM64Emulator.h 的 vc_* 公开 hook 接口，绝不触碰 VM 内部私有函数。
//   - 合理利用 Unicorn 的 3 个 hook，回调里只做最小记录（页号 insert / 符号 emplace），
//     不在热路径读值、不做重活，保证性能。
//   - 关键正确性：dump 要的是「函数入口那一刻」的内存（和入口寄存器配套，供 Unidbg 从入口
//     重放）。若跑完再统一读 host，可写页已被后续执行改成最终值 → 和入口寄存器自相矛盾。
//     所以一边跑一边存：某页第一次被写之前（UC_HOOK_MEM_WRITE 在提交前触发），先把整页快照
//     下来（=入口值）；只读过/没碰过的页不会变，跑完读即入口值。落盘优先用快照。
//   - external_jump 不影响这套：它只是把外部地址「跳出去用原生方式跑」而不在 VM 里逐指令执行，
//     其写内存是那个外部函数自身的行为——Unidbg 从入口重放时会重新调它、重新写一遍，无需我们
//     抓它的写前值。决定「入口内存」的是 VM 里逐指令的 guest 访问，都被 MEM hook 命中了。
//
// 用法（仿 trace：拿指针 → 调 → 释放）：
//   auto h = (jstring(*)(...))trace_unidbg_dump((void*)func, "/data/data/pkg/dump");
//   h(args...);                    // 真实跑一遍；返回前已由 VC_HOOK_EMU_STOP 回调自动把文件落好盘
//   trace_unidbg_dump_finish(h);   // 只释放（和 freeTrace 一样），不再写文件
// 落盘时机：目标函数正常返回 → start_vm_v2 分发 VC_HOOK_EMU_STOP → on_emu_stop → do_finalize。
// 多次 h(args...) 增量累积：dumped_pages 记已 dump 页，下次只写新页。
//
// 【支持多线程】wrapper 被 inline hook 到原地址后，同一个函数常被 app 多线程并发调用。此时
//   **每条线程各自独立采样、各出一份完整 dump**，落到 dumpDir/tid{N}/（每线程一个子目录，
//   互不覆盖）。做法：每条线程首次命中 hook 就 get-or-create 属于自己的 per-thread session
//   （tls_dump_sessions，镜像 trace 的 per-thread clone），各存各的触达页 / 写前快照 / 入口寄存器；
//   master session 只当模板不采样。finish 统一收尾+释放所有 per-thread session。
//   注意：do_finalize 里**绝不 AttachCurrentThread**——它在 emu_stop 里跑、SP 在 VM 的 mmap 栈上，
//   且 worker 线程可能没附着 ART，Attach 会触发 ART 栈边界检查 abort；JNI 表只用 GetEnv + 全局缓存。
//
// 已知局限：JNI 对象内容展开、TPIDR_EL0（vc_reg 未导出）暂未覆盖。
//



#include "ARM64Emulator.h"
#include "logging.h"
#include "LibraryUtils.h"
#include "SymbolTable.h"   // resolveSymbolInSo（从 libart 拿 JNI_GetCreatedJavaVMs）

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <set>
#include <algorithm>
#include <utility>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>          // offsetof
#include <jni.h>           // JNINativeInterface（JNI 函数表结构，槽位固定 ABI）
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace {

constexpr uint64_t PAGE        = 0x1000;
constexpr uint64_t STACK_BELOW = 0x10000;  // SP 以下 64KB：函数帧 / spill / 局部变量
constexpr uint64_t STACK_ABOVE = 0x2000;   // SP 以上 8KB ：ABI 第 9+ 个栈参数

struct DumpSession {
    vm_context* ctx        = nullptr;
    void*       handle     = nullptr;
    std::string dumpDir;
    uint64_t    target_addr = 0;

    vc_hook_h hEntry = 0, hMem = 0, hJump = 0, hSvc = 0, hStop = 0;

    // 入口快照（class 1 寄存器 / class 6 参数 / class 7 基址推导）
    bool     entry_captured = false;
    uint64_t xregs[31]      = {0};        // X0-X30
    uint64_t sp = 0, pc = 0, nzcv = 0, fpcr = 0, fpsr = 0;
    uint8_t  qregs[32][16]  = {{0}};

    // 运行时收集（回调里只做最小记录；v1 单线程，无需加锁）
    std::unordered_set<uint64_t>     pages;         // 触达的页号 addr>>12（class 3）
    // 一边跑一边存：某页第一次被写之前，先把整页原始内容存下（=入口值）。之后这页再被改
    // 也不影响——落盘用这份快照，而不是跑完后的最终值。只读过/没碰过的页不存（没变=入口值）。
    std::unordered_map<uint64_t, std::vector<uint8_t>> written;   // 页号 → 首次写前的整页快照
    std::map<uint64_t, std::string>  symbols;       // 外部跳转 addr → 符号名（class 4）
    std::set<std::string>            opened_files;  // 目标 open/openat 过的文件路径（class 8）

    // 已 dump 过的页号（跨多次 h(args) 累积）：已落盘的页下次跳过，实现增量 dump。
    std::unordered_set<uint64_t>     dumped_pages;
    bool any_finalized = false;   // 是否已至少收尾落盘一次（emu_stop 触发）；finish 据此决定是否兜底
    bool withTrace = false;       // 是否同时开完整指令 trace（句柄由 trace() 建，清理走 freeTrace）
    // 多线程支持:wrapper 被 inline hook 到原地址后,同一个函数可能被 app 多线程并发调用。此时
    // **每条线程各自独立采样、各出一份 dump**(落到 dumpDir/tid{N}/),互不干扰——见下面的
    // per-thread session 机制。上面这些采样字段属于「某条线程那一份」;master session 只当模板不采样。
    uint32_t     tid = 0;            // 0=master 模板;非 0=某线程的 per-thread 采样 session
    DumpSession* master = nullptr;   // per-thread session 指回它的 master(供 finish 统一清理)
};

std::unordered_map<void*, DumpSession*> g_sessions;   // handle → master session
std::mutex g_sessions_mtx;   // setup/finish 偶发调用，用锁保护 map 即可

// ---- 多线程:每线程各自一份 per-thread session（镜像 trace 的 per-thread clone 机制）----
// 同一个 wrapper 被多线程并发调用时，每条线程首次命中 hook 就 get-or-create 属于自己的 session，
// 各存各的触达页 / 写前快照 / 入口寄存器，互不干扰；emu_stop 时各落各的 dumpDir/tid{N}/。
// master 只当模板（存 config），从不参与采样。
static thread_local std::unordered_map<DumpSession*, DumpSession*> tls_dump_sessions;
static std::vector<DumpSession*> g_dump_all_sessions;   // 所有 per-thread session（finish 统一清理）
static int g_dump_sessions_lock = 0;

static DumpSession* getOrCreatePerThreadSession(DumpSession* master) {
    // 用 thread_local map 做本线程的 per-thread 缓存(usually 1 项,hash 查找 O(1))。
    // 注意:不能再叠一层「上次命中指针」缓存——finish 释放 session+master 后,malloc 会复用 master
    // 地址,裸指针缓存会命中已释放的 session 造成 UAF(踩过)。map 里 finish 已 erase 本线程条目,
    // 复用地址查不到 → 重建,安全。
    auto it = tls_dump_sessions.find(master);
    if (it != tls_dump_sessions.end()) return it->second;

    auto* s = new DumpSession();
    s->ctx = master->ctx; s->handle = master->handle; s->dumpDir = master->dumpDir;
    s->target_addr = master->target_addr; s->withTrace = master->withTrace;
    s->tid = (uint32_t)syscall(__NR_gettid);
    s->master = master;
    tls_dump_sessions[master] = s;
    while (__sync_lock_test_and_set(&g_dump_sessions_lock, 1)) __builtin_arm_wfe();
    g_dump_all_sessions.push_back(s);
    __sync_lock_release(&g_dump_sessions_lock);
    return s;
}

// JNI 函数表槽位（名字 + 在 JNINativeInterface 中的偏移，用 offsetof 让编译器算，绝不手写偏移）。
// env->functions 指向 libart 里全局唯一的 JNINativeInterface，槽位顺序是固定 ABI；dump 成
// 名字->地址，正好和 symbols.log 里 <unresolved> 的 JNI 表调用地址对照。
#define JNI_FN(n) { #n, offsetof(JNINativeInterface, n) }
const struct { const char* name; size_t off; } kJniFns[] = {
    JNI_FN(GetVersion),
    JNI_FN(DefineClass), JNI_FN(FindClass),
    JNI_FN(FromReflectedMethod), JNI_FN(FromReflectedField), JNI_FN(ToReflectedMethod),
    JNI_FN(GetSuperclass), JNI_FN(IsAssignableFrom), JNI_FN(ToReflectedField),
    JNI_FN(Throw), JNI_FN(ThrowNew), JNI_FN(ExceptionOccurred), JNI_FN(ExceptionDescribe),
    JNI_FN(ExceptionClear), JNI_FN(FatalError),
    JNI_FN(PushLocalFrame), JNI_FN(PopLocalFrame),
    JNI_FN(NewGlobalRef), JNI_FN(DeleteGlobalRef), JNI_FN(DeleteLocalRef),
    JNI_FN(IsSameObject), JNI_FN(NewLocalRef), JNI_FN(EnsureLocalCapacity),
    JNI_FN(AllocObject), JNI_FN(NewObject), JNI_FN(NewObjectV), JNI_FN(NewObjectA),
    JNI_FN(GetObjectClass), JNI_FN(IsInstanceOf),
    JNI_FN(GetMethodID),
    JNI_FN(CallObjectMethod),  JNI_FN(CallObjectMethodV),  JNI_FN(CallObjectMethodA),
    JNI_FN(CallBooleanMethod), JNI_FN(CallBooleanMethodV), JNI_FN(CallBooleanMethodA),
    JNI_FN(CallByteMethod),    JNI_FN(CallByteMethodV),    JNI_FN(CallByteMethodA),
    JNI_FN(CallCharMethod),    JNI_FN(CallCharMethodV),    JNI_FN(CallCharMethodA),
    JNI_FN(CallShortMethod),   JNI_FN(CallShortMethodV),   JNI_FN(CallShortMethodA),
    JNI_FN(CallIntMethod),     JNI_FN(CallIntMethodV),     JNI_FN(CallIntMethodA),
    JNI_FN(CallLongMethod),    JNI_FN(CallLongMethodV),    JNI_FN(CallLongMethodA),
    JNI_FN(CallFloatMethod),   JNI_FN(CallFloatMethodV),   JNI_FN(CallFloatMethodA),
    JNI_FN(CallDoubleMethod),  JNI_FN(CallDoubleMethodV),  JNI_FN(CallDoubleMethodA),
    JNI_FN(CallVoidMethod),    JNI_FN(CallVoidMethodV),    JNI_FN(CallVoidMethodA),
    JNI_FN(CallNonvirtualObjectMethod),  JNI_FN(CallNonvirtualObjectMethodV),  JNI_FN(CallNonvirtualObjectMethodA),
    JNI_FN(CallNonvirtualBooleanMethod), JNI_FN(CallNonvirtualBooleanMethodV), JNI_FN(CallNonvirtualBooleanMethodA),
    JNI_FN(CallNonvirtualByteMethod),    JNI_FN(CallNonvirtualByteMethodV),    JNI_FN(CallNonvirtualByteMethodA),
    JNI_FN(CallNonvirtualCharMethod),    JNI_FN(CallNonvirtualCharMethodV),    JNI_FN(CallNonvirtualCharMethodA),
    JNI_FN(CallNonvirtualShortMethod),   JNI_FN(CallNonvirtualShortMethodV),   JNI_FN(CallNonvirtualShortMethodA),
    JNI_FN(CallNonvirtualIntMethod),     JNI_FN(CallNonvirtualIntMethodV),     JNI_FN(CallNonvirtualIntMethodA),
    JNI_FN(CallNonvirtualLongMethod),    JNI_FN(CallNonvirtualLongMethodV),    JNI_FN(CallNonvirtualLongMethodA),
    JNI_FN(CallNonvirtualFloatMethod),   JNI_FN(CallNonvirtualFloatMethodV),   JNI_FN(CallNonvirtualFloatMethodA),
    JNI_FN(CallNonvirtualDoubleMethod),  JNI_FN(CallNonvirtualDoubleMethodV),  JNI_FN(CallNonvirtualDoubleMethodA),
    JNI_FN(CallNonvirtualVoidMethod),    JNI_FN(CallNonvirtualVoidMethodV),    JNI_FN(CallNonvirtualVoidMethodA),
    JNI_FN(GetFieldID),
    JNI_FN(GetObjectField), JNI_FN(GetBooleanField), JNI_FN(GetByteField), JNI_FN(GetCharField),
    JNI_FN(GetShortField), JNI_FN(GetIntField), JNI_FN(GetLongField), JNI_FN(GetFloatField), JNI_FN(GetDoubleField),
    JNI_FN(SetObjectField), JNI_FN(SetBooleanField), JNI_FN(SetByteField), JNI_FN(SetCharField),
    JNI_FN(SetShortField), JNI_FN(SetIntField), JNI_FN(SetLongField), JNI_FN(SetFloatField), JNI_FN(SetDoubleField),
    JNI_FN(GetStaticMethodID),
    JNI_FN(CallStaticObjectMethod),  JNI_FN(CallStaticObjectMethodV),  JNI_FN(CallStaticObjectMethodA),
    JNI_FN(CallStaticBooleanMethod), JNI_FN(CallStaticBooleanMethodV), JNI_FN(CallStaticBooleanMethodA),
    JNI_FN(CallStaticByteMethod),    JNI_FN(CallStaticByteMethodV),    JNI_FN(CallStaticByteMethodA),
    JNI_FN(CallStaticCharMethod),    JNI_FN(CallStaticCharMethodV),    JNI_FN(CallStaticCharMethodA),
    JNI_FN(CallStaticShortMethod),   JNI_FN(CallStaticShortMethodV),   JNI_FN(CallStaticShortMethodA),
    JNI_FN(CallStaticIntMethod),     JNI_FN(CallStaticIntMethodV),     JNI_FN(CallStaticIntMethodA),
    JNI_FN(CallStaticLongMethod),    JNI_FN(CallStaticLongMethodV),    JNI_FN(CallStaticLongMethodA),
    JNI_FN(CallStaticFloatMethod),   JNI_FN(CallStaticFloatMethodV),   JNI_FN(CallStaticFloatMethodA),
    JNI_FN(CallStaticDoubleMethod),  JNI_FN(CallStaticDoubleMethodV),  JNI_FN(CallStaticDoubleMethodA),
    JNI_FN(CallStaticVoidMethod),    JNI_FN(CallStaticVoidMethodV),    JNI_FN(CallStaticVoidMethodA),
    JNI_FN(GetStaticFieldID),
    JNI_FN(GetStaticObjectField), JNI_FN(GetStaticBooleanField), JNI_FN(GetStaticByteField), JNI_FN(GetStaticCharField),
    JNI_FN(GetStaticShortField), JNI_FN(GetStaticIntField), JNI_FN(GetStaticLongField), JNI_FN(GetStaticFloatField), JNI_FN(GetStaticDoubleField),
    JNI_FN(SetStaticObjectField), JNI_FN(SetStaticBooleanField), JNI_FN(SetStaticByteField), JNI_FN(SetStaticCharField),
    JNI_FN(SetStaticShortField), JNI_FN(SetStaticIntField), JNI_FN(SetStaticLongField), JNI_FN(SetStaticFloatField), JNI_FN(SetStaticDoubleField),
    JNI_FN(NewString), JNI_FN(GetStringLength), JNI_FN(GetStringChars), JNI_FN(ReleaseStringChars),
    JNI_FN(NewStringUTF), JNI_FN(GetStringUTFLength), JNI_FN(GetStringUTFChars), JNI_FN(ReleaseStringUTFChars),
    JNI_FN(GetArrayLength),
    JNI_FN(NewObjectArray), JNI_FN(GetObjectArrayElement), JNI_FN(SetObjectArrayElement),
    JNI_FN(NewBooleanArray), JNI_FN(NewByteArray), JNI_FN(NewCharArray), JNI_FN(NewShortArray),
    JNI_FN(NewIntArray), JNI_FN(NewLongArray), JNI_FN(NewFloatArray), JNI_FN(NewDoubleArray),
    JNI_FN(GetBooleanArrayElements), JNI_FN(GetByteArrayElements), JNI_FN(GetCharArrayElements), JNI_FN(GetShortArrayElements),
    JNI_FN(GetIntArrayElements), JNI_FN(GetLongArrayElements), JNI_FN(GetFloatArrayElements), JNI_FN(GetDoubleArrayElements),
    JNI_FN(ReleaseBooleanArrayElements), JNI_FN(ReleaseByteArrayElements), JNI_FN(ReleaseCharArrayElements), JNI_FN(ReleaseShortArrayElements),
    JNI_FN(ReleaseIntArrayElements), JNI_FN(ReleaseLongArrayElements), JNI_FN(ReleaseFloatArrayElements), JNI_FN(ReleaseDoubleArrayElements),
    JNI_FN(GetBooleanArrayRegion), JNI_FN(GetByteArrayRegion), JNI_FN(GetCharArrayRegion), JNI_FN(GetShortArrayRegion),
    JNI_FN(GetIntArrayRegion), JNI_FN(GetLongArrayRegion), JNI_FN(GetFloatArrayRegion), JNI_FN(GetDoubleArrayRegion),
    JNI_FN(SetBooleanArrayRegion), JNI_FN(SetByteArrayRegion), JNI_FN(SetCharArrayRegion), JNI_FN(SetShortArrayRegion),
    JNI_FN(SetIntArrayRegion), JNI_FN(SetLongArrayRegion), JNI_FN(SetFloatArrayRegion), JNI_FN(SetDoubleArrayRegion),
    JNI_FN(RegisterNatives), JNI_FN(UnregisterNatives),
    JNI_FN(MonitorEnter), JNI_FN(MonitorExit),
    JNI_FN(GetJavaVM),
    JNI_FN(GetStringRegion), JNI_FN(GetStringUTFRegion),
    JNI_FN(GetPrimitiveArrayCritical), JNI_FN(ReleasePrimitiveArrayCritical),
    JNI_FN(GetStringCritical), JNI_FN(ReleaseStringCritical),
    JNI_FN(NewWeakGlobalRef), JNI_FN(DeleteWeakGlobalRef),
    JNI_FN(ExceptionCheck),
    JNI_FN(NewDirectByteBuffer), JNI_FN(GetDirectBufferAddress), JNI_FN(GetDirectBufferCapacity),
    JNI_FN(GetObjectRefType),
};
#undef JNI_FN

// ---- hook 回调（user_data 直接是 DumpSession*，O(1) 拿到会话，不查表）----

// 入口窄范围 BLOCK：命中一次即快照全部寄存器（class 1）。
void on_entry(vm_context* ctx, uint64_t address, uint32_t size, void* ud) {
    (void)size;
    auto* s = getOrCreatePerThreadSession((DumpSession*)ud);   // 本线程自己那一份
    if (s->entry_captured) return;
    s->entry_captured = true;
    for (int i = 0; i < 31; i++) vc_reg_read(ctx, (vc_reg)(VC_REG_X0 + i), &s->xregs[i]);
    vc_reg_read(ctx, VC_REG_SP,   &s->sp);
    vc_reg_read(ctx, VC_REG_NZCV, &s->nzcv);
    vc_reg_read(ctx, VC_REG_FPCR, &s->fpcr);
    vc_reg_read(ctx, VC_REG_FPSR, &s->fpsr);
    for (int i = 0; i < 32; i++) vc_reg_read(ctx, (vc_reg)(VC_REG_Q0 + i), s->qregs[i]);
    s->pc = address;   // BLOCK 回调的 address 即入口块地址（不走 vc_reg_read(PC)，避免 block_hook PC 修正）
}

// 全量内存访问：记触达页号（class 3）；并在某页「第一次被写」时，趁写还没落下，先把整页
// 原始内容存快照（=入口值）。UC_HOOK_MEM_WRITE 在写入提交之前触发，此刻 memcpy 读到的是写前值。
// identity map → 页号<<12 就是 host 指针，直接读。读访问不存（没改，跑完读=入口值）。
void on_mem(vm_context* ctx, vc_mem_type type, uint64_t address, int size, int64_t value, void* ud) {
    (void)ctx; (void)value;
    auto* s = getOrCreatePerThreadSession((DumpSession*)ud);   // 本线程自己那一份
    uint64_t p0 = address >> 12;
    uint64_t p1 = (address + (size > 0 ? (uint64_t)size - 1 : 0)) >> 12;
    for (uint64_t p = p0; p <= p1; p++) {
        s->pages.insert(p);
        if (type == VC_MEM_WRITE && s->written.find(p) == s->written.end()) {
            auto& snap = s->written[p];
            snap.resize(PAGE);
            memcpy(snap.data(), (const void*)(p << 12), PAGE);   // 写前整页 = 入口值
        }
    }
}

// 外部跳转：记本次执行路径实际用到的符号（class 4），首个名字为准。
void on_jump(vm_context* ctx, uint64_t address, const char* symbol_name,
             vc_event_action* action, void* ud) {
    (void)ctx; (void)action;
    auto* s = getOrCreatePerThreadSession((DumpSession*)ud);   // 本线程自己那一份
    // 防御：按上限截断（即使上游给了未 null 终止的指针也不会拷成乱码），空/NULL 存空串
    s->symbols.emplace(address,
        symbol_name ? std::string(symbol_name, strnlen(symbol_name, 255)) : std::string());

    // open 家族 → 记下被打开的文件路径（供 finish dump 常规文件；char 设备如 /dev/urandom
    // 在 finish 里按 S_ISREG 过滤掉，天然不 dump、也不会读无限流卡死）。
    if (symbol_name && strstr(symbol_name, "open") && !strstr(symbol_name, "opendir")) {
        // openat(dirfd, path, ...) 路径在 x1；open/fopen(path, ...) 在 x0
        uint64_t p = 0;
        vc_reg_read(ctx, strstr(symbol_name, "openat") ? VC_REG_X1 : VC_REG_X0, &p);
        // identity map：guest 地址即 host 指针。只收绝对路径（过滤相对路径/野指针噪音）
        if (p && *(const char*)p == '/') {
            const char* path = (const char*)p;
            char buf[256]; size_t i = 0;
            for (; i < sizeof(buf) - 1 && path[i]; i++) buf[i] = path[i];
            buf[i] = 0;
            if (i > 1) s->opened_files.insert(buf);
        }
    }
}

// SVC 系统调用：ARM64 上文件打开的真正入口是 openat 系统调用(nr 56)——没有 open 系统调用。
// guest 直接发 openat 系统调用时，external_jump(拦 libc 符号)抓不到，必须在这里拦。
// openat(dirfd, path, flags, mode) → path 在 x1。
void on_svc(vm_context* ctx, uint64_t address, uint32_t syscall_nr, void* ud) {
    (void)address;
    bool is_openat = (syscall_nr == __NR_openat);
#ifdef __NR_openat2
    is_openat = is_openat || (syscall_nr == (uint32_t)__NR_openat2);
#endif
    if (!is_openat) return;
    auto* s = getOrCreatePerThreadSession((DumpSession*)ud);   // 本线程自己那一份
    uint64_t p = 0;
    vc_reg_read(ctx, VC_REG_X1, &p);
    if (p && *(const char*)p == '/') {
        const char* path = (const char*)p;
        char buf[256]; size_t i = 0;
        for (; i < sizeof(buf) - 1 && path[i]; i++) buf[i] = path[i];
        buf[i] = 0;
        if (i > 1) s->opened_files.insert(buf);
    }
}

// ---- 安全读 host 内存：经 /proc/self/mem，遇未映射返回 false 而非崩 ----
bool read_page_safe(int memfd, uint64_t addr, uint8_t* buf) {
    return memfd >= 0 && pread(memfd, buf, PAGE, (off_t)addr) == (ssize_t)PAGE;
}

// ---- 页集合 → 页对齐的连续区间（6.2.3：合并相邻/重复，避免碎片文件）----
std::vector<std::pair<uint64_t, uint64_t>> coalesce(std::vector<uint64_t>& pages) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::sort(pages.begin(), pages.end());
    for (uint64_t p : pages) {
        uint64_t start = p << 12, end = (p + 1) << 12;
        if (!ranges.empty() && start <= ranges.back().second) {
            if (end > ranges.back().second) ranges.back().second = end;   // 相邻/重叠 → 合并
        } else {
            ranges.push_back({start, end});
        }
    }
    return ranges;
}

void write_file(const std::string& path, const void* data, size_t len) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)!write(fd, data, len);
    close(fd);
}

// mkdir -p：逐级创建 path 中的每一层目录（已存在的 EEXIST 忽略）。
void mkdir_p(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    mkdir(path.c_str(), 0755);
}

}  // namespace

// 收尾落盘（emu_stop 回调触发；finish 兜底也走它）。定义在下方 finish 处，先前向声明。
static void do_finalize(DumpSession* s);

// VC_HOOK_EMU_STOP 回调：目标函数正常返回时 start_vm_v2 调到这，落盘就在 h(args) 返回前完成。
static void on_emu_stop(vm_context* ctx, void* ud) {
    (void)ctx;
    // 每条线程在自己返回那一刻，落自己那一份 dump（dumpDir/tid{N}/）
    do_finalize(getOrCreatePerThreadSession((DumpSession*)ud));
}

// ============================================================
//  公开 API
// ============================================================

void* trace_unidbg_dump(void* funcAddr, const char* dumpDir, bool withTrace) {
    std::string dir = dumpDir ? dumpDir : "";
    vm_context* ctx = nullptr;
    void* handle = nullptr;

    if (withTrace) {
        // 同一次运行既 dump 又出完整指令 trace：先用 trace() 建「带 trace 的 handle」——它内部
        // vc_make_handle + 挂逐指令 CODE hook + JNI + DirectWriteBuf，trace 落到 dumpDir/trace/*.lz4；
        // 再把下面 dump 的采样 hook 挂到同一个 ctx。一次跑同时产出「中段快照」+「完整执行流水」，
        // 灌进 Unidbg 后对着 trace 就知道哪一步该补环境。清理走 freeTrace(内部会 flush + vc_free)。
        mkdir(dir.c_str(), 0755);
        std::string tp = dir + "/trace";
        mkdir(tp.c_str(), 0755);
        handle = trace(funcAddr, (char*)tp.c_str(), &ctx);
    } else {
        handle = vc_make_handle(funcAddr, &ctx);
    }
    if (!handle || !ctx) {
        LOGE("trace_unidbg_dump: %s failed", withTrace ? "trace()" : "vc_make_handle");
        return nullptr;
    }

    auto* s = new DumpSession();
    s->ctx         = ctx;
    s->handle      = handle;
    s->dumpDir     = dir;
    s->target_addr = (uint64_t)funcAddr;
    s->withTrace   = withTrace;

    // 入口窄范围 BLOCK → 快照寄存器（class 1/6/7）
    vc_hook_add(ctx, &s->hEntry, VC_HOOK_BLOCK, (void*)on_entry, s,
                s->target_addr, s->target_addr + 4);
    // 全量内存访问 → 收集触达页（class 3）；回调只 insert 页号，极轻
    vc_hook_add(ctx, &s->hMem, VC_HOOK_MEM_ALL, (void*)on_mem, s, 0, 0);
    // 外部跳转 → 收集符号（class 4）+ 拦 libc open 家族(BLR 调 libc 的文件打开)
    vc_hook_add(ctx, &s->hJump, VC_HOOK_EXTERNAL_JUMP, (void*)on_jump, s, 0, 0);
    // SVC → 拦 openat 系统调用(ARM64 文件打开的真正入口，guest 直发系统调用时 external_jump 抓不到)
    vc_hook_add(ctx, &s->hSvc, VC_HOOK_SVC, (void*)on_svc, s, 0, 0);
    // EMU_STOP → 目标函数正常返回那一刻自动落盘（h(args) 返回前文件就写好，不用单独 finish 写）
    vc_hook_add(ctx, &s->hStop, VC_HOOK_EMU_STOP, (void*)on_emu_stop, s, 0, 0);

    {
        std::lock_guard<std::mutex> lk(g_sessions_mtx);
        g_sessions[handle] = s;
    }
    return handle;
}

// 2 参重载 = 只 dump、不开 trace（向后兼容旧调用）
void* trace_unidbg_dump(void* funcAddr, const char* dumpDir) {
    return trace_unidbg_dump(funcAddr, dumpDir, false);
}

// 收尾落盘：由 VC_HOOK_EMU_STOP 回调在目标函数返回那一刻触发（h(args) 返回前文件就写好）。
// 多次调用增量累积：已 dump 过的页（s->dumped_pages）跳过，只写新页。
static void do_finalize(DumpSession* s) {
    s->any_finalized = true;

    // 每条线程落自己的子目录 dumpDir/tid{N}/ —— 多线程并发调用同一函数时各出一份完整 dump 包，
    // 互不覆盖。（s 是本线程的 per-thread session，tid 就是它的线程号。）
    char tidbuf[24];
    snprintf(tidbuf, sizeof(tidbuf), "/tid%u", s->tid);
    mkdir(s->dumpDir.c_str(), 0755);
    std::string dir = s->dumpDir + tidbuf;
    mkdir(dir.c_str(), 0755);

    // class 7：目标 SO 段范围 + 模块基址
    LibraryInfo mod = findModuleByAddressExact(s->target_addr);
    uint64_t so_base = mod.segments_start, so_end = mod.segments_end;

    // 解析 /proc/self/maps（读一次复用）：既写 maps.log(class5)、收集 file-backed 路径(class8
    // 过滤)，也建 VMA 段表——下面把触达页扩展成完整段用。
    struct Vma { uint64_t start, end; bool file_backed; };
    std::vector<Vma> vmas;
    std::set<std::string> mapped_paths;
    {
        int in = open("/proc/self/maps", O_RDONLY);
        if (in >= 0) {
            std::string all; char rb[4096]; ssize_t n;
            while ((n = read(in, rb, sizeof(rb))) > 0) all.append(rb, (size_t)n);
            close(in);
            write_file(dir + "/maps.log", all.data(), all.size());
            size_t pos = 0;
            while (pos < all.size()) {
                size_t eol = all.find('\n', pos);
                if (eol == std::string::npos) eol = all.size();
                unsigned long long st = 0, en = 0;
                if (sscanf(all.c_str() + pos, "%llx-%llx", &st, &en) == 2 && en > st) {
                    size_t slash = all.find('/', pos);
                    bool fb = (slash != std::string::npos && slash < eol);
                    vmas.push_back({(uint64_t)st, (uint64_t)en, fb});
                    if (fb) mapped_paths.insert(all.substr(slash, eol - slash));
                }
                pos = eol + 1;
            }
        }
    }

    // 汇总要落盘的页：触达页(class 3) + 栈窗口(6.2.2 强制覆盖) + 目标 SO 整段(class 2)
    std::vector<uint64_t> pages(s->pages.begin(), s->pages.end());
    if (s->sp) {
        uint64_t lo = (s->sp - STACK_BELOW) >> 12, hi = (s->sp + STACK_ABOVE) >> 12;
        for (uint64_t p = lo; p <= hi; p++) pages.push_back(p);
    }
    if (so_base && so_end > so_base) {
        for (uint64_t p = so_base >> 12; p < ((so_end + PAGE - 1) >> 12); p++) pages.push_back(p);
    }
    // 关键：把触达页扩展成它所属的**完整 VMA 段**（以 maps 中一行 `start-end perms ...` 为单位，
    // file-backed 和匿名段一视同仁）。libc 等自校验 CRC 覆盖整段，只 dump 触达页会漏(未触达页在
    // .bin 里=0 → Unidbg 重放 CRC 对不上)；许多逻辑也按段读匿名数据。只用大小上限挡住巨段
    // (dalvik heap / large object space 256MB 那种)避免 dump 爆炸，其余整段落盘。pages 有序遍历，
    // last 缓存跳过同段重复扫描；expanded 防同段多次展开。
    {
        const uint64_t VMA_EXPAND_CAP = 64ull * 1024 * 1024;   // 单段 64MB 上限
        std::set<uint64_t> expanded;
        const Vma* last = nullptr;
        for (uint64_t p : s->pages) {
            uint64_t addr = p << 12;
            if (last && addr >= last->start && addr < last->end) continue;
            last = nullptr;
            for (const auto& v : vmas) {
                if (addr >= v.start && addr < v.end) {
                    last = &v;
                    if ((v.end - v.start) <= VMA_EXPAND_CAP && !expanded.count(v.start)) {
                        expanded.insert(v.start);
                        for (uint64_t a = v.start; a < v.end; a += PAGE) pages.push_back(a >> 12);
                    }
                    break;
                }
            }
        }
    }
    // 增量去重：只保留没 dump 过的新页（同时登记进 dumped_pages），多次 h(args) 累积不重复落盘。
    std::vector<uint64_t> fresh;
    fresh.reserve(pages.size());
    for (uint64_t p : pages)
        if (s->dumped_pages.insert(p).second) fresh.push_back(p);
    auto ranges = coalesce(fresh);

    // 逐段落盘：被 VM 写过的页用「首次写前快照」(入口值)；其余页(只读过/强制加的栈/SO/扩展段
    // 里没触达的页)在此刻读 host——它们没被 VM 改过，现在读到的就等于入口值。未映射填 0 保持偏移。
    int memfd = open("/proc/self/mem", O_RDONLY);
    uint8_t buf[PAGE];
    for (auto& r : ranges) {
        char name[64];
        snprintf(name, sizeof(name), "%llx_%llx.bin",
                 (unsigned long long)r.first, (unsigned long long)r.second);
        int fd = open((dir + "/" + name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) continue;
        for (uint64_t a = r.first; a < r.second; a += PAGE) {
            auto it = s->written.find(a >> 12);
            if (it != s->written.end()) {
                (void)!write(fd, it->second.data(), PAGE);   // 写前快照 = 入口值
            } else {
                if (!read_page_safe(memfd, a, buf)) memset(buf, 0, PAGE);
                (void)!write(fd, buf, PAGE);                 // 没被写过，现读 = 入口值
            }
        }
        close(fd);
    }
    if (memfd >= 0) close(memfd);

    char line[256];

    // class 1：寄存器快照
    {
        std::string out;
        for (int i = 0; i < 31; i++) {
            snprintf(line, sizeof(line), "X%d=0x%llx\n", i, (unsigned long long)s->xregs[i]);
            out += line;
        }
        snprintf(line, sizeof(line), "SP=0x%llx\nPC=0x%llx\nNZCV=0x%llx\nFPCR=0x%llx\nFPSR=0x%llx\n",
                 (unsigned long long)s->sp, (unsigned long long)s->pc, (unsigned long long)s->nzcv,
                 (unsigned long long)s->fpcr, (unsigned long long)s->fpsr);
        out += line;
        for (int i = 0; i < 32; i++) {
            char hex[33];
            for (int b = 0; b < 16; b++) snprintf(hex + b * 2, 3, "%02x", s->qregs[i][b]);
            snprintf(line, sizeof(line), "Q%d=%s\n", i, hex);
            out += line;
        }
        write_file(dir + "/regs.txt", out.data(), out.size());
    }

    // class 6：入口参数（X0-X7 + SP）
    {
        std::string out;
        for (int i = 0; i < 8; i++) {
            snprintf(line, sizeof(line), "arg%d(X%d)=0x%llx\n", i, i, (unsigned long long)s->xregs[i]);
            out += line;
        }
        snprintf(line, sizeof(line), "SP=0x%llx\n", (unsigned long long)s->sp);
        out += line;
        write_file(dir + "/args.txt", out.data(), out.size());
    }

    // class 4：symbols.log（本次执行路径实际用到的外部符号）
    {
        std::string out;
        for (auto& kv : s->symbols) {
            snprintf(line, sizeof(line), "0x%llx ", (unsigned long long)kv.first);
            out += line;
            out += kv.second.empty() ? "<unresolved>" : kv.second;   // 空名=未解析(多为 JNI 表调用)
            out += "\n";
        }
        write_file(dir + "/symbols.log", out.data(), out.size());
    }

    // class 9：JNI 函数表（jni_table.log）。JNI 表是 libart 里全局唯一的 JNINativeInterface，
    // 所有线程/所有 env 共享同一张 → 只要拿到过一次 functions 指针,全局缓存、所有线程复用。
    // **绝不 AttachCurrentThread**:do_finalize 在 emu_stop 里跑,此刻 SP 在 VM 的 mmap 栈上,且
    // dump 的 worker 线程可能没附着到 ART——调 AttachCurrentThread 会让 ART 的 InitStack 栈边界
    // 检查失败而 abort(踩过)。只用 GetEnv(附着线程直接拿到,没附着就跳过),拿到一次就缓存 ftbl。
    {
        static JavaVM* s_vm = nullptr;
        static const char* s_jni_ftbl = nullptr;   // 全局缓存的 JNINativeInterface 指针
        if (!s_jni_ftbl) {
            if (!s_vm) {
                uint64_t fnAddr = resolveSymbolInSo("/libart.so", "JNI_GetCreatedJavaVMs");
                if (fnAddr) {
                    typedef jint (*getvms_t)(JavaVM**, jsize, jsize*);
                    jsize num = 0;
                    ((getvms_t)fnAddr)(&s_vm, 1, &num);
                }
            }
            JNIEnv* jenv = nullptr;
            if (s_vm) s_vm->GetEnv((void**)&jenv, JNI_VERSION_1_6);   // 只 GetEnv,不 Attach
            if (jenv && jenv->functions) s_jni_ftbl = (const char*)jenv->functions;
        }

        if (s_jni_ftbl) {
            const char* ftbl = s_jni_ftbl;
            std::string out;
            snprintf(line, sizeof(line), "# functions=0x%llx (libart 全局 JNINativeInterface，%zu 项)\n",
                     (unsigned long long)(uintptr_t)ftbl, sizeof(kJniFns) / sizeof(kJniFns[0]));
            out += line;
            for (const auto& e : kJniFns) {
                void* p = *(void* const*)(ftbl + e.off);
                snprintf(line, sizeof(line), "env->functions->%s -> 0x%llx\n",
                         e.name, (unsigned long long)(uintptr_t)p);
                out += line;
            }
            write_file(dir + "/jni_table.log", out.data(), out.size());
            LOGE("trace_unidbg_dump: JNI 表 dump %zu 项 (functions=0x%llx)",
                 sizeof(kJniFns) / sizeof(kJniFns[0]), (unsigned long long)(uintptr_t)ftbl);
        } else {
            LOGE("trace_unidbg_dump: 未取到 JNIEnv，跳过 JNI 表 dump");
        }
    }

    // （maps.log 与 mapped_paths 已在上方页扩展前解析，class 8 直接复用 mapped_paths）

    // class 8：目标运行时 open 过的常规文件 → dump 成 rootfs/ 目录树，供 Unidbg 直接当
    // rootfs 用（Unidbg 设 rootfs 后按原绝对路径去 rootfs 下找）。**保持原路径、镜像成
    // 相对路径**（去掉前导 '/'）：/data/data/pkg/x.dat → rootfs/data/data/pkg/x.dat。
    // 只 dump S_ISREG —— /dev/urandom 等字符设备(无限流)、fifo、socket 自动跳过：
    // 既不会读无限流卡死，语义上随机源也本就该由 Unidbg 侧重新提供/固定。
    {
        std::string rootfsDir = dir + "/rootfs";
        mkdir(rootfsDir.c_str(), 0755);
        std::string manifest;
        char mline[600];
        int dumped = 0, skipped = 0;
        for (const auto& path : s->opened_files) {
            if (mapped_paths.count(path)) {   // 已作为文件映射加载(系统库/目标SO)→ Unidbg 自会加载，跳过
                snprintf(mline, sizeof(mline), "%s\tSKIP(已映射的系统库/SO)\n", path.c_str());
                manifest += mline; skipped++;
                continue;
            }
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {   // 非常规文件(含 /dev/urandom)→跳过
                snprintf(mline, sizeof(mline), "%s\tSKIP(非常规文件/设备)\n", path.c_str());
                manifest += mline; skipped++;
                continue;
            }
            int in = open(path.c_str(), O_RDONLY);
            if (in < 0) {
                snprintf(mline, sizeof(mline), "%s\tSKIP(打不开)\n", path.c_str());
                manifest += mline; skipped++;
                continue;
            }
            // 镜像原路径：rootfs + path（path 以 '/' 开头 → 相对 rootfs 的同名子树），建好父目录
            std::string outPath = rootfsDir + path;
            size_t slash = outPath.find_last_of('/');
            if (slash != std::string::npos) mkdir_p(outPath.substr(0, slash));
            int of = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (of >= 0) {
                char cb[65536]; ssize_t n; size_t total = 0;
                const size_t CAP = 64ull * 1024 * 1024;           // 单文件 64MB 上限，防意外巨/增长文件
                while (total < CAP && (n = read(in, cb, sizeof(cb))) > 0) { (void)!write(of, cb, (size_t)n); total += (size_t)n; }
                close(of);
                snprintf(mline, sizeof(mline), "%s\trootfs%s\t%zu\n", path.c_str(), path.c_str(), total);
                manifest += mline; dumped++;
            }
            close(in);
        }
        write_file(dir + "/files_manifest.txt", manifest.data(), manifest.size());
        LOGE("trace_unidbg_dump: rootfs dump %d 个常规文件, 跳过 %d 个(设备/映射/打不开)", dumped, skipped);
    }

    // class 7：info.txt（模块基址 / 目标偏移 / SO 路径）
    {
        std::string out;
        snprintf(line, sizeof(line), "target_addr=0x%llx\n", (unsigned long long)s->target_addr); out += line;
        snprintf(line, sizeof(line), "module_base=0x%llx\n",  (unsigned long long)so_base);         out += line;
        snprintf(line, sizeof(line), "module_end=0x%llx\n",   (unsigned long long)so_end);          out += line;
        snprintf(line, sizeof(line), "module_offset=0x%llx\n",(unsigned long long)(s->target_addr - so_base)); out += line;
        out += "module_path="; out += mod.name; out += "\n";
        write_file(dir + "/info.txt", out.data(), out.size());
    }

    LOGE("trace_unidbg_dump: 收尾落盘 %zu 段新内存 → %s", ranges.size(), dir.c_str());
}

// 释放：和 freeTrace 一样只负责清理（emu_stop 已在 h(args) 返回时把文件写好了）。
// 兜底：万一没触发过 emu_stop（如目标没正常返回/没调 h），这里补落一次盘。
void trace_unidbg_dump_finish(void* wrapper_func) {
    DumpSession* master = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_sessions_mtx);
        auto it = g_sessions.find(wrapper_func);
        if (it == g_sessions.end()) return;
        master = it->second;
        g_sessions.erase(it);
    }
    // 摘出本 master 下所有 per-thread session（正常都已在各自 emu_stop 落过盘；没落的这里兜底）。
    // 先在锁内摘链表，再在锁外做 finalize（文件 I/O 慢，不占 spinlock）。
    std::vector<DumpSession*> mine;
    while (__sync_lock_test_and_set(&g_dump_sessions_lock, 1)) __builtin_arm_wfe();
    for (auto it = g_dump_all_sessions.begin(); it != g_dump_all_sessions.end(); ) {
        if ((*it)->master == master) { mine.push_back(*it); it = g_dump_all_sessions.erase(it); }
        else ++it;
    }
    __sync_lock_release(&g_dump_sessions_lock);
    for (DumpSession* ps : mine) {
        if (!ps->any_finalized) do_finalize(ps);   // 兜底（该线程没触发过 emu_stop）
        delete ps;
    }
    tls_dump_sessions.erase(master);   // 清本线程 tls；其它线程条目随 handle 释放不再用

    // 句柄释放：withTrace 时由 trace() 建，走 freeTrace(flush 各线程 trace 尾 + 内部 vc_free)；否则 vc_free。
    if (master->withTrace) freeTrace((uint64_t)master->handle);
    else                   vc_free(master->ctx);
    delete master;
}
