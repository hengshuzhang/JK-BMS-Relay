/*
 * ESP32-S3 relay with two Legacy advertising sets.
 * Adapted from the Arduino NimBLE relay design in:
 * https://github.com/wenwumumu/JK-BMS-4G
 * NimBLE's multi-instance API is used internally, but no non-Legacy Extended
 * advertising payload is configured. Original project license: GPL-2.0.
 */
#include "BmsNimbleRelay.h"
#include "AppConfig.h"
#include <algorithm>
#include <string>

JkBmsNimbleRelay* JkBmsNimbleRelay::instance_ = nullptr;

namespace {
inline bool notifyMtuReady(uint16_t mtu) {
    return mtu == AppConfig::kNotifyMtuPrimary || mtu == AppConfig::kNotifyMtuSecondary;
}

bool isAtSequence(const std::vector<uint8_t>& data, size_t offset) {
    return offset + 4 <= data.size() && data[offset] == 0x41 &&
           data[offset + 1] == 0x54 && data[offset + 2] == 0x0D &&
           data[offset + 3] == 0x0A;
}
bool containsJkHeader(const std::vector<uint8_t>& data, uint8_t& previousPrefix) {
    const uint8_t header[] = {0x55, 0xAA, 0xEB, 0x90};
    const uint8_t prefixBefore = previousPrefix;
    previousPrefix = 0;
    if (prefixBefore > 0) {
        const size_t required = 4 - prefixBefore;
        const size_t available = std::min(required, data.size());
        bool matches = true;
        for (size_t i = 0; i < available; ++i) {
            if (data[i] != header[prefixBefore + i]) { matches = false; break; }
        }
        if (matches) {
            if (available == required) return true;
            previousPrefix = static_cast<uint8_t>(prefixBefore + available);
            return false;
        }
    }
    for (size_t i = 0; i + 4 <= data.size(); ++i) {
        if (data[i] == header[0] && data[i + 1] == header[1] &&
            data[i + 2] == header[2] && data[i + 3] == header[3]) return true;
    }
    for (uint8_t length = 3; length > 0; --length) {
        if (data.size() < length) continue;
        bool matches = true;
        for (uint8_t i = 0; i < length; ++i) {
            if (data[data.size() - length + i] != header[i]) { matches = false; break; }
        }
        if (matches) { previousPrefix = length; break; }
    }
    return false;
}
void removeAtSequences(std::vector<uint8_t>& data) {
    size_t read = 0;
    size_t write = 0;
    while (read < data.size()) {
        if (isAtSequence(data, read)) {
            read += 4;
            continue;
        }
        data[write++] = data[read++];
    }
    data.resize(write);
}
}

class RelayAdvertisingCallbacks final : public NimBLEExtAdvertisingCallbacks {
    void onStopped(NimBLEExtAdvertising*, int reason, uint8_t instId) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay) return;
        // Reason 0 means this connectable advertising instance ended because
        // a connection was established. Wait for onConnect() to record the
        // occupied instance before scheduling any other advertisement.
        if (reason == 0) {
            relay->pendingAdvertisingInstance_ = instId;
            APP_LOGF("[Relay] Advertising instance %u stopped by connection\n",
                     static_cast<unsigned>(instId));
            return;
        }
        // For other stop reasons only queue recovery; starting an
        // advertisement from the GAP callback can race with the Central
        // connection/service-discovery state transition.
        relay->scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartIntervalMs);
    }
};

class RelayServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay) return;
        const uint16_t handle = info.getConnHandle();
        const uint8_t advertisingInstance = relay->pendingAdvertisingInstance_;
        relay->pendingAdvertisingInstance_ = 0xFF;
        if (relay->centralPaused_) {
            // A connection can race with BMS loss after its advertising set
            // was stopped. Let relay.loop() terminate it outside this GAP
            // callback; it is not a valid relay client while BMS is absent.
            relay->pendingDisconnectHandles_.push_back(handle);
            return;
        }
        if (std::find(relay->phoneHandles_.begin(), relay->phoneHandles_.end(), handle) ==
            relay->phoneHandles_.end()) {
            if (relay->phoneHandles_.size() >= AppConfig::kMaxPhoneConnections) {
                APP_LOGF("[Relay] Rejecting phone connection: limit=%u\n",
                              static_cast<unsigned>(AppConfig::kMaxPhoneConnections));
                // Do not issue GAP stop/disconnect commands from this GAP
                // callback. relay.loop() owns queued disconnections and will
                // also stop advertising once the connection limit is reached.
                relay->queueDisconnect(handle);
                return;
            }
            relay->phoneHandles_.push_back(handle);
            relay->phoneMtus_.push_back(info.getMTU());
            relay->phoneAdvertisingInstances_.push_back(advertisingInstance);
            if (advertisingInstance < relay->signalCount_) {
                // NimBLE normally stopped this connectable instance before
                // onConnect(). Confirm it from relay.loop() so this signal
                // cannot continue to answer scanner requests, without
                // touching the established GATT connection or other signals.
                relay->advertisementStopPending_[advertisingInstance] = true;
            }
        }
        relay->scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartIntervalMs);
        APP_LOGF("[Relay] Phone connected: %s, count=%u, advertising_instance=%u\n",
                      info.getAddress().toString().c_str(),
                      static_cast<unsigned>(relay->phoneHandles_.size()),
                      static_cast<unsigned>(advertisingInstance));
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay) return;
        const auto handleIt = std::find(relay->phoneHandles_.begin(), relay->phoneHandles_.end(), info.getConnHandle());
        if (handleIt == relay->phoneHandles_.end()) return;
        const size_t index = static_cast<size_t>(handleIt - relay->phoneHandles_.begin());
        if (index < relay->phoneMtus_.size()) {
            relay->phoneMtus_[index] = mtu;
            APP_LOGF("[Relay] Phone MTU changed, handle=%u mtu=%u ready=%s\\n",
                     info.getConnHandle(), mtu,
                     relay->isNotifyMtuReady(info.getConnHandle()) ? "yes" : "no");
        }
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay) return;
        const uint16_t handle = info.getConnHandle();
        const auto handleIt = std::find(relay->phoneHandles_.begin(), relay->phoneHandles_.end(), handle);
        if (handleIt != relay->phoneHandles_.end()) {
            const size_t index = static_cast<size_t>(handleIt - relay->phoneHandles_.begin());
            if (index < relay->phoneAdvertisingInstances_.size()) {
                relay->phoneAdvertisingInstances_.erase(
                    relay->phoneAdvertisingInstances_.begin() + index);
            }
            if (index < relay->phoneMtus_.size()) {
                relay->phoneMtus_.erase(relay->phoneMtus_.begin() + index);
            }
            relay->phoneHandles_.erase(handleIt);
        }
        const bool hadSubscribers = !relay->subscribedHandles_.empty();
        const auto subscriberIt = std::find(relay->subscribedHandles_.begin(),
                                            relay->subscribedHandles_.end(), handle);
        if (subscriberIt != relay->subscribedHandles_.end()) {
            const size_t subscriberIndex =
                static_cast<size_t>(subscriberIt - relay->subscribedHandles_.begin());
            relay->subscribedHandles_.erase(subscriberIt);
            if (subscriberIndex < relay->subscriberNotifyFailureCounts_.size()) {
                relay->subscriberNotifyFailureCounts_.erase(
                    relay->subscriberNotifyFailureCounts_.begin() + subscriberIndex);
            }
        }
        relay->pendingInitialNotifyHandles_.erase(
            std::remove(relay->pendingInitialNotifyHandles_.begin(),
                        relay->pendingInitialNotifyHandles_.end(), handle),
            relay->pendingInitialNotifyHandles_.end());
        if (hadSubscribers && relay->subscribedHandles_.empty() && relay->subscribeCallback_) {
            relay->subscribeCallback_(false);
        }
        if (!relay->centralPaused_) {
            relay->scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartIntervalMs);
        }
        APP_LOGF("[Relay] Phone disconnected, handle=%u count=%u reason=%d\n",
                      handle, static_cast<unsigned>(relay->phoneHandles_.size()), reason);
    }
};

class RelayCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay || !relay->writeCallback_) return;
        std::string value = characteristic->getValue();
        if (value.empty()) return;

        const uint16_t handle = info.getConnHandle();
        const bool forwarded = relay->writeCallback_(
            handle, reinterpret_cast<const uint8_t*>(value.data()), value.size());
        if (forwarded) {
            ++relay->downstreamWriteSuccessCount_;
        } else {
            ++relay->downstreamWriteFailureCount_;
        }
        APP_LOGF("[Relay] Phone write %s, handle=%u len=%u\n",
                      forwarded ? "forwarded" : "rejected", handle,
                      static_cast<unsigned>(value.size()));
    }

    void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay || characteristic != relay->characteristic_) return;
        // Pull-mode clients do not subscribe to Notify. They can write a raw
        // BMS request, wait for the response to be cached by notifyBmsData(),
        // then read FFE1. The cache is shared by all phone connections and is
        // independent of which client most recently wrote or subscribed.
        if (relay->latestData_.empty()) {
            const uint8_t empty = 0;
            characteristic->setValue(&empty, 0);
            APP_LOGF("[Relay] Phone read: no BMS frame available, handle=%u\\n",
                          info.getConnHandle());
            return;
        }
        characteristic->setValue(relay->latestData_.data(), relay->latestData_.size());
        APP_LOGF("[Relay] Phone read: filtered BMS frame, handle=%u len=%u\\n",
                      info.getConnHandle(), static_cast<unsigned>(relay->latestData_.size()));
    }

    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) override {
        auto* relay = JkBmsNimbleRelay::instance_;
        if (!relay || characteristic != relay->characteristic_) return;
        const uint16_t handle = info.getConnHandle();
        const bool wasSubscribed = std::find(relay->subscribedHandles_.begin(),
                                             relay->subscribedHandles_.end(), handle) !=
                                    relay->subscribedHandles_.end();
        const bool enabled = (value & 0x0001U) != 0;
        if (enabled) {
            if (!wasSubscribed) {
                relay->subscribedHandles_.push_back(handle);
                relay->subscriberNotifyFailureCounts_.push_back(0);
                if (relay->subscribeCallback_) relay->subscribeCallback_(true);
                if (std::find(relay->pendingInitialNotifyHandles_.begin(),
                              relay->pendingInitialNotifyHandles_.end(), handle) ==
                    relay->pendingInitialNotifyHandles_.end()) {
                    relay->pendingInitialNotifyHandles_.push_back(handle);
                }
                relay->initialNotifyNotBeforeAt_ = millis() + AppConfig::kDownstreamInitialNotifyDelayMs;
            }
            APP_LOGF("[Relay] FFE1 CCCD enabled, handle=%u subscribers=%u\n",
                          handle, static_cast<unsigned>(relay->subscribedHandles_.size()));
        } else {
            const bool hadSubscribers = !relay->subscribedHandles_.empty();
            const auto subscriberIt = std::find(relay->subscribedHandles_.begin(),
                                                relay->subscribedHandles_.end(), handle);
            if (subscriberIt != relay->subscribedHandles_.end()) {
                const size_t subscriberIndex =
                    static_cast<size_t>(subscriberIt - relay->subscribedHandles_.begin());
                relay->subscribedHandles_.erase(subscriberIt);
                if (subscriberIndex < relay->subscriberNotifyFailureCounts_.size()) {
                    relay->subscriberNotifyFailureCounts_.erase(
                        relay->subscriberNotifyFailureCounts_.begin() + subscriberIndex);
                }
            }
            relay->pendingInitialNotifyHandles_.erase(
                std::remove(relay->pendingInitialNotifyHandles_.begin(),
                            relay->pendingInitialNotifyHandles_.end(), handle),
                relay->pendingInitialNotifyHandles_.end());
            if (hadSubscribers && relay->subscribedHandles_.empty() && relay->subscribeCallback_) {
                relay->subscribeCallback_(false);
            }
            APP_LOGF("[Relay] FFE1 CCCD disabled, handle=%u subscribers=%u\n",
                          handle, static_cast<unsigned>(relay->subscribedHandles_.size()));
        }
    }
};

JkBmsNimbleRelay::JkBmsNimbleRelay() { instance_ = this; }
bool JkBmsNimbleRelay::isNotifyMtuReady(uint16_t handle) const {
    const auto handleIt = std::find(phoneHandles_.begin(), phoneHandles_.end(), handle);
    if (handleIt == phoneHandles_.end()) return false;
    const size_t index = static_cast<size_t>(handleIt - phoneHandles_.begin());
    return index < phoneMtus_.size() && notifyMtuReady(phoneMtus_[index]);
}

void JkBmsNimbleRelay::init() {
    if (server_) return;
    server_ = NimBLEDevice::createServer();
    if (!server_) return;
    server_->setCallbacks(new RelayServerCallbacks());
    service_ = server_->createService(AppConfig::kBmsServiceUuid);
    if (!service_) return;
    characteristic_ = service_->createCharacteristic(
        AppConfig::kBmsCharacteristicUuid,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    if (!characteristic_) return;
    characteristic_->setCallbacks(new RelayCharacteristicCallbacks());
    // NimBLE creates and manages the CCCD automatically for NOTIFY.
    // A manually-created 0x2902 descriptor is hidden and has no functionality.
    service_->start();
    advertising_ = NimBLEDevice::getAdvertising();
    if (advertising_) {
        advertising_->setCallbacks(new RelayAdvertisingCallbacks());
    }
}

bool JkBmsNimbleRelay::configureAdvertisementInstance(uint8_t instance) {
    if (!advertising_ || instance >= AppConfig::kMaxVirtualSignals) return false;

    static const char* const kAddresses[AppConfig::kMaxVirtualSignals] = {
        "D2:3C:50:11:C1:01",
        "D2:3C:50:11:C2:02",
    };
    const uint8_t manufacturer[12] = {0x65, 0x0B, 0x88, 0xA0, 0xC8, 0x47,
                                      0x80, 0x37, 0x79, 0x38, 0x00, 0x00};
    const uint8_t serviceData[2] = {0x01, 0x00};
    const std::vector<NimBLEUUID> advertisedServices = {
        NimBLEUUID(AppConfig::kBmsServiceUuid)};

    NimBLEExtAdvertisement advertisement;
    advertisement.setLegacyAdvertising(true);
    advertisement.setFlags(0x06);
    advertisement.setConnectable(true);
    advertisement.setScannable(true);
    advertisement.setAddress(NimBLEAddress(kAddresses[instance], BLE_ADDR_RANDOM));
    advertisement.setName(std::string(signalNames_[instance].c_str()));
    advertisement.setManufacturerData(manufacturer, sizeof(manufacturer));
    if (!advertising_->setInstanceData(instance, advertisement)) {
        APP_LOGF("[Relay] signal %u legacy advertisement setup failed\n",
                      static_cast<unsigned>(instance));
        return false;
    }

    NimBLEExtAdvertisement scanResponse;
    scanResponse.setCompleteServices16(advertisedServices);
    scanResponse.setServiceData(
        NimBLEUUID(AppConfig::kBmsServiceUuid), serviceData, sizeof(serviceData));
    if (!advertising_->setScanResponseData(instance, scanResponse)) {
        APP_LOGF("[Relay] signal %u legacy Scan Response setup failed\n",
                      static_cast<unsigned>(instance));
        return false;
    }

    APP_LOGF("[Relay] signal %u configured: name=%s address=%s\n",
                  static_cast<unsigned>(instance), signalNames_[instance].c_str(),
                  kAddresses[instance]);
    return true;
}

void JkBmsNimbleRelay::configureAdvertisement() {
    if (!advertising_) return;
    for (uint8_t instance = 0; instance < signalCount_; ++instance) {
        configureAdvertisementInstance(instance);
    }
}

void JkBmsNimbleRelay::stopAdvertisements() {
    if (!advertising_) return;
    for (uint8_t instance = 0; instance < signalCount_; ++instance) {
        advertising_->stop(instance);
    }
}

void JkBmsNimbleRelay::scheduleAdvertisementStart(uint32_t delayMs) {
    if (centralPaused_ || !running_ || !advertising_) return;
    advertisementStartPending_ = true;
    advertisementStartNotBeforeAt_ = millis() + delayMs;
}

bool JkBmsNimbleRelay::advertising() const {
    if (signalCount_ == 0) return false;
    for (uint8_t instance = 0; instance < signalCount_; ++instance) {
        if (!signalAdvertising(instance)) return false;
    }
    return true;
}

bool JkBmsNimbleRelay::configureSignals(uint8_t count, const String* names) {
    if (!names || count < AppConfig::kMinVirtualSignalCount ||
        count > AppConfig::kMaxVirtualSignals) return false;
    for (uint8_t i = 0; i < count; ++i) {
        if (names[i].isEmpty() || names[i].length() > AppConfig::kVirtualSignalNameMaxLength) {
            return false;
        }
    }

    const uint8_t oldCount = signalCount_;
    const uint8_t commonCount = oldCount < count ? oldCount : count;
    bool needsConfiguration[AppConfig::kMaxVirtualSignals] = {};
    for (uint8_t i = 0; i < commonCount; ++i) {
        const bool changed = names[i] != signalNames_[i];
        if (advertisingInstanceOccupied(i) && changed) {
            APP_LOGF("[Relay] Cannot rename connected signal instance %u\n",
                          static_cast<unsigned>(i));
            return false;
        }
        needsConfiguration[i] = changed && !advertisingInstanceOccupied(i);
    }
    for (uint8_t i = commonCount; i < count; ++i) {
        needsConfiguration[i] = true;
    }
    for (uint8_t i = count; i < oldCount; ++i) {
        if (advertisingInstanceOccupied(i)) {
            APP_LOGF("[Relay] Cannot remove connected signal instance %u\n",
                          static_cast<unsigned>(i));
            return false;
        }
        if (advertising_ && advertising_->isActive(i) && !advertising_->stop(i)) return false;
    }

    if (advertising_) {
        for (uint8_t i = 0; i < commonCount; ++i) {
            if (needsConfiguration[i] && advertising_->isActive(i) && !advertising_->stop(i)) {
                return false;
            }
        }
    }

    for (uint8_t i = 0; i < AppConfig::kMaxVirtualSignals; ++i) {
        signalNames_[i] = i < count ? names[i] : AppConfig::defaultVirtualSignalName(i);
    }
    signalCount_ = count;

    if (!advertising_) return true;
    for (uint8_t i = 0; i < count; ++i) {
        if (!needsConfiguration[i]) continue;
        if (!configureAdvertisementInstance(i)) return false;
    }
    // Never restart advertisements from a Web request. The main relay loop
    // owns GAP start operations and starts at most one instance per interval.
    scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartIntervalMs);
    return true;
}


bool JkBmsNimbleRelay::begin() {
    if (!server_) init();
    if (!server_ || !advertising_) return false;
    if (signalNames_[0].isEmpty()) {
        for (uint8_t i = 0; i < AppConfig::kMaxVirtualSignals; ++i) {
            signalNames_[i] = AppConfig::defaultVirtualSignalName(i);
        }
    }
    phoneHandles_.clear();
    phoneMtus_.clear();
    phoneAdvertisingInstances_.clear();
    pendingAdvertisingInstance_ = 0xFF;
    std::fill(advertisementStopPending_,
              advertisementStopPending_ + AppConfig::kMaxVirtualSignals, false);
    subscribedHandles_.clear();
    subscriberNotifyFailureCounts_.clear();
    pendingDisconnectHandles_.clear();
    pendingInitialNotifyHandles_.clear();
    initialNotifyNotBeforeAt_ = 0;
    jkHeaderPrefix_ = 0;
    latestDataHasJkHeader_ = false;
    latestData_.clear();
    downstreamWriteSuccessCount_ = 0;
    downstreamWriteFailureCount_ = 0;
    downstreamNotifySuccessCount_ = 0;
    downstreamNotifyFailureCount_ = 0;
    downstreamForcedDisconnectCount_ = 0;
    cachedBmsFrameCount_ = 0;
    lastDownstreamNotifyAt_ = 0;
    configureAdvertisement();
    running_ = true;
    // Startup normally begins BMS-gated. If a caller initializes the relay
    // after BMS is already available, still use the same deferred scheduler.
    scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartDelayMs);
    APP_LOGF("[Relay] started: signal_count=%u service=%s characteristic=%s bms_available=%s\n",
                  static_cast<unsigned>(signalCount_),
                  AppConfig::kBmsServiceUuid, AppConfig::kBmsCharacteristicUuid,
                  centralPaused_ ? "no" : "yes");
    return true;
}

void JkBmsNimbleRelay::sendPendingInitialNotifications() {
    if (pendingInitialNotifyHandles_.empty() || latestData_.empty() || !characteristic_) return;
    if (static_cast<int32_t>(millis() - initialNotifyNotBeforeAt_) < 0) return;
    if (!latestDataHasJkHeader_) return;
    std::vector<uint16_t> remaining;
    for (const uint16_t handle : pendingInitialNotifyHandles_) {
        if (std::find(subscribedHandles_.begin(), subscribedHandles_.end(), handle) == subscribedHandles_.end()) continue;
        if (!isNotifyMtuReady(handle)) { remaining.push_back(handle); continue; }
        const bool sent = characteristic_->notify(latestData_.data(), latestData_.size(), handle);
        if (sent) {
            ++downstreamNotifySuccessCount_;
            lastDownstreamNotifyAt_ = millis();
        } else {
            ++downstreamNotifyFailureCount_;
        }
        APP_LOGF("[Relay] Initial BMS frame sent to handle=%u result=%s\\n",
                 handle, sent ? "ok" : "failed");
    }
    pendingInitialNotifyHandles_.swap(remaining);
}

void JkBmsNimbleRelay::loop() {
    while (!pendingDisconnectHandles_.empty()) {
        const uint16_t handle = pendingDisconnectHandles_.back();
        pendingDisconnectHandles_.pop_back();
        if (server_) server_->disconnect(handle);
    }

    // A connected signal must not continue advertising or answering scan
    // requests. This is deliberately deferred from onConnect() to avoid GAP
    // re-entry; stopping an advertising instance does not disconnect its GATT
    // client. Other virtual signal instances remain untouched.
    if (running_ && advertising_) {
        for (uint8_t instance = 0; instance < signalCount_; ++instance) {
            if (!advertisementStopPending_[instance]) continue;
            advertisementStopPending_[instance] = false;
            // NimBLE treats an already-stopped instance as success and clears
            // its cached active state, so this is safe after a connection.
            advertising_->stop(instance);
        }
    }

    if (!running_ || !advertising_ || centralPaused_ || !advertisementStartPending_) return;

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - advertisementStartNotBeforeAt_) < 0 ||
        now - lastAdvertisementMaintenanceAt_ < AppConfig::kVirtualAdvertisementStartIntervalMs) {
        return;
    }

    if (phoneHandles_.size() >= AppConfig::kMaxPhoneConnections) {
        stopAdvertisements();
        advertisementStartPending_ = false;
        APP_LOGLN("[Relay] Phone connection limit reached; virtual signal advertising stopped");
        return;
    }

    bool occupied[AppConfig::kMaxVirtualSignals] = {};
    if (phoneAdvertisingInstances_.size() != phoneHandles_.size()) {
        // Do not restart an unknown instance while a phone is connected; this
        // avoids restarting the advertising set that owns the live link.
        if (!phoneHandles_.empty()) {
            advertisementStartNotBeforeAt_ = now + AppConfig::kVirtualAdvertisementStartIntervalMs;
            APP_LOGLN("[Relay] Advertising owner unknown; delaying repair");
            return;
        }
    } else {
        bool unknownOwner = false;
        for (const uint8_t instance : phoneAdvertisingInstances_) {
            if (instance < signalCount_) {
                occupied[instance] = true;
            } else {
                unknownOwner = true;
            }
        }
        if (unknownOwner && !phoneHandles_.empty()) {
            advertisementStartNotBeforeAt_ = now + AppConfig::kVirtualAdvertisementStartIntervalMs;
            APP_LOGLN("[Relay] Advertising owner unknown; delaying repair");
            return;
        }
    }

    // Submit exactly one GAP advertising start per loop maintenance interval.
    // This avoids a back-to-back start(0)/start(1) sequence immediately after
    // the real BMS completed its Central connection and service discovery.
    for (uint8_t instance = 0; instance < signalCount_; ++instance) {
        if (occupied[instance] || advertising_->isActive(instance)) continue;
        lastAdvertisementMaintenanceAt_ = now;
        const bool started = advertising_->start(instance);
        advertisementStartNotBeforeAt_ = now + AppConfig::kVirtualAdvertisementStartIntervalMs;
        APP_LOGF("[Relay] Scheduled advertisement instance %u: %s\n",
                 static_cast<unsigned>(instance), started ? "started" : "failed");
        return;
    }

    advertisementStartPending_ = false;
}

void JkBmsNimbleRelay::setBmsConnectionAvailable(bool available) {
    if (!available) {
        if (centralPaused_) return;

        // Stop both advertising sets immediately. Existing downstream links
        // are deliberately terminated below rather than being allowed to use
        // a relay whose real BMS is unavailable.
        centralPaused_ = true;
        advertisementStartPending_ = false;
        advertisementStartNotBeforeAt_ = 0;
        stopAdvertisements();
        pendingInitialNotifyHandles_.clear();
        initialNotifyNotBeforeAt_ = 0;
    jkHeaderPrefix_ = 0;
    latestDataHasJkHeader_ = false;

        const bool hadSubscribers = !subscribedHandles_.empty();
        subscribedHandles_.clear();
        subscriberNotifyFailureCounts_.clear();
        if (hadSubscribers && subscribeCallback_) {
            subscribeCallback_(false);
        }

        for (const uint16_t handle : phoneHandles_) {
            if (std::find(pendingDisconnectHandles_.begin(), pendingDisconnectHandles_.end(), handle) ==
                pendingDisconnectHandles_.end()) {
                pendingDisconnectHandles_.push_back(handle);
            }
        }
        APP_LOGF("[Relay] BMS unavailable: advertisements stopped, queued %u phone disconnect(s)\n",
                 static_cast<unsigned>(phoneHandles_.size()));
        return;
    }

    if (!centralPaused_) return;
    centralPaused_ = false;
    // BmsCentral just completed synchronous service discovery. Do not issue
    // any advertising GAP command in this state-transition call; relay.loop
    // waits for the host to settle, then starts one instance at a time.
    scheduleAdvertisementStart(AppConfig::kVirtualAdvertisementStartDelayMs);
    APP_LOGLN("[Relay] BMS available: virtual advertisement recovery queued");
}

void JkBmsNimbleRelay::stop() {
    stopAdvertisements();
    running_ = false;
    phoneHandles_.clear();
    phoneMtus_.clear();
    phoneAdvertisingInstances_.clear();
    pendingAdvertisingInstance_ = 0xFF;
    std::fill(advertisementStopPending_,
              advertisementStopPending_ + AppConfig::kMaxVirtualSignals, false);
    subscribedHandles_.clear();
    subscriberNotifyFailureCounts_.clear();
    pendingDisconnectHandles_.clear();
    pendingInitialNotifyHandles_.clear();
    initialNotifyNotBeforeAt_ = 0;
    jkHeaderPrefix_ = 0;
    latestDataHasJkHeader_ = false;
    latestData_.clear();
    advertisementStartPending_ = false;
    advertisementStartNotBeforeAt_ = 0;
    centralPaused_ = true;
}

void JkBmsNimbleRelay::notifyBmsData(const uint8_t* data, size_t length) {
    if (!characteristic_ || !data || length == 0 || length > AppConfig::kMaxRawFrame) return;
    latestData_.assign(data, data + length);
    const size_t originalLength = latestData_.size();
    removeAtSequences(latestData_);
    if (latestData_.empty()) {
        APP_LOGF("[Relay] suppressed payload containing only AT\\r\\n, original_len=%u\\n",
                 static_cast<unsigned>(originalLength));
        return;
    }
    const size_t cleanLength = latestData_.size();
    const bool hasJkHeader = containsJkHeader(latestData_, jkHeaderPrefix_);
    latestDataHasJkHeader_ = hasJkHeader;
    ++cachedBmsFrameCount_;
    characteristic_->setValue(latestData_.data(), cleanLength);
    if (cleanLength != originalLength) {
        APP_LOGF("[Relay] removed %u AT\\r\\n sequence bytes, clean_len=%u\\n",
                 static_cast<unsigned>(originalLength - cleanLength),
                 static_cast<unsigned>(cleanLength));
    }

    if (subscribedHandles_.empty()) {
        APP_LOGLN("[Relay] BMS response cached for GATT read; no phone subscriber");
        return;
    }

    if (subscriberNotifyFailureCounts_.size() != subscribedHandles_.size()) {
        subscriberNotifyFailureCounts_.assign(subscribedHandles_.size(), 0);
        APP_LOGLN("[Relay] repaired downstream Notify failure tracking state");
    }

    // The BMS stream is shared by all relay clients. Notify every subscribed
    // connection instead of routing the frame to one global response owner.
    uint16_t disconnectHandles[AppConfig::kMaxPhoneConnections] = {};
    uint8_t disconnectCount = 0;
    for (size_t index = 0; index < subscribedHandles_.size(); ++index) {
        const uint16_t handle = subscribedHandles_[index];
        if (!isNotifyMtuReady(handle)) continue;
        const auto pendingIt = std::find(pendingInitialNotifyHandles_.begin(),
                                         pendingInitialNotifyHandles_.end(), handle);
        if (pendingIt != pendingInitialNotifyHandles_.end() && !hasJkHeader) continue;
        const bool sent = characteristic_->notify(latestData_.data(), cleanLength, handle);
        if (sent) {
            ++downstreamNotifySuccessCount_;
            lastDownstreamNotifyAt_ = millis();
            subscriberNotifyFailureCounts_[index] = 0;
            if (pendingIt != pendingInitialNotifyHandles_.end()) {
                pendingInitialNotifyHandles_.erase(pendingIt);
            }
        } else {
            ++downstreamNotifyFailureCount_;
            uint8_t& failures = subscriberNotifyFailureCounts_[index];
            if (failures < 0xFFU) ++failures;
            if (failures >= AppConfig::kDownstreamNotifyFailureLimit &&
                disconnectCount < AppConfig::kMaxPhoneConnections) {
                disconnectHandles[disconnectCount++] = handle;
            }
        }
        APP_LOGF("[Relay] BMS Notify routed to handle=%u len=%u result=%s\n",
                      handle, static_cast<unsigned>(cleanLength), sent ? "ok" : "failed");
    }

    if (disconnectCount == 0) return;

    const bool hadSubscribers = !subscribedHandles_.empty();
    for (uint8_t disconnectIndex = 0; disconnectIndex < disconnectCount; ++disconnectIndex) {
        const uint16_t handle = disconnectHandles[disconnectIndex];
        const auto subscriberIt = std::find(subscribedHandles_.begin(),
                                            subscribedHandles_.end(), handle);
        if (subscriberIt == subscribedHandles_.end()) continue;
        const size_t subscriberIndex =
            static_cast<size_t>(subscriberIt - subscribedHandles_.begin());
        subscribedHandles_.erase(subscriberIt);
        subscriberNotifyFailureCounts_.erase(
            subscriberNotifyFailureCounts_.begin() + subscriberIndex);
        ++downstreamForcedDisconnectCount_;
        APP_LOGF("[Relay] Disconnecting stale phone handle=%u after %u Notify failures\n",
                      handle, static_cast<unsigned>(AppConfig::kDownstreamNotifyFailureLimit));
        // Defer GAP termination to loop(), outside the NimBLE notification
        // callback that detected the stale connection.
        pendingDisconnectHandles_.push_back(handle);
    }
    if (hadSubscribers && subscribedHandles_.empty() && subscribeCallback_) {
        subscribeCallback_(false);
    }
}
