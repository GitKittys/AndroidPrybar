#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "ARM64Emulator.h"

#define LOG_TAG "TraceDemo"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {

// 这个文件故意写成“教学示例”的风格。
// 下面每一组 demo 都尽量回答三件事：
// 1. 这个阶段该调用哪个 Prybar API，
// 2. 这个 API 更适合什么场景，
// 3. 跑完之后你应该重点看什么结果。

static volatile uint64_t g_atomic_test_val __attribute__((aligned(8))) = 0;
static volatile uint32_t g_atomic_test_val32 __attribute__((aligned(4))) = 0;
static volatile uint16_t g_atomic_test_val16 __attribute__((aligned(2))) = 0;
static volatile uint8_t g_atomic_test_val8 __attribute__((aligned(1))) = 0;

// 这里专门演示一个很实用的写法：
// 不要把策略和统计信息写死在全局里，而是通过 user_data 传给回调。
// 这样同一个回调函数就可以服务多个目标函数，各自带不同配置。
struct CustomCallbackState {
    int codeCount = 0;
    int blockCount = 0;
    int externalJumpCount = 0;
    bool skippedSymbol = false;
    uint64_t lastPc = 0;
    uint64_t lastX0 = 0;
    const char* symbolToSkip = "strlen";
    uint64_t forcedReturnX0 = 6;
    char lastSymbol[64] = {};
    char resolvedSymbol[64] = {};
};

std::vector<char> makeWritablePath(const std::string& text) {
    std::vector<char> bytes(text.begin(), text.end());
    bytes.push_back('\0');
    return bytes;
}

std::string jstringToUtf8(JNIEnv* env, jstring value) {
    if (env == nullptr || value == nullptr) {
        return "<null>";
    }

    const char* utf = env->GetStringUTFChars(value, nullptr);
    if (utf == nullptr) {
        return "<GetStringUTFChars failed>";
    }

    std::string result(utf);
    env->ReleaseStringUTFChars(value, utf);
    return result;
}

void recordCheck(const char* name, bool ok, int& pass, int& fail) {
    if (ok) {
        ++pass;
        LOGD("[PASS] %s", name);
    } else {
        ++fail;
        LOGD("[FAIL] %s", name);
    }
}

uint32_t packCounts(int pass, int fail) {
    return (static_cast<uint32_t>(pass) << 16) | static_cast<uint32_t>(fail);
}

void unpackCounts(uint32_t packed, int& pass, int& fail) {
    pass = static_cast<int>((packed >> 16) & 0xFFFFu);
    fail = static_cast<int>(packed & 0xFFFFu);
}

uint64_t __attribute__((noinline)) custom_callback_target(uint64_t seed) {
    volatile uint64_t mixed = (seed ^ 0x55AA55AAULL) + 0x1234ULL;
    mixed += static_cast<uint64_t>(strlen("Prybar"));
    return mixed;
}

uint64_t __attribute__((noinline)) plain_trace_target(uint64_t value) {
    volatile uint64_t base = value + 3;
    volatile uint64_t len = static_cast<uint64_t>(strlen("TraceDemo"));
    return (base * 2ULL) + len;
}

// 这一组回调示例，想讲的是 Prybar 最灵活的一种用法：
// 先把函数放进 VM 里跑，再在事件回调里一边观察、一边决定接下来怎么处理。
// 如果你的需求不只是“记日志”，而是“运行时顺手改行为”，那基本就是这一套。
void custom_event_callback(vm_context* ctx, vc_cpu_event_info* event, void* user_data) {
    auto* state = reinterpret_cast<CustomCallbackState*>(user_data);
    if (ctx == nullptr || event == nullptr || state == nullptr) {
        return;
    }

    switch (event->type) {
        case VC_CPU_EVENT_CODE:
            state->codeCount++;
            break;
        case VC_CPU_EVENT_BLOCK:
            state->blockCount++;
            break;
        case VC_CPU_EVENT_EXTERNAL_JUMP: {
            state->externalJumpCount++;
            vc_reg_read(ctx, VC_REG_PC, &state->lastPc);
            vc_reg_read(ctx, VC_REG_X0, &state->lastX0);

            const char* symbol = event->symbol_name ? event->symbol_name : "<unknown>";
            std::snprintf(state->lastSymbol, sizeof(state->lastSymbol), "%s", symbol);

            const char* resolved = vc_lookup_symbol(ctx, event->address);
            if (resolved != nullptr) {
                std::snprintf(state->resolvedSymbol, sizeof(state->resolvedSymbol), "%s", resolved);
            }

            if (event->symbol_name != nullptr &&
                std::strcmp(event->symbol_name, state->symbolToSkip) == 0) {
                vc_reg_write(ctx, VC_REG_X0, &state->forcedReturnX0);
                event->action = VC_ACTION_SKIP;
                state->skippedSymbol = true;
            }
            break;
        }
        default:
            break;
    }
}

std::string run_custom_callback_demo() {
    vm_context* ctx = nullptr;
    CustomCallbackState state{};

    // vc_make_handle 的核心意义很简单：
    // 它把一个原始函数包装成“还能像普通函数一样调用”的 VM 句柄。
    // 适合那种你既想保留原来的调用方式，又想把执行过程交给 Prybar 接管的场景。
    auto handle = reinterpret_cast<uint64_t(*)(uint64_t)>(
        vc_make_handle(reinterpret_cast<void*>(custom_callback_target),
                       &ctx,
                       custom_event_callback,
                       &state));
    if (handle == nullptr || ctx == nullptr) {
        return "[custom_callback]\nvc_make_handle failed";
    }

    // 事件掩码本质上是在“性能”和“细节”之间做取舍。
    // 这里把 CODE 打开，是因为这个工程更偏教学，想让输出更直观。
    // 真正落地时，如果你不需要逐条指令观察，通常没必要一直开着。
    vc_set_event_mask(ctx, VC_EVENT_DEFAULT_MASK | VC_EVENT_ENABLE_CODE);

    const uint64_t input = 0x12345678ULL;
    const uint64_t hostResult = custom_callback_target(input);
    const uint64_t vmResult = handle(input);

    std::ostringstream out;
    out << "[custom_callback]\n";
    out << "host_result = 0x" << std::hex << hostResult << "\n";
    out << "vm_result   = 0x" << std::hex << vmResult << "\n";
    out << "code_events = " << std::dec << state.codeCount << "\n";
    out << "block_events = " << state.blockCount << "\n";
    out << "external_jumps = " << state.externalJumpCount << "\n";
    out << "skipped_symbol = " << (state.skippedSymbol ? "true" : "false") << "\n";
    out << "skip_target = " << state.symbolToSkip << "\n";
    out << "forced_x0 = " << state.forcedReturnX0 << "\n";
    out << "last_pc = 0x" << std::hex << state.lastPc << "\n";
    out << "last_x0 = 0x" << std::hex << state.lastX0 << "\n";
    out << "last_symbol = " << state.lastSymbol << "\n";
    out << "resolved_symbol = "
        << (state.resolvedSymbol[0] != '\0' ? state.resolvedSymbol : "<unavailable>");

    vc_free(ctx);
    return out.str();
}

jstring __attribute__((noinline)) jni_demo_target(JavaVM* javaVm) {
    // 这个目标函数故意塞了一组常见 JNI 操作进去，
    // 目的不是炫技，而是让生成出来的 trace 日志更有代表性，
    // 这样别人看示例时，能很快明白 Prybar 在 JNI 场景下到底能看到什么。
    if (javaVm == nullptr) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    if (javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (javaVm->AttachCurrentThread(&env, nullptr) != 0) {
            return nullptr;
        }
    }
    if (env == nullptr) {
        return nullptr;
    }

    jclass clsString = env->FindClass("java/lang/String");
    jclass clsInteger = env->FindClass("java/lang/Integer");
    jclass clsArrayList = env->FindClass("java/util/ArrayList");
    if (clsString == nullptr || clsInteger == nullptr || clsArrayList == nullptr) {
        return env->NewStringUTF("jni-demo-class-lookup-failed");
    }

    jmethodID midLength = env->GetMethodID(clsString, "length", "()I");
    jmethodID midHashCode = env->GetMethodID(clsString, "hashCode", "()I");
    jmethodID midValueOf = env->GetStaticMethodID(clsString, "valueOf", "(I)Ljava/lang/String;");
    jmethodID midCtorList = env->GetMethodID(clsArrayList, "<init>", "()V");
    jmethodID midAdd = env->GetMethodID(clsArrayList, "add", "(Ljava/lang/Object;)Z");
    jmethodID midSize = env->GetMethodID(clsArrayList, "size", "()I");
    jmethodID midParseInt =
        env->GetStaticMethodID(clsInteger, "parseInt", "(Ljava/lang/String;)I");

    jstring source = env->NewStringUTF("jni trace demo");
    jint length = env->CallIntMethod(source, midLength);
    jint hashCode = env->CallIntMethod(source, midHashCode);

    jstring numberString = env->NewStringUTF("2026");
    jint parsed = env->CallStaticIntMethod(clsInteger, midParseInt, numberString);
    jstring rendered = static_cast<jstring>(env->CallStaticObjectMethod(clsString, midValueOf, 2026));

    jintArray intArray = env->NewIntArray(4);
    jint numbers[4] = {7, 11, 19, 23};
    env->SetIntArrayRegion(intArray, 0, 4, numbers);
    jint* arrayPtr = env->GetIntArrayElements(intArray, nullptr);
    jint arrayChecksum = 0;
    for (int index = 0; index < 4; ++index) {
        arrayChecksum += arrayPtr[index];
    }
    env->ReleaseIntArrayElements(intArray, arrayPtr, JNI_ABORT);

    jobject list = env->NewObject(clsArrayList, midCtorList);
    env->CallBooleanMethod(list, midAdd, source);
    env->CallBooleanMethod(list, midAdd, rendered);
    jint listSize = env->CallIntMethod(list, midSize);

    jobject globalRef = env->NewGlobalRef(source);
    env->DeleteGlobalRef(globalRef);

    env->MonitorEnter(list);
    env->MonitorExit(list);

    std::ostringstream out;
    out << "jni-demo-ok"
        << " len=" << length
        << " hash=0x" << std::hex << hashCode
        << " parsed=" << std::dec << parsed
        << " array_checksum=" << arrayChecksum
        << " list_size=" << listSize;

    jstring result = env->NewStringUTF(out.str().c_str());

    env->DeleteLocalRef(list);
    env->DeleteLocalRef(intArray);
    env->DeleteLocalRef(rendered);
    env->DeleteLocalRef(numberString);
    env->DeleteLocalRef(source);
    env->DeleteLocalRef(clsArrayList);
    env->DeleteLocalRef(clsInteger);
    env->DeleteLocalRef(clsString);
    return result;
}

std::string run_jni_trace_demo(JNIEnv* env, const std::string& filesDir) {
    JavaVM* javaVm = nullptr;
    if (env == nullptr || env->GetJavaVM(&javaVm) != JNI_OK || javaVm == nullptr) {
        return "[jni_trace]\nGetJavaVM failed";
    }

    const std::string logPath = filesDir + "/prybar-jni-trace.log";
    auto writablePath = makeWritablePath(logPath);

    // trace() 是最省心的一条路：
    // 你给它一个函数和一个日志路径，它就帮你包一层 trace 版句柄出来。
    // 如果你当前只是想先拿到日志，而不是在回调里改行为，那优先从它开始最合适。
    auto traced = reinterpret_cast<jstring(*)(JavaVM*)>(
        trace(reinterpret_cast<void*>(jni_demo_target), writablePath.data()));
    if (traced == nullptr) {
        return "[jni_trace]\ntrace() failed";
    }

    jstring result = traced(javaVm);
    std::string resultText = jstringToUtf8(env, result);
    if (result != nullptr) {
        env->DeleteLocalRef(result);
    }

    freeTrace(reinterpret_cast<uint64_t>(traced));

    std::ostringstream out;
    out << "[jni_trace]\n";
    out << "result = " << resultText << "\n";
    out << "trace_log = " << logPath << "\n";
    out << "tip = open the log file to inspect intercepted JNI calls";
    return out.str();
}

uint32_t __attribute__((noinline)) atomic_demo_target() {
    // 这一组原子示例更偏“正确性验证”。
    // 它不是为了展示某个花哨效果，而是想回答一个很实际的问题：
    // 这类指令放进 VM 里之后，行为到底有没有跑偏。
    // 在你准备追更大的真实目标之前，先拿这种小而硬的例子做自测会很安心。
    int pass = 0;
    int fail = 0;

    {
        g_atomic_test_val = 0x1234;
        uint64_t loaded = 0;
        uint32_t status = 1;
        asm volatile(
            "ldxr %0, [%2]\n"
            "add %0, %0, #1\n"
            "stxr %w1, %0, [%2]\n"
            : "=&r"(loaded), "=&r"(status)
            : "r"(&g_atomic_test_val)
            : "memory");
        recordCheck("LDXR/STXR 64-bit status==0", status == 0, pass, fail);
        recordCheck("LDXR/STXR 64-bit value==0x1235", g_atomic_test_val == 0x1235, pass, fail);
    }

    {
        g_atomic_test_val32 = 50;
        uint32_t expected = 50;
        uint32_t desired = 100;
        asm volatile(
            "cas %w0, %w1, [%2]\n"
            : "+r"(expected)
            : "r"(desired), "r"(&g_atomic_test_val32)
            : "memory");
        recordCheck("CAS 32-bit value==100", g_atomic_test_val32 == 100, pass, fail);
        recordCheck("CAS 32-bit old==50", expected == 50, pass, fail);
    }

    {
        g_atomic_test_val = 0xABCD;
        uint64_t loaded = 0;
        uint32_t status = 1;
        asm volatile(
            "ldaxr %0, [%2]\n"
            "add %0, %0, #0x10\n"
            "stlxr %w1, %0, [%2]\n"
            : "=&r"(loaded), "=&r"(status)
            : "r"(&g_atomic_test_val)
            : "memory");
        recordCheck("LDAXR/STLXR status==0", status == 0, pass, fail);
        recordCheck("LDAXR/STLXR value==0xABDD", g_atomic_test_val == 0xABDD, pass, fail);
    }

    {
        g_atomic_test_val = 0x1111;
        uint64_t oldValue = 0;
        uint64_t newValue = 0x2222;
        asm volatile(
            "swp %0, %1, [%2]\n"
            : "=r"(oldValue)
            : "r"(newValue), "r"(&g_atomic_test_val)
            : "memory");
        recordCheck("SWP 64-bit old==0x1111", oldValue == 0x1111, pass, fail);
        recordCheck("SWP 64-bit value==0x2222", g_atomic_test_val == 0x2222, pass, fail);
    }

    {
        g_atomic_test_val32 = 100;
        uint32_t addValue = 50;
        uint32_t oldValue = 0;
        asm volatile(
            "ldadd %w1, %w0, [%2]\n"
            : "=r"(oldValue)
            : "r"(addValue), "r"(&g_atomic_test_val32)
            : "memory");
        recordCheck("LDADD 32-bit old==100", oldValue == 100, pass, fail);
        recordCheck("LDADD 32-bit value==150", g_atomic_test_val32 == 150, pass, fail);
    }

    {
        g_atomic_test_val16 = 0x1234;
        uint32_t loaded = 0;
        asm volatile(
            "ldarh %w0, [%1]\n"
            : "=r"(loaded)
            : "r"(&g_atomic_test_val16)
            : "memory");
        recordCheck("LDARH loaded==0x1234", (loaded & 0xFFFFu) == 0x1234u, pass, fail);

        uint32_t newValue = 0x5678;
        asm volatile(
            "stlrh %w0, [%1]\n"
            :
            : "r"(newValue), "r"(&g_atomic_test_val16)
            : "memory");
        recordCheck("STLRH value==0x5678", g_atomic_test_val16 == 0x5678u, pass, fail);
    }

    {
        g_atomic_test_val8 = 10;
        uint32_t addValue = 5;
        uint32_t oldValue = 0;
        asm volatile(
            "ldaddb %w1, %w0, [%2]\n"
            : "=r"(oldValue)
            : "r"(addValue), "r"(&g_atomic_test_val8)
            : "memory");
        recordCheck("LDADDB old==10", (oldValue & 0xFFu) == 10u, pass, fail);
        recordCheck("LDADDB value==15", g_atomic_test_val8 == 15u, pass, fail);
    }

    {
        std::atomic<int> counter{0};
        for (int index = 0; index < 100; ++index) {
            counter.fetch_add(1, std::memory_order_seq_cst);
        }
        recordCheck("std::atomic fetch_add counter==100", counter.load() == 100, pass, fail);

        std::atomic<int> casValue{10};
        int expected = 10;
        bool swapped = casValue.compare_exchange_strong(
            expected, 20, std::memory_order_seq_cst, std::memory_order_seq_cst);
        recordCheck("std::atomic compare_exchange success", swapped && casValue.load() == 20, pass, fail);

        expected = 10;
        swapped = casValue.compare_exchange_strong(
            expected, 30, std::memory_order_seq_cst, std::memory_order_seq_cst);
        recordCheck("std::atomic compare_exchange fail updates expected",
                    !swapped && expected == 20,
                    pass,
                    fail);

        std::atomic<uint64_t> bitmask{0};
        bitmask.fetch_or(0xFF00, std::memory_order_seq_cst);
        bitmask.fetch_xor(0x0F00, std::memory_order_seq_cst);
        recordCheck("std::atomic bit operations value==0xF000",
                    bitmask.load() == 0xF000,
                    pass,
                    fail);
    }

    return packCounts(pass, fail);
}

std::string run_atomic_demo(const std::string& filesDir) {
    const uint32_t hostPacked = atomic_demo_target();

    vm_context* ctx = nullptr;
    // 这里先跑一遍 host，再跑一遍 vc_make_handle 版，
    // 是因为“先对比结果，再做深入分析”是很实用的一种接入习惯。
    // 以后你拿自己的目标函数来测时，也很推荐先走这一步。
    auto handle = reinterpret_cast<uint32_t(*)()>(
        vc_make_handle(reinterpret_cast<void*>(atomic_demo_target), &ctx));
    if (handle == nullptr || ctx == nullptr) {
        return "[atomic_vm]\nvc_make_handle failed";
    }

    const uint32_t vmPacked = handle();
    vc_free(ctx);

    const std::string logPath = filesDir + "/prybar-atomic-trace.log";
    auto writablePath = makeWritablePath(logPath);
    // 接着再用 trace() 跑同一个目标，
    // 这样就能把“结果对比”和“日志观察”两件事放在一个例子里一起讲清楚。
    auto traced = reinterpret_cast<uint32_t(*)()>(
        trace((void*)(atomic_demo_target), writablePath.data()));

    uint32_t tracePacked = 0;
    if (traced != nullptr) {
        tracePacked = traced();
        freeTrace(reinterpret_cast<uint64_t>(traced));
    }

    int hostPass = 0;
    int hostFail = 0;
    int vmPass = 0;
    int vmFail = 0;
    int tracePass = 0;
    int traceFail = 0;
    unpackCounts(hostPacked, hostPass, hostFail);
    unpackCounts(vmPacked, vmPass, vmFail);
    unpackCounts(tracePacked, tracePass, traceFail);

    std::ostringstream out;
    out << "[atomic_vm]\n";
    out << "host_pass = " << hostPass << ", host_fail = " << hostFail << "\n";
    out << "vm_pass = " << vmPass << ", vm_fail = " << vmFail << "\n";
    out << "trace_pass = " << tracePass << ", trace_fail = " << traceFail << "\n";
    out << "trace_log = " << logPath << "\n";
    out << "tip = this demo covers ARM64 atomics plus std::atomic helpers";
    return out.str();
}

std::string run_plain_trace_demo(const std::string& filesDir) {
    const std::string logPath = filesDir + "/prybar-plain-trace.log";
    auto writablePath = makeWritablePath(logPath);

    // 这是给第一次接 Prybar 的人准备的最小示例：
    // 包一个函数，跑一下，然后去看日志。
    // 如果你只想确认“我接进来之后能不能先出日志”，先看这个就够了。
    auto traced = reinterpret_cast<uint64_t(*)(uint64_t)>(
        trace(reinterpret_cast<void*>(plain_trace_target), writablePath.data()));
    if (traced == nullptr) {
        return "[plain_trace]\ntrace() failed";
    }

    const uint64_t input = 21;
    const uint64_t result = traced(input);
    // 关闭这个trace会话
    freeTrace(reinterpret_cast<uint64_t>(traced));

    std::ostringstream out;
    out << "[plain_trace]\n";
    out << "trace_result = " << result << "\n";
    out << "trace_log = " << logPath << "\n";
    out << "tip = open the log file above to inspect the plain trace wrapper output";
    return out.str();
}

}  // 匿名命名空间

extern "C" JNIEXPORT jstring JNICALL
Java_com_prybar_tracedemo_MainActivity_stringFromJNI(JNIEnv* env, jobject /* this */) {
    std::string intro =
        "TraceDemo links a prebuilt libtrace.so and now includes custom callback, "
        "JNI trace, atomic instruction, and plain trace examples.";
    return env->NewStringUTF(intro.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_prybar_tracedemo_MainActivity_runNativeDemo(
    JNIEnv* env, jobject /* this */, jstring filesDir_) {
    const char* rawFilesDir = env->GetStringUTFChars(filesDir_, nullptr);
    std::string filesDir = rawFilesDir != nullptr ? rawFilesDir : "";
    if (rawFilesDir != nullptr) {
        env->ReleaseStringUTFChars(filesDir_, rawFilesDir);
    }

    // 这里的示例顺序是故意这么排的：
    // 1. 先看自定义回调，因为它最能体现 Prybar 的控制能力；
    // 2. 再看 JNI trace，因为这是 Android 里最贴近真实业务的场景；
    // 3. 再看原子测试，因为它更像正确性和兼容性自测；
    // 4. 最后看普通 trace，把最小接入路径单独留出来。
    const std::string callbackDemo = run_custom_callback_demo(); //添加自定义回调
    const std::string jniTraceDemo = run_jni_trace_demo(env, filesDir); // 测试jnitrace
    const std::string atomicDemo = run_atomic_demo(filesDir); // 测试原子指令是否支持
    const std::string plainTraceDemo = run_plain_trace_demo(filesDir); // 测试普通trace是否支持

    LOGD("%s", callbackDemo.c_str());
    LOGD("%s", jniTraceDemo.c_str());
    LOGD("%s", atomicDemo.c_str());
    LOGD("%s", plainTraceDemo.c_str());

    std::ostringstream out;
    out << "Prybar / TraceDemo sample completed.\n";
    out << "All generated trace logs live under:\n" << filesDir << "\n\n";
    out << callbackDemo << "\n\n";
    out << jniTraceDemo << "\n\n";
    out << atomicDemo << "\n\n";
    out << plainTraceDemo;
    return env->NewStringUTF(out.str().c_str());
}
