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

static void airborneCPRDecode(double evenLat, double evenLon, double oddLat, double oddLon,
                              double& outLat, double& outLon)
{
    const double dLatEven = 360.0 / 60.0;
    const double dLatOdd  = 360.0 / 59.0;

    double j = std::floor(59.0 * evenLat - 60.0 * oddLat + 0.5);
    double latEven = dLatEven * (std::fmod(j, 60.0) + evenLat);
    double latOdd  = dLatOdd  * (std::fmod(j, 59.0) + oddLat);

    if (latEven >= 270) latEven -= 360;
    if (latOdd >= 270)  latOdd -= 360;

    if (nl(latEven) != nl(latOdd))
    {
        outLat = latEven;
        outLon = evenLon;
        return;
    }

    double ni = nl(latEven);
    if (ni < 1) ni = 1;

    double dLon = 360.0 / ni;
    double m = std::floor(evenLon * (ni - 1.0) - oddLon * ni + 0.5);
    double lon = dLon * (std::fmod(m, ni) + evenLon);
    if (lon >= 180) lon -= 360;

    outLat = latEven;
    outLon = lon;
}

static void surfaceCPRDecode(double evenLat, double evenLon, double oddLat, double oddLon,
                             double& outLat, double& outLon)
{
    const double dLatEven = 90.0 / 60.0;
    const double dLatOdd  = 90.0 / 59.0;

    double j = std::floor(59.0 * evenLat - 60.0 * oddLat + 0.5);
    double latEven = dLatEven * (std::fmod(j, 60.0) + evenLat);
    double latOdd  = dLatOdd  * (std::fmod(j, 59.0) + oddLat);

    if (latEven >= 270) latEven -= 360;
    if (latOdd >= 270)  latOdd -= 360;

    if (nl(latEven) != nl(latOdd))
    {
        outLat = latEven;
        outLon = evenLon;
        return;
    }

    double ni = nl(latEven);
    if (ni < 1) ni = 1;

    double dLon = 90.0 / ni;
    double m = std::floor(evenLon * (ni - 1.0) - oddLon * ni + 0.5);
    double lon = dLon * (std::fmod(m, ni) + evenLon);
    if (lon >= 180) lon -= 360;

    outLat = latEven;
    outLon = lon;
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

    double evenLat = e.cprEvenLat, evenLon = e.cprEvenLon;
    double oddLat = e.cprOddLat, oddLon = e.cprOddLon;

    if (msg.meType >= 5 && msg.meType <= 8)
        surfaceCPRDecode(evenLat, evenLon, oddLat, oddLon, e.ac.lat, e.ac.lon);
    else
        airborneCPRDecode(evenLat, evenLon, oddLat, oddLon, e.ac.lat, e.ac.lon);

    e.cprValid = true;
    e.ac.hasPos = true;
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
