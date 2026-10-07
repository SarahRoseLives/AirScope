// Per-receiver ADS-B engine: turns raw float IQ from any source rate into
// decoded aircraft, feeding the shared AircraftTable and optional Beast
// output. The demodulator is built for 2.4 MS/s, so the IQ is resampled to
// that rate first (Airspy/SDRplay cannot be tuned to exactly 2.4 MS/s).
#pragma once

#include "adsb/adsb_decoder.h"
#include "adsb/adsb_tracker.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class BeastWriter;
class AircraftTable;

class AdsbManager
{
public:
    struct Stats
    {
        uint64_t total = 0;   // frames demodulated
        uint64_t good = 0;    // CRC-valid
        uint64_t bad = 0;     // CRC-failed
    };

    AdsbManager();
    ~AdsbManager();

    void configure(double sampleRateHz, double centerHz);
    void start();
    void stop();
    bool running() const { return running_.load(); }

    // Called from the SDR read thread.
    void feed(const float* iq, int nComplex);

    void setFixBits(int n) { fixBits_ = n; adsb::setFixBits(n); }
    void setAircraftTable(AircraftTable* t) { acTable_ = t; }
    void setBeast(BeastWriter* b) { beast_ = b; }

    Stats stats() const;
    int aircraftCount();
    bool rateSupported() const { return rateOk_; }

private:
    void workerLoop();

    // Convert one normalised IQ sample to a magnitude LUT entry and append it
    // to the ring. Caller must hold mtx_.
    void pushSample(float iv, float qv);

    // ---- configuration ----
    double sampleRate_ = 2.4e6;
    double centerHz_ = 1090.0e6;
    bool   rateOk_ = false;
    int    fixBits_ = 1;

    // ---- magnitude ring (locked) ----
    static constexpr size_t kRingCap = 16 * 16384;
    static constexpr size_t kOverlap = 2048;
    std::mutex mtx_;
    std::vector<uint16_t> ring_;
    size_t write_ = 0;
    size_t length_ = 0;

    // ---- IQ resampler (source rate -> 2.4 MS/s) ----
    std::vector<float> inBuf_;   // interleaved IQ pending resample
    double inPhase_ = 0.0;       // fractional input index of next output sample

    // ---- worker ----
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_{0}, good_{0}, bad_{0};

    adsb::Tracker tracker_;
    AircraftTable* acTable_ = nullptr;
    BeastWriter*   beast_ = nullptr;
};
