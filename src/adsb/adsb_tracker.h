// ADS-B aircraft tracker with Compact Position Reporting (CPR) decoding.
// Ported from goadsb (MIT).
#pragma once

#include "adsb/adsb_decoder.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace adsb
{

struct TrackedAircraft
{
    uint32_t icao = 0;
    std::string callsign;
    int32_t  altitude = 0;
    double   speed = 0.0;
    double   heading = 0.0;
    int32_t  vertRate = 0;
    double   lat = 0.0;
    double   lon = 0.0;
    bool     hasPos = false;
    uint16_t squawk = 0;
    uint64_t msgs = 0;
    double   lastSeen = 0.0;
    double   firstSeen = 0.0;
    double   sigLevel = 0.0;
};

struct TrackResult
{
    bool    hasPos = false;
    double  lat = 0.0;
    double  lon = 0.0;
    int32_t alt = 0;
    std::string callsign;
};

class Tracker
{
public:
    // Apply a decoded message. Returns the current resolved position (whether
    // or not this particular message contained one).
    TrackResult update(const Decoded& msg, double nowSec);

    void removeStale(double nowSec, double ttlSec = 60.0);

    int count();

    std::vector<TrackedAircraft> snapshot();

private:
    struct Entry
    {
        TrackedAircraft ac;
        double cprOddLat = 0, cprOddLon = 0;
        double cprEvenLat = 0, cprEvenLon = 0;
        double cprOddTime = 0, cprEvenTime = 0;
        bool   cprOddValid = false, cprEvenValid = false;
        bool   cprValid = false;
    };

    void updateCPR(Entry& e, const Decoded& msg, double nowSec);

    std::mutex mtx_;
    std::map<uint32_t, Entry> byId_;
};

} // namespace adsb
