#pragma once

#include "AppConfig.h"

class NvsStore {
public:
    bool begin();
    PersistentConfig load() const;
    bool saveBmsMac(const String& mac);
    bool saveVirtualSignals(uint8_t count, const String* names);

private:
};
