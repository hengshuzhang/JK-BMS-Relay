#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEExtAdvertising.h>
#include <algorithm>
#include <functional>
#include <vector>

#include "AppConfig.h"

class JkBmsNimbleRelay {
public:
    using WriteCallback = std::function<bool(uint16_t, const uint8_t*, size_t)>;
    using SubscribeCallback = std::function<bool(bool)>;

    JkBmsNimbleRelay();
    bool configureSignals(uint8_t count, const String* names);
    uint8_t signalCount() const { return signalCount_; }
    const String& signalName(uint8_t index) const {
        return signalNames_[index < AppConfig::kMaxVirtualSignals ? index : 0];
    }
    bool signalAvailable(uint8_t index) const {
        return index < signalCount_ && service_ != nullptr && characteristic_ != nullptr;
    }
    bool signalAdvertising(uint8_t index) const {
        return signalAvailable(index) && advertising_ != nullptr &&
               !advertisingInstanceOccupied(index) && advertising_->isActive(index);
    }
    bool signalConnected(uint8_t index) const {
        return index < signalCount_ && advertisingInstanceOccupied(index);
    }

    void init();
    bool begin();
    void loop();
    void stop();
    void setBmsConnectionAvailable(bool available);
    void sendPendingInitialNotifications();
    bool bmsConnectionAvailable() const { return !centralPaused_; }
    bool advertisementStartPending() const { return advertisementStartPending_; }
    void notifyBmsData(const uint8_t* data, size_t length);
    void setWriteCallback(WriteCallback callback) { writeCallback_ = callback; }
    void setSubscribeCallback(SubscribeCallback callback) { subscribeCallback_ = callback; }
    bool running() const { return running_; }
    bool phoneConnected() const { return !phoneHandles_.empty(); }
    uint8_t phoneConnectionCount() const { return static_cast<uint8_t>(phoneHandles_.size()); }
    bool notificationsEnabled() const { return !subscribedHandles_.empty(); }
    uint32_t downstreamWriteSuccessCount() const { return downstreamWriteSuccessCount_; }
    uint32_t downstreamWriteFailureCount() const { return downstreamWriteFailureCount_; }
    uint32_t downstreamNotifySuccessCount() const { return downstreamNotifySuccessCount_; }
    uint32_t downstreamNotifyFailureCount() const { return downstreamNotifyFailureCount_; }
    uint32_t downstreamForcedDisconnectCount() const { return downstreamForcedDisconnectCount_; }
    uint32_t cachedBmsFrameCount() const { return cachedBmsFrameCount_; }
    uint16_t latestDataLength() const { return static_cast<uint16_t>(latestData_.size()); }
    uint32_t lastDownstreamNotifyAt() const { return lastDownstreamNotifyAt_; }
    bool primarySignalAvailable() const { return signalAvailable(0); }
    bool secondarySignalAvailable() const { return signalAvailable(1); }
    bool primaryAdvertising() const { return signalAdvertising(0); }
    bool secondaryAdvertising() const { return signalAdvertising(1); }
    bool advertising() const;
    const String& name() const { return signalNames_[0]; }
    const String& secondaryName() const { return signalNames_[1]; }

private:
    static JkBmsNimbleRelay* instance_;
    NimBLEServer* server_{nullptr};
    NimBLEService* service_{nullptr};
    NimBLECharacteristic* characteristic_{nullptr};
    NimBLEExtAdvertising* advertising_{nullptr};
    WriteCallback writeCallback_;
    SubscribeCallback subscribeCallback_;
    String signalNames_[AppConfig::kMaxVirtualSignals];
    uint8_t signalCount_{AppConfig::kDefaultVirtualSignalCount};
    std::vector<uint8_t> latestData_;
    std::vector<uint16_t> phoneHandles_;
    std::vector<uint16_t> phoneMtus_;
    std::vector<uint16_t> subscribedHandles_;
    std::vector<uint8_t> subscriberNotifyFailureCounts_;
    std::vector<uint16_t> pendingDisconnectHandles_;
    std::vector<uint8_t> phoneAdvertisingInstances_;
    std::vector<uint16_t> pendingInitialNotifyHandles_;
    uint32_t initialNotifyNotBeforeAt_{0};
    uint8_t jkHeaderPrefix_{0};
    bool advertisementStopPending_[AppConfig::kMaxVirtualSignals] = {};
    bool latestDataHasJkHeader_{false};
    uint8_t pendingAdvertisingInstance_{0xFF};
    uint32_t lastAdvertisementMaintenanceAt_{0};
    uint32_t advertisementStartNotBeforeAt_{0};
    uint32_t downstreamWriteSuccessCount_{0};
    uint32_t downstreamWriteFailureCount_{0};
    uint32_t downstreamNotifySuccessCount_{0};
    uint32_t downstreamNotifyFailureCount_{0};
    uint32_t downstreamForcedDisconnectCount_{0};
    uint32_t cachedBmsFrameCount_{0};
    uint32_t lastDownstreamNotifyAt_{0};
    bool running_{false};
    bool centralPaused_{true};
    bool advertisementStartPending_{false};

    bool advertisingInstanceOccupied(uint8_t instance) const {
        return std::find(phoneAdvertisingInstances_.begin(), phoneAdvertisingInstances_.end(), instance) !=
               phoneAdvertisingInstances_.end();
    }
    bool isNotifyMtuReady(uint16_t handle) const;

    void queueDisconnect(uint16_t handle) {
        if (std::find(pendingDisconnectHandles_.begin(), pendingDisconnectHandles_.end(), handle) ==
            pendingDisconnectHandles_.end()) {
            pendingDisconnectHandles_.push_back(handle);
        }
    }
    void stopAdvertisements();
    void scheduleAdvertisementStart(uint32_t delayMs);
    bool configureAdvertisementInstance(uint8_t instance);
    void configureAdvertisement();
    friend class RelayAdvertisingCallbacks;
    friend class RelayServerCallbacks;
    friend class RelayCharacteristicCallbacks;
};
