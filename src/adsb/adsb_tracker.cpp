#include "adsb/adsb_tracker.h"

#include <algorithm>
#include <cmath>

namespace adsb
{

// CPR longitude-zone count NL(lat) = floor(2*pi / acos(1 - (1-cos(pi/30))/cos^2(lat)))
static double nl(double declat)
{
    if (declat < 0.0) declat = -declat;
    if (declat > 87.0) return 1.0;
    double a = 1.0 - std::cos(M_PI / 30.0); // NZ = 15 -> pi/(2*NZ) = pi/30
    double c = std::cos(declat * M_PI / 180.0);
    double b = c * c;
    double v = 1.0 - a / b;
    if (v < -1.0) v = -1.0;
    if (v > 1.0) v = 1.0;
    return std::floor((2.0 * M_PI) / std::acos(v));
}

// Mathematical modulo (always non-negative), unlike fmod.
static double posMod(double a, double b) { return a - b * std::floor(a / b); }

// Approximate great-circle distance in nautical miles.
static double distNm(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * 60.0;
    double dlon = (lon2 - lon1) * 60.0 *
                  std::cos((lat1 + lat2) * 0.5 * M_PI / 180.0);
    return std::sqrt(dlat * dlat + dlon * dlon);
}

// Global CPR decode. `evenNewer` selects which frame's latitude zone is the
// reference (dump1090/pyModeS). `span` is 360 for airborne, 90 for surface.
static bool cprDecode(double evenLat, double evenLon, double oddLat, double oddLon,
                      bool evenNewer, double span, double& outLat, double& outLon)
{
    const double dLatEven = span / 60.0;
    const double dLatOdd  = span / 59.0;

    double j = std::floor(59.0 * evenLat - 60.0 * oddLat + 0.5);
    double latEven = dLatEven * (posMod(j, 60.0) + evenLat);
    double latOdd  = dLatOdd  * (posMod(j, 59.0) + oddLat);

    if (latEven >= 270) latEven -= 360;
    if (latOdd >= 270)  latOdd -= 360;

    double lat, lon, ni;
    if (evenNewer)
    {
        lat = latEven;
        double n = nl(lat);
        if (n < 1) n = 1;
        ni = n;
        double m = std::floor(evenLon * (n - 1.0) - oddLon * n + 0.5);
        lon = (span / ni) * (posMod(m, ni) + evenLon);
    }
    else
    {
        lat = latOdd;
        double n = nl(lat);
        if (n < 1) n = 1;
        ni = n - 1.0;
        if (ni < 1) ni = 1;
        double m = std::floor(evenLon * (n - 1.0) - oddLon * n + 0.5);
        lon = (span / ni) * (posMod(m, ni) + oddLon);
    }

    if (lon >= 180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return false;

    outLat = lat;
    outLon = lon;
    return true;
}

TrackResult Tracker::update(const Decoded& msg, double nowSec)
{
    std::lock_guard<std::mutex> lk(mtx_);
    Entry& e = byId_[msg.icao];
    TrackedAircraft& a = e.ac;

    if (a.icao == 0)
    {
        a.icao = msg.icao;
        a.firstSeen = nowSec;
    }

    a.lastSeen = nowSec;
    ++a.msgs;
    a.sigLevel = msg.sigLevel;

    if (!msg.callsign.empty()) a.callsign = msg.callsign;

    if (msg.altitude != 0 || (msg.meType >= 9 && msg.meType <= 22))
        a.altitude = msg.altitude;
    if (msg.speed > 0) a.speed = msg.speed;
    if (msg.heading > 0) a.heading = msg.heading;
    if (msg.vertRate != 0) a.vertRate = msg.vertRate;
    if (msg.squawk > 0) a.squawk = msg.squawk;

    if (msg.meType >= 5 && msg.meType <= 22)
        updateCPR(e, msg, nowSec);

    TrackResult r;
    r.hasPos = a.hasPos;
    r.lat = a.lat;
    r.lon = a.lon;
    r.alt = a.altitude;
    r.heading = a.heading;
    r.callsign = a.callsign;
    return r;
}

void Tracker::updateCPR(Entry& e, const Decoded& msg, double nowSec)
{
    if (msg.cprEven)
    {
        e.cprEvenLat = msg.lat;
        e.cprEvenLon = msg.lon;
        e.cprEvenTime = nowSec;
        e.cprEvenValid = true;
    }
    else
    {
        e.cprOddLat = msg.lat;
        e.cprOddLon = msg.lon;
        e.cprOddTime = nowSec;
        e.cprOddValid = true;
    }

    if (!e.cprEvenValid || !e.cprOddValid)
        return;

    // The pair must be received close together, otherwise the decode locks to
    // the wrong latitude/longitude zone and the aircraft jumps around. dump1090
    // uses 10 s; with sparse reception a stale frame must simply be discarded.
    if (std::fabs(e.cprEvenTime - e.cprOddTime) > 10.0)
        return;

    bool evenNewer = e.cprEvenTime >= e.cprOddTime;
    double span = (msg.meType >= 5 && msg.meType <= 8) ? 90.0 : 360.0;

    double lat = 0.0, lon = 0.0;
    if (!cprDecode(e.cprEvenLat, e.cprEvenLon, e.cprOddLat, e.cprOddLon,
                   evenNewer, span, lat, lon))
        return;

    // Reject implausible jumps. A single flipped bit in the CPR field still
    // passes CRC but resolves to a position in the wrong zone (the "other side
    // of the world"). Only accept a fix that is reachable from the last one.
    if (e.cprValid && e.lastPosTime > 0.0)
    {
        double dt = nowSec - e.lastPosTime;
        if (dt >= 0.0 && dt < 60.0)
        {
            double d = distNm(e.ac.lat, e.ac.lon, lat, lon);
            double maxNm = 1.0 + 0.6 * dt; // ~1 nm jitter + up to ~2160 kt
            // Accept after several consistent rejects in case the *old* fix was
            // the corrupt one.
            if (d > maxNm && ++e.posRejects < 4)
                return;
        }
    }

    e.posRejects = 0;
    e.ac.lat = lat;
    e.ac.lon = lon;
    e.cprValid = true;
    e.ac.hasPos = true;
    e.lastPosTime = nowSec;
}

void Tracker::removeStale(double nowSec, double ttlSec)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = byId_.begin(); it != byId_.end(); )
    {
        if (nowSec - it->second.ac.lastSeen > ttlSec)
            it = byId_.erase(it);
        else
            ++it;
    }
}

int Tracker::count()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return (int)byId_.size();
}

std::vector<TrackedAircraft> Tracker::snapshot()
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<TrackedAircraft> out;
    out.reserve(byId_.size());
    for (auto& kv : byId_)
        out.push_back(kv.second.ac);
    return out;
}

} // namespace adsb
