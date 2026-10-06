// 1090 MHz Mode S / ADS-B demodulator and message decoder.
//
// Ported to C++ from goadsb (github.com/SarahRoseLives/goadsb, MIT), itself a
// port of Malcolm Robb's dump1090 demodulator/decoder. Operates on magnitude
// samples at 2.4 MS/s.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adsb
{

constexpr int kLongMsgBits  = 112;
constexpr int kLongMsgBytes = 14;
constexpr int kShortMsgBits = 56;
constexpr int kShortMsgBytes = 7;

// A demodulated-but-not-yet-decoded Mode S frame.
struct RawMessage
{
    uint8_t data[kLongMsgBytes] = {0};
    int     bits = 0;
    int     score = 0;
    double  sigLevel = 0.0;
};

// A decoded Mode S / ADS-B message.
struct Decoded
{
    uint8_t  df = 0;
    uint8_t  ca = 0;
    uint32_t icao = 0;
    bool     crc = false;
    int      bits = 0;
    double   sigLevel = 0.0;

    // Extended squitter (DF17/18) fields.
    uint8_t  meType = 0;
    uint8_t  meSubType = 0;
    int32_t  altitude = 0;
    double   lat = 0.0;       // raw CPR fraction (0..1), not degrees
    double   lon = 0.0;
    bool     cprEven = false;
    double   speed = 0.0;
    double   heading = 0.0;
    int32_t  vertRate = 0;
    std::string callsign;
    uint16_t squawk = 0;

    // Corrected frame bytes (CRC-fixed) for output.
    uint8_t  raw[kLongMsgBytes] = {0};
    int      rawBytes = 0;

    std::string hex() const;
};

// One-time init: builds CRC tables and the magnitude lookup table.
// fixBits: 0 = no correction, 1 = 1-bit, 2 = 2-bit.
void init(int fixBits = 1);

// Rebuild the CRC error-correction tables (safe to call at runtime).
void setFixBits(int fixBits);

// Set the 256x256 magnitude lookup table (always available; built by init()).
void buildMagLut();
extern uint16_t g_magLut[256][256];

// Scan magnitude samples for Mode S frames. `mag` must contain at least
// `mlen + 32` valid samples (the demod reads a little past `mlen`, mirroring
// dump1090's overlap handling). All five sample phases are tried, as in
// dump1090-fa, for maximum sensitivity.
void demodulate2400(const uint16_t* mag, int mlen, std::vector<RawMessage>& out);

// Decode a raw frame. For DF17/18 the extended-squitter payload is parsed and
// CPR fields are left as raw fractions for the tracker.
void decode(const RawMessage& raw, Decoded& out);

} // namespace adsb
