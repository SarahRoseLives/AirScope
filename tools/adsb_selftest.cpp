#include "adsb/adsb_decoder.h"
#include "adsb/adsb_tracker.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static bool fromHex(const char* h, adsb::RawMessage& rm) {
    int n = (int)std::strlen(h);
    if (n != 28) { std::printf("len != 28\n"); return false; }
    for (int i = 0; i < 14; ++i) {
        auto nib = [](char c)->int{ if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; if(c>='a'&&c<='f')return c-'a'+10; return 0;};
        rm.data[i] = (uint8_t)((nib(h[i*2])<<4) | nib(h[i*2+1]));
    }
    rm.bits = 112;
    return true;
}

int main() {
    adsb::init(1);

    // Identification: KLM1023, ICAO 4840D6
    {
        adsb::RawMessage raw; fromHex("8D4840D6202CC371C32CE0576098", raw);
        adsb::Decoded d; adsb::decode(raw, d);
        std::printf("ident: crc=%d icao=%06X cs='%s'  (expect 4840D6/KLM1023)\n",
                    (int)d.crc, d.icao, d.callsign.c_str());
    }

    // Airborne position even/odd pair -> lat 52.2572, lon 3.91937
    {
        adsb::RawMessage e, o;
        fromHex("8D40621D58C382D690C8AC2863A7", e);
        fromHex("8D40621D58C386435CC412692AD6", o);
        adsb::Decoded de, dob;
        adsb::decode(e, de); adsb::decode(o, dob);
        std::printf("even: crc=%d type=%u alt=%d\n", (int)de.crc, de.meType, de.altitude);
        std::printf("odd : crc=%d type=%u alt=%d\n", (int)dob.crc, dob.meType, dob.altitude);
        adsb::Tracker t;
        t.update(de, 0.0);
        auto r = t.update(dob, 0.0);
        std::printf("cpr : hasPos=%d lat=%.5f lon=%.5f alt=%d  (expect ~52.25720/3.91937)\n",
                    (int)r.hasPos, r.lat, r.lon, r.alt);
        bool ok = r.hasPos && std::fabs(r.lat-52.25720)<0.01 && std::fabs(r.lon-3.91937)<0.01;
        std::printf("%s\n", ok ? "CPR OK" : "CPR FAIL");
    }

    // Velocity: classic 8D485020994409940838175B284F -> ICAO 485020
    {
        adsb::RawMessage raw; fromHex("8D485020994409940838175B284F", raw);
        adsb::Decoded d; adsb::decode(raw, d);
        std::printf("vel : crc=%d type=%u spd=%.1f hdg=%.1f vr=%d\n",
                    (int)d.crc, d.meType, d.speed, d.heading, d.vertRate);
    }
    return 0;
}
