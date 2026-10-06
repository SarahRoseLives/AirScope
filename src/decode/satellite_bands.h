// Satellite channel band plan — known frequency allocations per satellite.
// Data from reference/inmarsat-sniffer/satellites.c (GPL-3.0).
#pragma once

#include <cstddef>
#include <cstdint>

enum class BandType : uint8_t {
    EGC    = 0,  // STD-C EGC
    Aero600  = 1,  // Aero 600 baud BPSK
    Aero1200 = 2,  // Aero 1200 baud BPSK
    Aero8400 = 3,  // Aero 8400 baud OQPSK (voice)
    Aero10500 = 4, // Aero-H+ 10500 baud OQPSK
};

struct BandEntry {
    double freqHz;    // centre frequency (Hz)
    BandType type;
    int channelId;
};

struct BandSatellite {
    const char* name;       // e.g. "I4-F3 AOR-W"
    const char* designator; // e.g. "4F3"
    double position;        // orbital position (negative = west)
    const BandEntry* channels;
    int numChannels;
};

// Bandwidth for visual rectangles (~ ±half of this from centre).
inline double bandWidthHz(BandType t) {
    switch (t) {
    case BandType::EGC:       return 5000.0;
    case BandType::Aero600:   return 3000.0;
    case BandType::Aero1200:  return 6000.0;
    case BandType::Aero8400:  return 15000.0;
    case BandType::Aero10500: return 22000.0;
    default: return 5000.0;
    }
}

static const BandEntry kChannels4F3[] = {
    {1537700000.0, BandType::EGC,         0},
    {1545020000.0, BandType::Aero600,     1},
    {1545050000.0, BandType::Aero600,     2},
    {1545060000.0, BandType::Aero600,     3},
    {1545065000.0, BandType::Aero600,    4},
    {1545080000.0, BandType::Aero600,     5},
    {1545085000.0, BandType::Aero600,    6},
    {1545090000.0, BandType::Aero600,     7},
    {1545100000.0, BandType::Aero600,     8},
    {1545110000.0, BandType::Aero600,     9},
    {1545170000.0, BandType::Aero600,    10},
    {1545175000.0, BandType::Aero600,   11},
    {1545205000.0, BandType::Aero600,   12},
    {1545075000.0, BandType::Aero1200,   13},
    {1546005000.0, BandType::Aero10500,  14},
    {1546020000.0, BandType::Aero10500,  15},
    {1546062500.0, BandType::Aero10500, 16},
    {1546077500.0, BandType::Aero10500, 17},
    {1542937500.0, BandType::Aero8400,   18},
    {1542942500.0, BandType::Aero8400,   19},
    {1542947500.0, BandType::Aero8400,   20},
    {1542952500.0, BandType::Aero8400,   21},
    {1542957500.0, BandType::Aero8400,   22},
    {1542977500.0, BandType::Aero8400,   23},
    {1542982500.0, BandType::Aero8400,   24},
    {1542987500.0, BandType::Aero8400,   25},
    {1542992500.0, BandType::Aero8400,   26},
};

static const BandEntry kChannels3F5[] = {
    {1541450000.0, BandType::EGC,         0},
    {1541760000.0, BandType::Aero600,     1},
    {1541770000.0, BandType::Aero600,     2},
    {1541790000.0, BandType::Aero600,     3},
    {1541815000.0, BandType::Aero600,    4},
    {1541937500.0, BandType::Aero10500,   5},
    {1541952500.0, BandType::Aero10500,  6},
    {1540870000.0, BandType::Aero8400,    7},
    {1540885000.0, BandType::Aero8400,    8},
    {1540900000.0, BandType::Aero8400,    9},
};

static const BandEntry kChannelsAF4[] = {
    {1539145000.0, BandType::EGC,         0},
    {1545445000.0, BandType::Aero600,     1},
    {1545530000.0, BandType::Aero600,     2},
    {1545545000.0, BandType::Aero600,     3},
    {1545560000.0, BandType::Aero600,    4},
    {1546125000.0, BandType::Aero10500,   5},
    {1546140000.0, BandType::Aero10500,  6},
    {1544112500.0, BandType::Aero8400,    7},
    {1544135000.0, BandType::Aero8400,    8},
    {1544157500.0, BandType::Aero8400,    9},
    {1544210000.0, BandType::Aero8400,   10},
    {1544230000.0, BandType::Aero8400,   11},
};

static const BandEntry kChannelsF1[] = {
    {1541765000.0, BandType::EGC,         0},
    {1540690000.0, BandType::Aero600,     1},
    {1540705000.0, BandType::Aero600,     2},
    {1540720000.0, BandType::Aero600,     3},
    {1540825000.0, BandType::Aero600,    4},
    {1540855000.0, BandType::Aero600,     5},
    {1540870000.0, BandType::Aero600,     6},
    {1540885000.0, BandType::Aero600,    7},
    {1540900000.0, BandType::Aero600,     8},
    {1540967500.0, BandType::Aero10500,   9},
    {1540982500.0, BandType::Aero10500,  10},
    {1539725000.0, BandType::Aero8400,   11},
    {1539755000.0, BandType::Aero8400,   12},
    {1539785000.0, BandType::Aero8400,   13},
    {1539845000.0, BandType::Aero8400,   14},
};

static const BandSatellite kBandSatellites[] = {
    {"I4-F3  AOR-W", "4F3",  -98.0, kChannels4F3, (int)(sizeof(kChannels4F3)/sizeof(kChannels4F3[0]))},
    {"I3-F5  AOR-E", "3F5",  -54.0, kChannels3F5, (int)(sizeof(kChannels3F5)/sizeof(kChannels3F5[0]))},
    {"Alphasat  EMEA", "AF4", 25.0, kChannelsAF4, (int)(sizeof(kChannelsAF4)/sizeof(kChannelsAF4[0]))},
    {"I-6 F1  IOE", "F1",   83.5, kChannelsF1, (int)(sizeof(kChannelsF1)/sizeof(kChannelsF1[0]))},
};
constexpr int kNumBandSats = (int)(sizeof(kBandSatellites)/sizeof(kBandSatellites[0]));
