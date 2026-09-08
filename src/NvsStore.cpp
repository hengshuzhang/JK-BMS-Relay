#include "NvsStore.h"

#include <Preferences.h>

namespace {
Preferences preferences;

String signalKey(uint8_t index) {
    return String("sig") + index;
}
}

bool NvsStore::begin() {
    return preferences.begin("jkbms", false);
}

PersistentConfig NvsStore::load() const {
    PersistentConfig config;
    preferences.end();
    if (!preferences.begin("jkbms", true)) return config;

    config.bmsMac = preferences.getString("mac", "");
    const uint8_t storedCount = preferences.getUChar(
        "sig_count", AppConfig::kDefaultVirtualSignalCount);
    // Earlier firmware allowed three or four virtual signals. Keep the first
    // two saved names, but always start this fixed two-signal firmware with
    // exactly two advertising instances.
    if (storedCount != AppConfig::kDefaultVirtualSignalCount) {
        APP_LOGF("[NVS] Migrating stored virtual signal count %u to 2\n",
                      static_cast<unsigned>(storedCount));
    }
    config.virtualSignalCount = AppConfig::kDefaultVirtualSignalCount;
    for (uint8_t i = 0; i < AppConfig::kMaxVirtualSignals; ++i) {
        const String fallback = AppConfig::defaultVirtualSignalName(i);
        config.virtualSignalNames[i] = preferences.getString(signalKey(i).c_str(), fallback);
        if (config.virtualSignalNames[i].isEmpty() ||
            config.virtualSignalNames[i].length() > AppConfig::kVirtualSignalNameMaxLength) {
            config.virtualSignalNames[i] = fallback;
        }
    }
    preferences.end();
    return config;
}

bool NvsStore::saveBmsMac(const String& mac) {
    if (!preferences.begin("jkbms", false)) return false;
    const bool ok = preferences.putString("mac", mac) == mac.length();
    preferences.end();
    return ok;
}

bool NvsStore::saveVirtualSignals(uint8_t count, const String* names) {
    if (!names || count != AppConfig::kDefaultVirtualSignalCount) return false;
    if (!preferences.begin("jkbms", false)) return false;

    bool ok = preferences.putUChar("sig_count", AppConfig::kDefaultVirtualSignalCount) ==
              sizeof(uint8_t);
    for (uint8_t i = 0; i < AppConfig::kMaxVirtualSignals; ++i) {
        const String& value = names[i];
        if (value.isEmpty() || value.length() > AppConfig::kVirtualSignalNameMaxLength ||
            preferences.putString(signalKey(i).c_str(), value) != value.length()) {
            ok = false;
        }
    }
    // Reclaim obsolete settings created by firmware that supported C3/C4.
    preferences.remove("sig2");
    preferences.remove("sig3");
    preferences.end();
    return ok;
}
