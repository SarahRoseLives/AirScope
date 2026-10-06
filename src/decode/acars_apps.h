// Thin C++ wrapper around libacars: decodes the ACARS application layer
// (CPDLC, ADS-C, MIAM, media-advisory, ...) embedded in an Aero ACARS message.
#pragma once

#include <string>

struct AcarsAppResult
{
    bool   decoded = false;  // an application payload was recognized
    std::string text;        // human-readable formatted decode
    bool   hasPos = false;   // a position was extracted (ADS-C)
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;          // altitude in feet
    std::string icaoHex;     // 6-hex ICAO 24-bit address (ADS-C airframe id), if present
    std::string flightId;    // flight/callsign (ADS-C flight id), if present
};

// Decode the application inside an ACARS message given its label + text.
// `downlink` selects the message direction (air->ground when true).
// Returns decoded=false when there is no recognizable embedded application.
AcarsAppResult decodeAcarsApps(const std::string& label, const std::string& text,
                               bool downlink);
