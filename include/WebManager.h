#pragma once

#include <WebServer.h>

class BmsCentral;
class JkBmsNimbleRelay;
class NvsStore;

class WebManager {
public:
    WebManager(BmsCentral& central, JkBmsNimbleRelay& relay, NvsStore& store);
    void begin();
    void handle();

private:
    WebServer server_{80};
    BmsCentral& central_;
    JkBmsNimbleRelay& relay_;
    NvsStore& store_;
    void root();
    void status();
    void connect();
    void disconnect();
    void updateSignals();
    void exportFrames();
    bool startSoftAp();
    void updatePowerSaving();
    bool wifiSleeping_ = false;
    bool otaUploadStarted_ = false;
    bool otaUploadComplete_ = false;
    unsigned long bmsConnectedSince_ = 0;
    unsigned long nextWifiRestoreAt_ = 0;
};
