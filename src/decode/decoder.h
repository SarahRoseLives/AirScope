// A single channel decoder.
//
// Phase 1 placeholder: the historical Inmarsat Aero demodulators have been
// removed. This class currently keeps only the per-channel DDC front end so the
// sub-band engine still runs; VHF ACARS (2400 bps MSK) and AM voice decoders
// are attached here in later phases.
#pragma once

#include "dsp/ddc.h"

#include <atomic>
#include <cstdint>
#include <vector>

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
            int channelId);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    bool   locked() const { return false; }
    double ebno() const { return 0.0; }
    double mse() const { return 0.0; }
    // Copy up to maxPairs constellation points (interleaved I,Q doubles into
    // iqOut, capacity >= 2*maxPairs). Returns the number of pairs written.
    int    getConstellation(double* iqOut, int maxPairs) const;

    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    baud() const { return baud_; }
    int    channelId() const { return channelId_; }
    uint64_t msgCount() const { return msgCount_.load(); }

private:
    Ddc ddc_;
    std::vector<double> ddcOut_;

    double subCenterHz_;
    double chanFreqHz_;
    int baud_;
    int channelId_;
    std::atomic<uint64_t> msgCount_{0};
};
