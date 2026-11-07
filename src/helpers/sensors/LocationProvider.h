#pragma once

#include "Mesh.h"


class LocationProvider {
protected:
    bool _time_sync_needed = true;

public:
    virtual void syncTime() { _time_sync_needed = true; }
    virtual bool waitingTimeSync() { return _time_sync_needed; }
    virtual long getLatitude() = 0;
    virtual long getLongitude() = 0;
    virtual long getAltitude() = 0;
    virtual long satellitesCount() = 0;
    virtual bool isValid() = 0;
    // Ground speed from the receiver. False when the sentence has no speed, so a parked fix is not treated as travel.
    virtual bool hasSpeed() { return false; }
    virtual float getSpeedMps() { return 0; }
    virtual uint32_t rxBytes() { return 0; }
    virtual uint32_t nmeaMarks() { return 0; }
    virtual const char* lastSentence() { return ""; }
    virtual uint32_t ggaCount() { return 0; }
    virtual uint32_t rmcCount() { return 0; }
    virtual uint32_t gsvCount() { return 0; }
    virtual long getTimestamp() = 0;
    virtual void sendSentence(const char * sentence);
    virtual void reset() = 0;
    virtual void begin() = 0;
    virtual void stop() = 0;
    virtual void loop() = 0;
    virtual bool isEnabled() = 0;
};
