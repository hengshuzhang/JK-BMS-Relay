#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>
#include <vector>

class WebServer;

#include "AppConfig.h"

class BmsCentral {
public:
    enum class State { Idle, Scanning, Connecting, Connected, Disconnected };
    using NotifyCallback = std::function<void(const uint8_t*, size_t)>;

    bool begin(const String& savedMac);
    void loop();
    static bool normalizeMac(const String& input, String& canonical);
    bool connectTo(const String& mac);
    void disconnect();
    bool write(const uint8_t* data, size_t length);
    void onNotify(const uint8_t* data, size_t length);
    void setNotifyCallback(NotifyCallback callback) { notifyCallback_ = callback; }
    void requestUpstreamSubscription(bool enabled);
    bool upstreamSubscriptionRequested() const { return upstreamSubscriptionRequested_; }
    bool upstreamSubscriptionPending() const { return upstreamSubscriptionChangePending_; }
    bool upstreamSubscribed() const { return upstreamSubscribed_; }
    bool connected() const { return connected_; }
    bool hasUpstreamNotify() const { return lastUpstreamNotifyAt_ != 0; }
    uint32_t lastUpstreamNotifyAt() const { return lastUpstreamNotifyAt_; }
    uint16_t lastUpstreamNotifyLength() const { return lastUpstreamNotifyLength_; }
    uint32_t upstreamNotifyCount() const { return upstreamNotifyCount_; }
    uint32_t upstreamSubscriptionAttemptCount() const { return upstreamSubscriptionAttemptCount_; }
    uint32_t upstreamSubscriptionSuccessCount() const { return upstreamSubscriptionSuccessCount_; }
    uint32_t upstreamSubscriptionFailureCount() const { return upstreamSubscriptionFailureCount_; }
    uint32_t upstreamUnsubscribeFailureCount() const { return upstreamUnsubscribeFailureCount_; }
    uint32_t replayFrameCount() const { return static_cast<uint32_t>(replayQueries_.size()); }
    void exportReplayFrames(WebServer& server) const;
    State state() const { return state_; }
    const String& targetMac() const { return targetMac_; }
    const String& lastError() const { return lastError_; }

private:
    NimBLEClient* client_{nullptr};
    NimBLERemoteCharacteristic* characteristic_{nullptr};
    NimBLEAddress peerAddress_{};
    uint8_t targetMacBytes_[6]{};
    NotifyCallback notifyCallback_;
    String targetMac_;
    String lastError_;
    State state_{State::Idle};
    bool wantConnection_{false};
    bool connectRequested_{false};
    bool connectAttemptPending_{false};
    bool connectCancelPending_{false};
    bool scanStopPending_{false};
    bool connected_{false};
    bool disconnectPending_{false};
    bool writeWithResponse_{false};
    bool upstreamSubscriptionRequested_{false};
    bool upstreamSubscriptionChangePending_{false};
    bool upstreamSubscriptionRetryPending_{false};
    bool upstreamSubscribed_{false};
    uint32_t nextScanAt_{0};
    uint32_t scanStopRequestedAt_{0};
    uint32_t upstreamSubscriptionRetryAt_{0};
    uint32_t upstreamSubscriptionNotBeforeAt_{0};
    uint32_t upstreamSubscribedAt_{0};
    uint32_t lastUpstreamWriteAt_{0};
    uint32_t replayCaptureStartedAt_{0};
    bool replayCaptureActive_{false};
    struct ReplayQuery {
        std::vector<uint8_t> data;
    };
    std::vector<ReplayQuery> replayQueries_;
    bool replayAfterSubscription_{false};
    bool replayingQueries_{false};
    uint32_t lastUpstreamNotifyAt_{0};
    uint16_t lastUpstreamNotifyLength_{0};
    uint32_t upstreamNotifyCount_{0};
    uint32_t upstreamSubscriptionAttemptCount_{0};
    uint32_t upstreamSubscriptionSuccessCount_{0};
    uint32_t upstreamSubscriptionFailureCount_{0};
    uint32_t upstreamUnsubscribeFailureCount_{0};

    bool connectDiscoveredDevice();
    bool matchesTargetAddress(const NimBLEAddress& address) const;
    void startScan();
    bool applyUpstreamSubscription();
    void replayRecentQueries();
    friend class CentralClientCallbacks;
    friend class CentralScanCallbacks;
};
