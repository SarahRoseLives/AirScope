// A single channel decoder. Currently supports VHF ACARS (2400 bps MSK);
// VHF AM voice and other modes attach here in later phases.
#pragma once

#include "dsp/ddc.h"
#include "decode/message_log.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class AcarsDecoder;
struct AcarsMsg;

// Special "baud" code selecting the VHF ACARS (2400 bps MSK) decoder.
static constexpr int kAcarsBaud = 2400;

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
            int channelId, MessageLog* log, AircraftTable* acTable);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    bool   locked() const;
    double ebno() const;
    double mse() const { return 0.0; }
    // Copy up to maxPairs constellation points (interleaved I,Q doubles into
    // iqOut, capacity >= 2*maxPairs). Returns the number of pairs written.
    int    getConstellation(double* iqOut, int maxPairs) const;

    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    baud() const { return baud_; }
    int    channelId() const { return channelId_; }
    uint64_t msgCount() const;

private:
    void onAcarsMsg(const AcarsMsg& m);

    Ddc ddc_;
    std::vector<double> ddcOut_;
    std::vector<float> env_;

    MessageLog* log_ = nullptr;
    AircraftTable* acTable_ = nullptr;

    double subCenterHz_;
    double chanFreqHz_;
    int baud_;
    int channelId_;

    std::unique_ptr<AcarsDecoder> acars_;
    std::atomic<uint64_t> msgCount_{0};
};
