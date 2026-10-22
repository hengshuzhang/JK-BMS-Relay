/*
 * Central flow adapted from the Arduino/NimBLE implementation in:
 * https://github.com/wenwumumu/JK-BMS-4G
 * This file forwards raw payloads from the real BMS to the relay.
 */
#include "BmsCentral.h"
#include "AppConfig.h"
#include <NimBLEUtils.h>
#include <ctype.h>
#include <string.h>
#include <WebServer.h>

namespace {
BmsCentral* currentCentral = nullptr;

bool normalizeMacValue(const String& input, String& canonical) {
    String mac = input;
    mac.trim();
    mac.toLowerCase();

    if (mac.length() == 12) {
        for (uint8_t i = 0; i < mac.length(); ++i) {
            if (!isxdigit(mac[i])) return false;
        }
        canonical = "";
        for (uint8_t i = 0; i < mac.length(); i += 2) {
            if (i != 0) canonical += ':';
            canonical += mac.substring(i, i + 2);
        }
        return true;
    }

    if (mac.length() != 17) return false;
    for (uint8_t i = 0; i < mac.length(); ++i) {
        if ((i % 3) == 2) {
            if (mac[i] != ':') return false;
        } else if (!isxdigit(mac[i])) {
            return false;
        }
    }
    canonical = mac;
    return true;
}

uint8_t hexValue(char value) {
    if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<uint8_t>(value - 'A' + 10);
    return 0xFF;
}

bool parseCanonicalMacBytes(const String& canonical, uint8_t* bytes) {
    if (!bytes || canonical.length() != 17) return false;
    for (uint8_t index = 0; index < 6; ++index) {
        const uint8_t high = hexValue(canonical[index * 3]);
        const uint8_t low = hexValue(canonical[index * 3 + 1]);
        if (high == 0xFF || low == 0xFF) return false;
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void setCentralError(String& target, const String& message) {
    target = message;
    if (!message.isEmpty()) {
        APP_LOGF("[Central][ERROR] %s\n", message.c_str());
    }
}

void rawNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (!currentCentral || !data || length == 0) return;
    currentCentral->onNotify(data, length);
}
}

class CentralClientCallbacks final : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        if (!currentCentral || client != currentCentral->client_) return;

        // The link is established, but the BMS is not usable until service
        // discovery and characteristic validation succeed. Upstream CCCD
        // subscription is controlled by downstream client requests.
        currentCentral->connectAttemptPending_ = false;
        currentCentral->connectRequested_ = false;
        if (!currentCentral->wantConnection_ || currentCentral->connectCancelPending_ ||
            currentCentral->disconnectPending_) {
            currentCentral->disconnectPending_ = true;
            client->disconnect();
            return;
        }
        currentCentral->disconnectPending_ = false;
        currentCentral->connected_ = false;
        currentCentral->state_ = BmsCentral::State::Connecting;
        APP_LOGF("[Central] BMS link established, handle=%u; discovering services\n",
                      client->getConnHandle());
    }

    void onConnectFail(NimBLEClient* client, int reason) override {
        const int lastError = client ? client->getLastError() : 0;
        const char* reasonText = NimBLEUtils::returnCodeToString(reason);
        const char* errorText = NimBLEUtils::returnCodeToString(lastError);
        APP_LOGF("[Central] BMS connect failed, reason=%d (%s) lastError=%d (%s)\n",
                      reason, reasonText, lastError, errorText);
        if (!currentCentral || client != currentCentral->client_) return;

        // This callback is the confirmation that the previous GAP connect
        // procedure has ended. Only now may the main loop scan again.
        currentCentral->connected_ = false;
        currentCentral->connectAttemptPending_ = false;
        currentCentral->connectCancelPending_ = false;
        currentCentral->disconnectPending_ = false;
        currentCentral->connectRequested_ = false;
        currentCentral->characteristic_ = nullptr;
        currentCentral->writeWithResponse_ = false;
        currentCentral->upstreamSubscribed_ = false;
        currentCentral->upstreamSubscriptionChangePending_ =
            currentCentral->upstreamSubscriptionRequested_;
        currentCentral->upstreamSubscriptionRetryPending_ = false;
        currentCentral->peerAddress_ = NimBLEAddress();
        setCentralError(currentCentral->lastError_,
                        String("connect failed (") + lastError + "): " + errorText);
        currentCentral->state_ = currentCentral->wantConnection_
            ? BmsCentral::State::Disconnected : BmsCentral::State::Idle;
        currentCentral->nextScanAt_ = currentCentral->wantConnection_
            ? millis() + AppConfig::kReconnectDelayMs : 0;
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        if (!currentCentral || client != currentCentral->client_) return;
        currentCentral->connected_ = false;
        currentCentral->connectAttemptPending_ = false;
        currentCentral->connectCancelPending_ = false;
        currentCentral->disconnectPending_ = false;
        currentCentral->connectRequested_ = false;
        currentCentral->characteristic_ = nullptr;
        currentCentral->writeWithResponse_ = false;
        currentCentral->upstreamSubscribed_ = false;
        currentCentral->upstreamSubscriptionChangePending_ =
            currentCentral->upstreamSubscriptionRequested_;
        currentCentral->upstreamSubscriptionRetryPending_ = false;
        currentCentral->peerAddress_ = NimBLEAddress();
        currentCentral->state_ = currentCentral->wantConnection_
            ? BmsCentral::State::Disconnected : BmsCentral::State::Idle;
        currentCentral->nextScanAt_ = currentCentral->wantConnection_
            ? millis() + AppConfig::kReconnectDelayMs : 0;
        const char* reasonText = NimBLEUtils::returnCodeToString(reason);
        APP_LOGF("[Central] BMS disconnected, reason=%d (%s)\n", reason, reasonText);
    }
};

class CentralScanCallbacks final : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (!currentCentral || !device || currentCentral->connected_ ||
            currentCentral->connectAttemptPending_ || currentCentral->connectCancelPending_ ||
            currentCentral->disconnectPending_ || currentCentral->connectRequested_) return;
        const NimBLEAddress address = device->getAddress();
        if (!currentCentral->matchesTargetAddress(address)) return;
        if (!device->isConnectable()) return;

        currentCentral->peerAddress_ = address;
        currentCentral->connectRequested_ = true;
        currentCentral->state_ = BmsCentral::State::Connecting;
        const char* advType = NimBLEUtils::advTypeToString(device->getAdvType());
        APP_LOGF("[Central] BMS advertisement: addrType=%u RSSI=%d connectable=%s scannable=%s legacy=%s advType=%s primaryPHY=%u secondaryPHY=%u\n",
                      static_cast<unsigned>(device->getAddressType()),
                      static_cast<int>(device->getRSSI()),
                      device->isConnectable() ? "yes" : "no",
                      device->isScannable() ? "yes" : "no",
                      device->isLegacyAdvertisement() ? "yes" : "no",
                      advType,
                      static_cast<unsigned>(device->getPrimaryPhy()),
                      static_cast<unsigned>(device->getSecondaryPhy()));
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan && scan->stop()) {
            currentCentral->scanStopPending_ = true;
            currentCentral->scanStopRequestedAt_ = millis();
        }
        APP_LOGF("[Central] Found configured BMS: %s\n", currentCentral->targetMac_.c_str());
    }
};

CentralScanCallbacks scanCallbacks;

bool BmsCentral::begin(const String& savedMac) {
    currentCentral = this;
    replayAfterSubscription_ = false;
    replayingQueries_ = false;
    replayQueries_.clear();
    replayCaptureActive_ = false;
    replayCaptureStartedAt_ = 0;
    connected_ = false;
    connectRequested_ = false;
    connectAttemptPending_ = false;
    connectCancelPending_ = false;
    scanStopPending_ = false;
    scanStopRequestedAt_ = 0;
    disconnectPending_ = false;
    peerAddress_ = NimBLEAddress();
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) scan->setScanCallbacks(&scanCallbacks);

    String canonical;
    if (savedMac.isEmpty()) {
        targetMac_ = "";
        state_ = State::Idle;
        return true;
    }
    if (!normalizeMacValue(savedMac, canonical)) {
        targetMac_ = savedMac;
        setCentralError(lastError_, "invalid saved BMS MAC");
        state_ = State::Idle;
        return false;
    }

    if (!parseCanonicalMacBytes(canonical, targetMacBytes_)) {
        setCentralError(lastError_, "invalid saved BMS MAC");
        state_ = State::Idle;
        return false;
    }
    targetMac_ = canonical;
    wantConnection_ = true;
    state_ = State::Scanning;
    nextScanAt_ = 0;
    return true;
}

bool BmsCentral::normalizeMac(const String& input, String& canonical) {
    return normalizeMacValue(input, canonical);
}

bool BmsCentral::matchesTargetAddress(const NimBLEAddress& address) const {
    const uint8_t* const value = address.getVal();
    if (!value || targetMac_.length() != 17) return false;
    // NimBLE stores BLE addresses least-significant byte first; the saved MAC
    // uses normal display order. Compare bytes directly to avoid allocating a
    // String for every non-matching scan result.
    for (uint8_t index = 0; index < sizeof(targetMacBytes_); ++index) {
        if (targetMacBytes_[index] != value[sizeof(targetMacBytes_) - 1 - index]) {
            return false;
        }
    }
    return true;
}

bool BmsCentral::connectTo(const String& mac) {
    String canonical;
    if (!normalizeMacValue(mac, canonical)) {
        setCentralError(lastError_, "invalid BMS MAC");
        return false;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
        scan->stop();
        scanStopPending_ = true;
        scanStopRequestedAt_ = millis();
    }
    if (!parseCanonicalMacBytes(canonical, targetMacBytes_)) {
        setCentralError(lastError_, "invalid BMS MAC");
        return false;
    }
    targetMac_ = canonical;
    lastError_ = "";
    wantConnection_ = true;
    connectRequested_ = false;
    peerAddress_ = NimBLEAddress();

    // Never overlap a new scan/connect with an existing BMS GAP procedure.
    // A new target is remembered and will be scanned after the old procedure
    // reaches onConnectFail/onDisconnect.
    if (client_ && (client_->isConnected() || connectAttemptPending_ ||
                    connectCancelPending_ || disconnectPending_)) {
        if (client_->isConnected()) {
            connected_ = false;
            characteristic_ = nullptr;
            if (upstreamSubscribed_) {
        upstreamSubscribed_ = false;
        upstreamSubscribedAt_ = 0;
    }
    writeWithResponse_ = false;
            disconnectPending_ = true;
            state_ = State::Disconnected;
            client_->disconnect();
        } else if (connectAttemptPending_ || connectCancelPending_) {
            disconnectPending_ = true;
            connectCancelPending_ = true;
            state_ = State::Connecting;
            const bool cancelled = client_->cancelConnect();
            APP_LOGF("[Central] cancelled previous BMS connect: %s\n",
                          cancelled ? "requested" : "failed");
        } else {
            // A disconnect request is already in flight. Keep the new target
            // saved and let its terminal callback open the scan gate.
            state_ = State::Disconnected;
        }
        return true;
    }

    connected_ = false;
    connectAttemptPending_ = false;
    connectCancelPending_ = false;
    disconnectPending_ = false;
    characteristic_ = nullptr;
    if (upstreamSubscribed_) {
        upstreamSubscribed_ = false;
        upstreamSubscribedAt_ = 0;
    }
    writeWithResponse_ = false;
    state_ = State::Scanning;
    nextScanAt_ = 0;
    return true;
}

void BmsCentral::requestUpstreamSubscription(bool enabled) {
    const uint32_t now = millis();
    if (enabled && !upstreamSubscriptionRequested_) {
        replayQueries_.clear();
        replayCaptureStartedAt_ = now;
        replayCaptureActive_ = true;
        APP_LOGF("[Central] started %lu ms client-frame capture window\\n",
                 static_cast<unsigned long>(AppConfig::kReplayQueryCaptureWindowMs));
    } else if (!enabled) {
        replayCaptureActive_ = false;
        replayCaptureStartedAt_ = 0;
    }
    upstreamSubscriptionRequested_ = enabled;
    upstreamSubscriptionChangePending_ = true;
    upstreamSubscriptionRetryPending_ = false;
    upstreamSubscriptionNotBeforeAt_ = enabled ? now + AppConfig::kDownstreamInitialNotifyDelayMs : now;
}

bool BmsCentral::applyUpstreamSubscription() {
    if (!upstreamSubscriptionChangePending_) return true;
    if (!connected_ || !characteristic_ || disconnectPending_) return true;

    const uint32_t now = millis();
    if (upstreamSubscriptionRetryPending_ &&
        static_cast<int32_t>(now - upstreamSubscriptionRetryAt_) < 0) {
        return true;
    }
    upstreamSubscriptionRetryPending_ = false;
    if (upstreamSubscriptionRequested_ && static_cast<int32_t>(now - upstreamSubscriptionNotBeforeAt_) < 0) {
        return true;
    }

    if (upstreamSubscriptionRequested_) {
        if (upstreamSubscribed_) {
            upstreamSubscriptionChangePending_ = false;
            return true;
        }
        const bool notifications = characteristic_->canNotify();
        if (!notifications && !characteristic_->canIndicate()) {
            setCentralError(lastError_, "BMS FFE1 has no Notify/Indicate capability");
            upstreamSubscriptionChangePending_ = false;
            return false;
        }

        ++upstreamSubscriptionAttemptCount_;
        if (!characteristic_->subscribe(notifications, rawNotify, true)) {
            ++upstreamSubscriptionFailureCount_;
            setCentralError(lastError_,
                            notifications ? "BMS Notify subscription failed"
                                          : "BMS Indicate subscription failed");
            // Keep the request pending. A transient ATT/CCCD failure must not
            // require a phone reconnect before the relay becomes usable again.
            upstreamSubscriptionRetryPending_ = true;
            upstreamSubscriptionRetryAt_ = now + AppConfig::kUpstreamSubscriptionRetryMs;
            APP_LOGF("[Central] upstream subscription failed; retry in %lu ms\n",
                          static_cast<unsigned long>(AppConfig::kUpstreamSubscriptionRetryMs));
            return false;
        }
        ++upstreamSubscriptionSuccessCount_;
        upstreamSubscribed_ = true;
        upstreamSubscribedAt_ = now;
        upstreamSubscriptionChangePending_ = false;
        if (replayAfterSubscription_) {
            replayAfterSubscription_ = false;
            replayRecentQueries();
        }
        lastError_ = "";
        APP_LOGF("[Central] upstream %s subscribed by downstream client request\n",
                      notifications ? "Notify" : "Indicate");
        return true;
    }

    if (!upstreamSubscribed_) {
        upstreamSubscriptionChangePending_ = false;
        return true;
    }
    if (!characteristic_->unsubscribe(true)) {
        ++upstreamUnsubscribeFailureCount_;
        setCentralError(lastError_, "BMS upstream subscription removal failed");
        // Do not leave an unwanted upstream subscription permanently active.
        upstreamSubscriptionRetryPending_ = true;
        upstreamSubscriptionRetryAt_ = now + AppConfig::kUpstreamSubscriptionRetryMs;
        APP_LOGF("[Central] upstream unsubscribe failed; retry in %lu ms\n",
                      static_cast<unsigned long>(AppConfig::kUpstreamSubscriptionRetryMs));
        return false;
    }
    upstreamSubscribed_ = false;
    upstreamSubscriptionChangePending_ = false;
    lastError_ = "";
    APP_LOGLN("[Central] upstream subscription removed after last downstream client unsubscribed");
    return true;
}

void BmsCentral::startScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan || targetMac_.isEmpty() || connected_ || connectAttemptPending_ ||
        connectCancelPending_ || disconnectPending_ || scanStopPending_ || scan->isScanning()) return;
    scan->clearResults();
    scan->setActiveScan(true);
    scan->setInterval(500);
    scan->setWindow(100);
#if CONFIG_BT_NIMBLE_EXT_ADV
    scan->setPhy(NimBLEScan::SCAN_1M);
#endif
    state_ = State::Scanning;
    // NimBLE-Arduino uses milliseconds here. Passing 5 with extended
    // advertising becomes 0 after the library's 10 ms conversion and leaves
    // the scan running forever.
    if (!scan->start(5000, false, true)) {
        setCentralError(lastError_, "scan start failed");
        state_ = State::Disconnected;
        nextScanAt_ = millis() + AppConfig::kReconnectDelayMs;
        APP_LOGLN("[Central] BMS scan start failed");
        return;
    }
    scanStopPending_ = false;
    scanStopRequestedAt_ = 0;
    nextScanAt_ = millis() + 6000;
    APP_LOGF("[Central] Scanning for %s\n", targetMac_.c_str());
}

bool BmsCentral::connectDiscoveredDevice() {
    if (peerAddress_.isNull() || disconnectPending_ || connectAttemptPending_ ||
        connectCancelPending_) return false;

    const NimBLEAddress peerAddress = peerAddress_;
    if (client_ && client_->isConnected()) {
        setCentralError(lastError_, "previous BMS client is still connected");
        return false;
    }

    // Follow the reference project: reuse the Client associated with the
    // scanned peer address. If the MAC was changed, remove the old idle Client
    // before creating a Client for the new peer.
    if (client_ && client_->getPeerAddress() != peerAddress) {
        if (!NimBLEDevice::deleteClient(client_)) {
            setCentralError(lastError_, "delete old BMS client failed");
            return false;
        }
        client_ = nullptr;
    }

    bool newClient = false;
    if (!client_) client_ = NimBLEDevice::getClientByPeerAddress(peerAddress);
    if (!client_) {
        client_ = NimBLEDevice::createClient(peerAddress);
        newClient = true;
    }
    if (!client_) {
        setCentralError(lastError_, "createClient failed");
        return false;
    }
    if (newClient) client_->setClientCallbacks(new CentralClientCallbacks(), true);

    // These are the same stable parameters used by JK-BMS-4G. Restricting the
    // initial connection procedure to 1M improves compatibility with older JK
    // Bluetooth modules while keeping the normal legacy advertisement path.
    client_->setConnectionParams(60, 60, 1, 400);
#if CONFIG_BT_NIMBLE_EXT_ADV
    client_->setConnectPhy(BLE_GAP_LE_PHY_1M_MASK);
#endif
    client_->setConnectTimeout(8000);

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
        scan->stop();
        scanStopPending_ = true;
        scanStopRequestedAt_ = millis();
        connectRequested_ = true;
        return false;
    }

    connectAttemptPending_ = true;
    // Request an ATT MTU exchange. The local preferred MTU is configured to
    // 517; the final value is negotiated with the BMS and may be lower.
    if (!client_->connect(peerAddress, true, false, true)) {
        const int error = client_->getLastError();
        const char* errorText = NimBLEUtils::returnCodeToString(error);
        const bool linkEstablished = client_->isConnected();
        if (linkEstablished) {
            // A setup step failed after the ACL link was established. This is
            // not a pending GAP connect, so terminate the real connection.
            connectAttemptPending_ = false;
            connectCancelPending_ = false;
            disconnectPending_ = true;
            state_ = State::Disconnected;
            client_->disconnect();
            APP_LOGLN("[Central] BMS link established but connection setup failed; disconnecting");
        } else {
            // connect() is synchronous in this project. On timeout it has
            // already cancelled the GAP procedure, so do not wait for a
            // second terminal callback that may never arrive.
            connectAttemptPending_ = false;
            connectCancelPending_ = false;
            disconnectPending_ = false;
            peerAddress_ = NimBLEAddress();
            state_ = wantConnection_ ? State::Disconnected : State::Idle;
            nextScanAt_ = wantConnection_ ? millis() + AppConfig::kReconnectDelayMs : 0;
        }
        connected_ = false;
        connectRequested_ = false;
        characteristic_ = nullptr;
        if (upstreamSubscribed_) {
        upstreamSubscribed_ = false;
        upstreamSubscribedAt_ = 0;
    }
    writeWithResponse_ = false;
        setCentralError(lastError_, String("connect failed (") + error + "): " + errorText);
        APP_LOGF("[Central] BMS connect failed, lastError=%d (%s)\n", error, errorText);
        return false;
    }

    // A late connect callback may have observed a manual cancellation. Do not
    // proceed with discovery in that case; the disconnect callback owns final
    // cleanup and the retry gate.
    if (!wantConnection_ || connectCancelPending_ || disconnectPending_ ||
        !client_->isConnected()) {
        if (client_->isConnected()) {
            disconnectPending_ = true;
            client_->disconnect();
        }
        return false;
    }

    NimBLERemoteService* service = client_->getService(AppConfig::kBmsServiceUuid);
    const int ffe0Error = client_->getLastError();
    const char* serviceName = AppConfig::kBmsServiceUuid;
    int serviceError = ffe0Error;
    if (!service && client_->isConnected()) {
        service = client_->getService(AppConfig::kBmsVendorServiceUuid);
        serviceName = AppConfig::kBmsVendorServiceUuid;
        serviceError = client_->getLastError();
    }
    if (!service) {
        const bool stillConnected = client_->isConnected();
        const char* errorText = NimBLEUtils::returnCodeToString(serviceError);
        setCentralError(
            lastError_,
            String("BMS service discovery failed (FFE0/E07F), firstError=") +
                serviceError + ": " + errorText);
        APP_LOGF("[Central] BMS service discovery failed, firstError=%d (%s), connected=%s\n",
                      serviceError, errorText, stillConnected ? "yes" : "no");
        connected_ = false;
        characteristic_ = nullptr;
        if (upstreamSubscribed_) {
        upstreamSubscribed_ = false;
        upstreamSubscribedAt_ = 0;
    }
    writeWithResponse_ = false;
        connectAttemptPending_ = false;
        connectCancelPending_ = false;
        connectRequested_ = false;
        state_ = State::Disconnected;
        if (stillConnected) {
            disconnectPending_ = true;
            client_->disconnect();
        } else {
            // The GAP disconnect may already have completed while service
            // discovery was waiting. Do not wait for a callback that will not
            // arrive; finalize the local state here.
            disconnectPending_ = false;
            peerAddress_ = NimBLEAddress();
            nextScanAt_ = wantConnection_ ? millis() + AppConfig::kReconnectDelayMs : 0;
        }
        return false;
    }

    NimBLERemoteCharacteristic* characteristic =
        service->getCharacteristic(AppConfig::kBmsCharacteristicUuid);
    if (!characteristic ||
        (!characteristic->canWriteNoResponse() && !characteristic->canWrite())) {
        setCentralError(lastError_, String("FFE1 missing write capability on service ") + serviceName);
        APP_LOGF("[Central] FFE1 missing write capability on %s\n", serviceName);
        disconnectPending_ = true;
        state_ = State::Disconnected;
        client_->disconnect();
        return false;
    }

    characteristic_ = characteristic;
    upstreamSubscribed_ = false;
    upstreamSubscriptionChangePending_ = upstreamSubscriptionRequested_;
    upstreamSubscriptionRetryPending_ = false;
    writeWithResponse_ = !characteristic_->canWriteNoResponse() && characteristic_->canWrite();
    connected_ = true;
    disconnectPending_ = false;
    state_ = State::Connected;
    lastError_ = "";
    APP_LOGF("[Central] %s/FFE1 discovered, upstream subscription requested=%s, write=%s\n",
                  serviceName, upstreamSubscriptionRequested_ ? "yes" : "no",
                  writeWithResponse_ ? "with-response" : "without-response");

    // The downstream client's CCCD request is applied by loop(). No BMS
    // protocol query or unsolicited upstream subscription is generated here.
    return true;
}

void BmsCentral::disconnect() {
    replayAfterSubscription_ = false;
    replayCaptureActive_ = false;
    replayCaptureStartedAt_ = 0;
    lastUpstreamWriteAt_ = 0;
    wantConnection_ = false;
    connectRequested_ = false;
    peerAddress_ = NimBLEAddress();

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
        scan->stop();
        scanStopPending_ = true;
        scanStopRequestedAt_ = millis();
    }

    if (client_ && client_->isConnected()) {
        connected_ = false;
        disconnectPending_ = true;
        state_ = State::Disconnected;
        client_->disconnect();
        return;
    }

    if (client_ && (connectAttemptPending_ || connectCancelPending_)) {
        connected_ = false;
        disconnectPending_ = true;
        connectCancelPending_ = true;
        state_ = State::Connecting;
        const bool cancelled = client_->cancelConnect();
        APP_LOGF("[Central] cancelled BMS connect for disconnect: %s\n",
                      cancelled ? "requested" : "failed");
        return;
    }

    connected_ = false;
    connectAttemptPending_ = false;
    connectCancelPending_ = false;
    disconnectPending_ = false;
    characteristic_ = nullptr;
    if (upstreamSubscribed_) {
        upstreamSubscribed_ = false;
        upstreamSubscribedAt_ = 0;
    }
    writeWithResponse_ = false;
    state_ = State::Idle;
}

bool BmsCentral::write(const uint8_t* data, size_t length) {
    if (!connected_ || !characteristic_ || !data || length == 0) return false;
    if (!characteristic_->writeValue(data, length, writeWithResponse_)) return false;
    if (!replayingQueries_ && replayCaptureActive_) {
        if (millis() - replayCaptureStartedAt_ < AppConfig::kReplayQueryCaptureWindowMs) {
            ReplayQuery query;
            query.data.assign(data, data + length);
            replayQueries_.push_back(query);
        } else {
            replayCaptureActive_ = false;
        }
    }
    lastUpstreamWriteAt_ = millis();
    return true;
}

void BmsCentral::exportReplayFrames(WebServer& server) const {
    server.sendHeader("Content-Disposition", "attachment; filename=\"bms_replay_frames.hex\"");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain; charset=UTF-8", "");
    static const char hex[] = "0123456789ABCDEF";
    for (const auto& query : replayQueries_) {
        String line;
        line.reserve(query.data.size() * 3 + 2);
        for (size_t i = 0; i < query.data.size(); ++i) {
            if (i) line += ' ';
            line += hex[(query.data[i] >> 4) & 0x0F];
            line += hex[query.data[i] & 0x0F];
        }
        line += "\\r\\n";
        server.sendContent(line);
    }
}

void BmsCentral::replayRecentQueries() {
    if (!client_ || !client_->isConnected() || !characteristic_ || replayQueries_.empty()) return;
    replayingQueries_ = true;
    // Replay every client frame in its original order. This intentionally
    // includes the complete handshake/query sequence, not just the newest
    // command; the target BMS requires that sequence before it responds.
    for (const ReplayQuery& query : replayQueries_) {
        if (query.data.empty()) continue;
        const bool sent = characteristic_->writeValue(
            query.data.data(), query.data.size(), writeWithResponse_);
        APP_LOGF("[Central] replayed client frame len=%u result=%s\\n",
                 static_cast<unsigned>(query.data.size()), sent ? "ok" : "failed");
        if (sent) lastUpstreamWriteAt_ = millis();
    }
    replayingQueries_ = false;
}

void BmsCentral::loop() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scanStopPending_) {
        const uint32_t now = millis();
        constexpr uint32_t kScanStopTimeoutMs = 1500U;
        if (scan && scan->isScanning()) {
            if (now - scanStopRequestedAt_ < kScanStopTimeoutMs) return;
            APP_LOGLN("[Central] Scan stop timeout; retrying stop request");
            scan->stop();
            if (scan->isScanning()) return;
        }
        scanStopPending_ = false;
        scanStopRequestedAt_ = 0;
    }

    if (connectRequested_ && !connected_ && !connectAttemptPending_ &&
        !connectCancelPending_ && !disconnectPending_) {
        connectRequested_ = false;
        if (!connectDiscoveredDevice() && !connectAttemptPending_ &&
            !connectCancelPending_ && !disconnectPending_ && !scanStopPending_) {
            nextScanAt_ = millis() + AppConfig::kReconnectDelayMs;
        }
    }
    if (wantConnection_ && !connected_ && !connectAttemptPending_ &&
        !connectCancelPending_ && !disconnectPending_ && !connectRequested_ &&
        !scanStopPending_ && !targetMac_.isEmpty() &&
        static_cast<int32_t>(millis() - nextScanAt_) >= 0) {
        startScan();
    }
    const uint32_t now = millis();
    if (connected_ && upstreamSubscriptionRequested_ && upstreamSubscribed_ &&
        !upstreamSubscriptionChangePending_ && characteristic_) {
        const uint32_t notifyReference = upstreamSubscribedAt_ > lastUpstreamNotifyAt_
            ? upstreamSubscribedAt_ : lastUpstreamNotifyAt_;
        const bool notifyStale = notifyReference != 0 &&
            now - notifyReference >= AppConfig::kUpstreamNotifyWatchdogMs;
        const bool responseStale = lastUpstreamWriteAt_ != 0 &&
            now - lastUpstreamWriteAt_ >= AppConfig::kUpstreamResponseWatchdogMs;
        if (notifyStale || responseStale) {
            if (characteristic_->unsubscribe(true)) {
                upstreamSubscribed_ = false;
                upstreamSubscriptionChangePending_ = true;
                upstreamSubscriptionRetryPending_ = false;
                upstreamSubscriptionNotBeforeAt_ = now + AppConfig::kDownstreamInitialNotifyDelayMs;
                replayAfterSubscription_ = true;
                lastUpstreamWriteAt_ = 0;
                APP_LOGF("[Central] upstream Notify watchdog recovery (%s)\\n",
                         responseStale ? "response_timeout" : "notify_stale");
            } else {
                upstreamSubscriptionRetryPending_ = true;
                upstreamSubscriptionRetryAt_ = now + AppConfig::kUpstreamSubscriptionRetryMs;
                setCentralError(lastError_, "BMS upstream watchdog unsubscribe failed");
            }
        }
    }
    applyUpstreamSubscription();
}

void BmsCentral::onNotify(const uint8_t* data, size_t length) {
    if (!data || length == 0) return;
    lastUpstreamNotifyAt_ = millis();
    lastUpstreamWriteAt_ = 0;
    lastUpstreamNotifyLength_ = static_cast<uint16_t>(length);
    ++upstreamNotifyCount_;
    if (notifyCallback_) notifyCallback_(data, length);
}
