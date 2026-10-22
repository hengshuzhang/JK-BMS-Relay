/*
 * ESP32-S3-N16R8 dual-Legacy-advertisement relay.
 * Architecture follows the Arduino relay in JK-BMS-4G; no legacy ESP-IDF
 * implementation from this workspace is used by the PlatformIO build.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>

#include <esp_task_wdt.h>
#include <esp_system.h>
#include "AppConfig.h"
#include "BmsCentral.h"
#include "BmsNimbleRelay.h"
#include "NvsStore.h"
#include "WebManager.h"

namespace {
NvsStore store;
BmsCentral central;
JkBmsNimbleRelay relay;
WebManager web(central, relay, store);

bool forwardPhoneWrite(uint16_t connHandle, const uint8_t* data, size_t length) {
    if (!central.write(data, length)) {
        APP_LOGF("[Relay] phone write rejected, handle=%u len=%u\n",
                      connHandle, static_cast<unsigned>(length));
        return false;
    }
    APP_LOGF("[Relay] phone write forwarded, handle=%u len=%u\n",
                  connHandle, static_cast<unsigned>(length));
    return true;
}

void forwardBmsNotify(const uint8_t* data, size_t length) {
    relay.notifyBmsData(data, length);
}

bool forwardPhoneSubscribe(bool enabled) {
    central.requestUpstreamSubscription(enabled);
    APP_LOGF("[Relay] downstream subscription %s; upstream request queued\n",
                  enabled ? "enabled" : "disabled");
    return true;
}

bool relayBmsConnected = false;

void updateRelayForBmsConnection() {
    const bool bmsConnected = central.connected();
    if (bmsConnected == relayBmsConnected) return;

    relay.setBmsConnectionAvailable(bmsConnected);
    relayBmsConnected = bmsConnected;
    APP_LOGF("[Relay] BMS %s; virtual advertisements %s; phones %s\n",
                  bmsConnected ? "connected" : "disconnected",
                  bmsConnected ? "enabled" : "disabled",
                  bmsConnected ? "may connect" : "forced to disconnect");
}
}

void setup() {
    APP_SERIAL_BEGIN(115200);
    // Give the USB-UART monitor time to attach without blocking forever when
    // the board is deployed without a host computer.
    delay(1500);
    APP_LOGLN("\n[App] ESP32-S3FH4R2 Arduino BMS relay");
    APP_LOGF("[App] reset reason=%d\n", static_cast<int>(esp_reset_reason()));
    APP_LOGLN("[App] Based on JK-BMS-4G Arduino NimBLE relay structure");

    // Keep the loop task protected against a NimBLE GAP/GATT operation that
    // fails to return. The normal synchronous connection path can take up to
    // about 9 seconds, so use a 15-second timeout rather than the default.
    const esp_err_t wdtConfigResult = esp_task_wdt_init(15, true);
    if (wdtConfigResult != ESP_OK) {
        APP_LOGF("[WDT] configuration failed: %d\n",
                      static_cast<int>(wdtConfigResult));
    }
    enableLoopWDT();
    APP_LOGLN("[WDT] loop watchdog enabled, timeout=15 seconds");

    if (!store.begin()) APP_LOGLN("[NVS] Preferences begin failed; defaults will be used");
    PersistentConfig config = store.load();

    web.begin();

    NimBLEDevice::init("JKBMS-Client");
    if (!NimBLEDevice::setMTU(517)) {
        APP_LOGLN("[BLE] Failed to set preferred ATT MTU to 517");
    } else {
        APP_LOGLN("[BLE] Preferred ATT MTU set to 517");
    }
    if (!NimBLEDevice::setPower(9, NimBLETxPowerType::All)) {
        APP_LOGLN("[BLE] Failed to set TX power to 9 dBm");
    } else {
        APP_LOGLN("[BLE] TX power set to 9 dBm (all roles)");
    }
    // Keep bonding and secure connections enabled. NimBLE-Arduino owns the
    // controller privacy/identity store; no Bluedroid API is used here.
    NimBLEDevice::setSecurityAuth(true, false, true);

    relay.configureSignals(config.virtualSignalCount, config.virtualSignalNames);
    relay.setWriteCallback(forwardPhoneWrite);
    relay.setSubscribeCallback(forwardPhoneSubscribe);
    // Do not expose virtual BMS signals until the real BMS completed service
    // discovery and BmsCentral::connected() becomes true.
    relay.setBmsConnectionAvailable(false);
    relay.init();
    if (!relay.begin()) APP_LOGLN("[Relay] initialization failed");

    central.setNotifyCallback(forwardBmsNotify);
    if (!central.begin(config.bmsMac)) {
        APP_LOGF("[App][ERROR] BMS central init failed: %s\n",
                      central.lastError().c_str());
    }
    APP_LOGF("[App] target BMS: %s, transparent client-driven mode\n",
                  config.bmsMac.isEmpty() ? "not configured" : config.bmsMac.c_str());
}

void loop() {
    web.handle();
    updateRelayForBmsConnection();
    central.loop();
    updateRelayForBmsConnection();
    relay.loop();

#if APP_ENABLE_SERIAL_LOGS
    static uint32_t lastLogAt = 0;
    const uint32_t now = millis();
    if (now - lastLogAt >= AppConfig::kHealthLogIntervalMs) {
        lastLogAt = now;
        const bool hasUpstreamData = central.hasUpstreamNotify();
        const uint32_t upstreamDataAge = hasUpstreamData
            ? now - central.lastUpstreamNotifyAt() : 0;
        APP_LOGF(
            "[App] health: BMS=%s relay=%s primary=%s secondary=%s phones=%u "
            "upstream=req:%s pending:%s active:%s frames:%lu age:%s%lu ms "
            "sub=%lu/%lu/%lu notify=%lu/%lu stale=%lu heap=%u/%u\n",
            central.connected() ? "connected" : "disconnected",
            relay.running() ? "running" : "stopped",
            relay.primaryAdvertising() ? "on" : "off",
            relay.secondaryAdvertising() ? "on" : "off",
            static_cast<unsigned>(relay.phoneConnectionCount()),
            central.upstreamSubscriptionRequested() ? "yes" : "no",
            central.upstreamSubscriptionPending() ? "yes" : "no",
            central.upstreamSubscribed() ? "yes" : "no",
            static_cast<unsigned long>(central.upstreamNotifyCount()),
            hasUpstreamData ? "" : "n/a:",
            static_cast<unsigned long>(upstreamDataAge),
            static_cast<unsigned long>(central.upstreamSubscriptionAttemptCount()),
            static_cast<unsigned long>(central.upstreamSubscriptionSuccessCount()),
            static_cast<unsigned long>(central.upstreamSubscriptionFailureCount()),
            static_cast<unsigned long>(relay.downstreamNotifySuccessCount()),
            static_cast<unsigned long>(relay.downstreamNotifyFailureCount()),
            static_cast<unsigned long>(relay.downstreamForcedDisconnectCount()),
            static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMinFreeHeap()));
    }
#endif
    delay(10);
}
