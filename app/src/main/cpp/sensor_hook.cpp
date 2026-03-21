//
// Created by fuqiuluo on 2024/10/15.
//
#include <dobby.h>
#include <unistd.h>
#include "sensor_hook.h"
#include "logging.h"
#include "elf_util.h"
#include "dobby_hook.h"

#define LIBSF_PATH_64 "/system/lib64/libsensorservice.so"
#define LIBSF_PATH_32 "/system/lib/libsensorservice.so"
#define LIBS_PATH_64 "/system/lib64/libsensor.so"
#define LIBS_PATH_32 "/system/lib/libsensor.so"

extern bool enableSensorHook;

// _ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventm
OriginalSensorEventQueueWriteType OriginalSensorEventQueueWrite = nullptr;

OriginalConvertToSensorEventType OriginalConvertToSensorEvent = nullptr;

int64_t SensorEventQueueWrite(void *tube, void *events, int64_t numEvents) {
    if (enableSensorHook) {
        LOGD("SensorEventQueueWrite called");
    }
    return OriginalSensorEventQueueWrite(tube, events, numEvents);
}

void ConvertToSensorEvent(void *src, void *dst) {
    // 无论是否开启 Hook，始终先调用原始函数以确保结构体被正确初始化
    OriginalConvertToSensorEvent(src, dst);

    if (enableSensorHook) {
        // 从已初始化的 dst 中获取类型 (偏移 12 字节)
        auto type = *(int32_t *)((char*)dst + 12);
        auto handle = *(int32_t *)((char*)dst + 8);

        LOGD("ConvertToSensorEvent (Hooked): handle=%d, type=%d", handle, type);

        // 根据传感器类型修改数据
        switch (type) {
            case 18: // SENSOR_TYPE_STEP_DETECTOR
                *(float *)((char*)dst + 16) = 1.0f; // 步数检测通常返回 1.0
                LOGD("Sensor Data (Type 18): modified to 1.0");
                break;
            case 19: // SENSOR_TYPE_STEP_COUNTER
                *(uint64_t *)((char*)dst + 16) = 99999; // 步数计数器返回累计步数
                LOGD("Sensor Data (Type 19): modified to 99999");
                break;
            case 5:  // SENSOR_TYPE_LIGHT (光线传感器)
                *(float *)((char*)dst + 16) = 100.0f; // 设置一个正常的光照强度 (100 lux)
                LOGD("Sensor Data (Type 5): modified to 100.0 (preventing crash)");
                break;
            case 8:  // SENSOR_TYPE_PROXIMITY (距离传感器)
                *(float *)((char*)dst + 16) = 5.0f; // 设置为“远”状态 (5.0 cm)
                LOGD("Sensor Data (Type 8): modified to 5.0");
                break;
            case 1:  // SENSOR_TYPE_ACCELEROMETER
            case 4:  // SENSOR_TYPE_GYROSCOPE
            case 9:  // SENSOR_TYPE_GRAVITY
            case 10: // SENSOR_TYPE_LINEAR_ACCELERATION
                *(float *)((char*)dst + 16) = 0.0f;
                *(float *)((char*)dst + 20) = 0.0f;
                *(float *)((char*)dst + 24) = 0.0f;
                LOGD("Sensor Data (Type %d): XYZ axes zeroed", type);
                break;
            default:
                // 其他传感器不建议盲目设为 -1.0，尤其是某些系统服务强依赖的传感器
                // 如果需要屏蔽，可以设为 0 或保持原样
                break;
        }
    }
}

void doSensorHook() {
    const char* sfPath = LIBSF_PATH_64;
    if (access(sfPath, F_OK) != 0) {
        sfPath = LIBSF_PATH_32;
    }

    const char* sPath = LIBS_PATH_64;
    if (access(sPath, F_OK) != 0) {
        sPath = LIBS_PATH_32;
    }

    // 1. 从 libsensor.so 加载 SensorEventQueue::write
    if (access(sPath, F_OK) == 0) {
        SandHook::ElfImg sensorLib(sPath);
        if (sensorLib.isValid()) {
            auto sensorWrite = sensorLib.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventm");
            if (sensorWrite == nullptr) {
                sensorWrite = sensorLib.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventj");
            }
            if (sensorWrite == nullptr) {
                sensorWrite = sensorLib.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventy");
            }
            // 增加用户提供的特定符号变体 (无下划线版本和 RKNS2sp 版本)
            if (sensorWrite == nullptr) {
                sensorWrite = sensorLib.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS2spINS_7BitTubeEEEPK12ASensorEventm");
            }
            if (sensorWrite == nullptr) {
                sensorWrite = sensorLib.getSymbolAddress<void*>("ZN7android16SensorEventQueue5writeERKNS2spINS_7BitTubeEEEPK12ASensorEventm");
            }
            if (sensorWrite == nullptr) {
                sensorWrite = sensorLib.getSymbolAddressByPrefix<void*>("_ZN7android16SensorEventQueue5write");
            }

            LOGD("Dobby SensorEventQueue::write found at %p in %s", sensorWrite, sPath);
            if (sensorWrite != nullptr) {
                OriginalSensorEventQueueWrite = (OriginalSensorEventQueueWriteType)InlineHook(sensorWrite, (void *)SensorEventQueueWrite);
            }
        } else {
            LOGD("failed to load %s via ElfImg", sPath);
        }
    } else {
        LOGD("libsensor.so not found, skipping SensorEventQueue::write hook");
    }

    // 2. 从 libsensorservice.so 加载 convertToSensorEvent
    if (access(sfPath, F_OK) == 0) {
        SandHook::ElfImg sensorService(sfPath);
        if (sensorService.isValid()) {
            auto convertToSensorEvent = sensorService.getSymbolAddress<void*>("_ZN7android8hardware7sensors4V1_014implementation20convertToSensorEventERKNS2_5EventEP15sensors_event_t");
            if (convertToSensorEvent == nullptr) {
                convertToSensorEvent = sensorService.getSymbolAddressByPrefix<void*>("_ZN7android8hardware7sensors4V1_014implementation20convertToSensorEvent");
            }

            LOGD("Dobby convertToSensorEvent found at %p in %s", convertToSensorEvent, sfPath);
            if (convertToSensorEvent != nullptr) {
                OriginalConvertToSensorEvent = (OriginalConvertToSensorEventType)InlineHook(convertToSensorEvent, (void *)ConvertToSensorEvent);
            }
        } else {
            LOGD("failed to load %s via ElfImg", sfPath);
        }
    } else {
        LOGD("libsensorservice.so not found, skipping convertToSensorEvent hook");
    }
}
