#pragma once

#include <Arduino.h>

#include "DebugLog.h"

namespace AppConfig {
constexpr char kApSsid[] = "CAR";
constexpr char kApPassword[] = "147258369";
constexpr char kRelayDefaultName[] = "JXL50111C1";
constexpr char kRelaySecondaryName[] = "JXL50111C2";
constexpr char kBmsServiceUuid[] = "FFE0";
constexpr char kBmsVendorServiceUuid[] = "E07F";
constexpr char kBmsCharacteristicUuid[] = "FFE1";
constexpr uint8_t kDefaultVirtualSignalCount = 2;
constexpr uint8_t kMinVirtualSignalCount = 2;
constexpr uint8_t kMaxVirtualSignals = 2;
constexpr uint8_t kVirtualSignalNameMaxLength = 12;
constexpr uint8_t kMaxPhoneConnections = 2;
constexpr uint16_t kMaxRawFrame = 1024;
constexpr uint32_t kReconnectDelayMs = 3000;
constexpr uint32_t kUpstreamSubscriptionRetryMs = 5000;
constexpr uint32_t kUpstreamNotifyWatchdogMs = 10000;
constexpr uint32_t kUpstreamResponseWatchdogMs = 2500;
constexpr uint32_t kReplayQueryCaptureWindowMs = 20000;
constexpr uint32_t kDownstreamInitialNotifyDelayMs = 200;
constexpr uint8_t kDownstreamNotifyFailureLimit = 3;
constexpr uint16_t kNotifyMtuPrimary = 517;
constexpr uint16_t kNotifyMtuSecondary = 512;
constexpr uint32_t kHealthLogIntervalMs = 5000;
constexpr uint32_t kVirtualAdvertisementStartDelayMs = 500;
constexpr uint32_t kVirtualAdvertisementStartIntervalMs = 250;

inline const char* defaultVirtualSignalName(uint8_t index) {
    switch (index) {
        case 0: return kRelayDefaultName;
        case 1: return kRelaySecondaryName;
        default: return "BMS-Relay";
    }
}
}

struct PersistentConfig {
    String bmsMac;
    uint8_t virtualSignalCount{AppConfig::kDefaultVirtualSignalCount};
    String virtualSignalNames[AppConfig::kMaxVirtualSignals];
};
