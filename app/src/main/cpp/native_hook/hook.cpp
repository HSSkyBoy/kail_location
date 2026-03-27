#include <jni.h>

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <dobby.h>

#include "sensor_simulator.h"

#define LOG_TAG "NativeHook"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Symbol names
static constexpr const char* kSensorServiceLib = "libsensorservice.so";
static constexpr const char* kThreadLoopSymbol = "_ZN7android13SensorService9threadLoopEv";

// Original function pointer type
typedef bool (*ThreadLoopFunc)(void*);

// Global state
static ThreadLoopFunc original_thread_loop = nullptr;
static bool hook_installed = false;
static bool initialized = false;

// Forward declaration
extern "C" bool hooked_threadLoop(void* this_ptr);

// ============================================================================
// Symbol Resolution
// ============================================================================

static void* resolve_symbol(const char* lib, const char* symbol) {
    ALOGI("Resolving symbol: %s from %s", symbol, lib);
    
    void* handle = dlopen(lib, RTLD_NOW);
    if (!handle) {
        ALOGE("dlopen %s failed: %s", lib, dlerror());
        return nullptr;
    }
    
    ALOGI("dlopen succeeded, now dlsym...");
    
    void* sym = dlsym(handle, symbol);
    if (!sym) {
        ALOGE("dlsym %s failed: %s", symbol, dlerror());
        return nullptr;
    }
    
    ALOGI("Resolved %s -> %p", symbol, sym);
    return sym;
}

// ============================================================================
// mSensorEventBuffer Access
// ============================================================================

// SensorEvent structure size (typical)
// struct sensors_event_t { int32_t version; int32_t sensor; int32_t type; int64_t timestamp; float data[16]; };
// Size = 4 + 4 + 4 + 8 + 64 = 84 bytes, aligned to 88 bytes

static constexpr size_t kSensorEventSize = 88;
static constexpr size_t kMaxEvents = 256;

// Try to find mSensorEventBuffer offset in SensorService
// This is a heuristic - actual offset may vary by Android version
struct SensorServiceOffsets {
    uint32_t sensor_event_buffer;  // mSensorEventBuffer offset
    uint32_t event_count;         // mSensorEventCount offset
};

// Known offsets for different Android versions
// These are estimated values and may need adjustment
static const std::vector<SensorServiceOffsets> kOffsetCandidates = {
    {0x1B0, 0x1B8},  // Android 13
    {0x1C0, 0x1C8},  // Android 14
    {0x1D0, 0x1D8},  // Android 15
};

static sensors_event_t* get_sensor_buffer(void* this_ptr) {
    if (!this_ptr) return nullptr;
    
    // Try each known offset
    for (const auto& offsets : kOffsetCandidates) {
        auto* buffer = reinterpret_cast<sensors_event_t*>(
            reinterpret_cast<char*>(this_ptr) + offsets.sensor_event_buffer);
        
        // Sanity check - buffer should be readable
        if (buffer) {
            ALOGD("Found sensor buffer at offset 0x%x -> %p", 
                  offsets.sensor_event_buffer, buffer);
            return buffer;
        }
    }
    
    // Fallback: try common offset
    static constexpr uint32_t kCommonOffset = 0x1B0;
    return reinterpret_cast<sensors_event_t*>(
        reinterpret_cast<char*>(this_ptr) + kCommonOffset);
}

static size_t get_event_count(void* this_ptr) {
    if (!this_ptr) return 0;
    
    // Try known offsets
    for (const auto& offsets : kOffsetCandidates) {
        auto* count = reinterpret_cast<size_t*>(
            reinterpret_cast<char*>(this_ptr) + offsets.event_count);
        if (count && *count > 0 && *count <= kMaxEvents) {
            ALOGD("Found event count at offset 0x%x: %zu", 
                  offsets.event_count, *count);
            return *count;
        }
    }
    
    // Default to reasonable count
    return kMaxEvents;
}

// ============================================================================
// Hook Function
// ============================================================================

extern "C" bool hooked_threadLoop(void* this_ptr) {
    // Call original function first
    bool result = false;
    if (original_thread_loop) {
        result = original_thread_loop(this_ptr);
    }
    
    // Try to get and modify sensor events
    sensors_event_t* buffer = get_sensor_buffer(this_ptr);
    size_t count = get_event_count(this_ptr);
    
    if (buffer && count > 0 && count <= kMaxEvents) {
        // Process sensor events with our simulator
        gait::SensorSimulator::Get().ProcessSensorEvents(buffer, count);
        
        ALOGD("Processed %zu sensor events", count);
    }
    
    return result;
}

// ============================================================================
// JNI Interface for Parameter Setting
// ============================================================================

extern "C" {

// Called from Kotlin via JNI
JNIEXPORT void JNICALL 
Java_com_kail_location_xposed_FakeLocState_nativeSetGaitParams(
    JNIEnv* env, 
    jclass clazz, 
    jfloat spm, 
    jint mode, 
    jboolean enable
) {
    ALOGI("JNI: Set gait params spm=%.2f, mode=%d, enable=%d", spm, mode, enable ? 1 : 0);
    gait::SensorSimulator::Get().UpdateParams(spm, mode, enable);
}

JNIEXPORT jboolean JNICALL 
Java_com_kail_location_xposed_FakeLocState_nativeReloadConfig(
    JNIEnv* env, 
    jclass clazz
) {
    ALOGI("JNI: Reload config");
    return gait::SensorSimulator::Get().ReloadConfig() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

// ============================================================================
// Hook Installation
// ============================================================================

static bool install_hook() {
    ALOGI("Installing SensorService::threadLoop hook...");
    
    // Resolve symbol
    void* thread_loop_addr = resolve_symbol(kSensorServiceLib, kThreadLoopSymbol);
    if (!thread_loop_addr) {
        ALOGE("Failed to resolve threadLoop symbol");
        return false;
    }
    
    original_thread_loop = reinterpret_cast<ThreadLoopFunc>(thread_loop_addr);
    
    // Install Dobby hook
    int ret = DobbyHook(
        thread_loop_addr,
        reinterpret_cast<void*>(&hooked_threadLoop),
        reinterpret_cast<void**>(&original_thread_loop)
    );
    
    if (ret != 0) {
        ALOGE("DobbyHook failed: %d", ret);
        return false;
    }
    
    ALOGI("DobbyHook installed successfully!");
    hook_installed = true;
    return true;
}

// ============================================================================
// Constructor - Auto Initialize
// ============================================================================

__attribute__((constructor))
static void native_hook_init() {
    ALOGI("========================================");
    ALOGI("Native Hook Library Loading...");
    ALOGI("========================================");
    
    // Initialize sensor simulator
    gait::SensorSimulator::Get().Init();
    
    // Try to install hook
    if (install_hook()) {
        ALOGI("Hook installation SUCCESS!");
    } else {
        ALOGE("Hook installation FAILED!");
        ALOGE("Sensor simulation will run in standalone mode");
    }
    
    // Try to reload config from file
    gait::SensorSimulator::Get().ReloadConfig();
    
    ALOGI("Native Hook Library Loaded!");
    ALOGI("========================================");
}

// ============================================================================
// Destructor
// ============================================================================

__attribute__((destructor))
static void native_hook_destroy() {
    ALOGI("Native Hook Library Unloading...");
    if (hook_installed && original_thread_loop) {
        DobbyDestroy((void*)original_thread_loop);
        ALOGI("Hook destroyed");
    }
}
