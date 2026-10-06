#ifndef MSKDEMODULATOR_H
#define MSKDEMODULATOR_H

/*
 * MskDemodulator -- JAERO's CONTINUOUS MSK demodulator for P-channel.
 * Ported from https://github.com/jontio/JAERO, Qt stripped to pure C++.
 * SPDX-License-Identifier: MIT (JAERO original license)
 */

#include "DSP.h"
#include <vector>
#include <complex>
#include <cstdint>
#include "fftrwrapper.h"
#include "hilbert_usb.h"

class CoarseFreqEstimate;

/* Callback: soft bits ready. Soft bit is unsigned char 0-255, 128 = threshold. */
typedef void (*msk_soft_bits_cb)(const short *bits, int num_bits, void *user);

class MskDemodulator
{
public:
    enum ScatterPointType{ SPT_constellation, SPT_phaseoffseterror, SPT_phaseoffsetest, SPT_None};
    struct Settings
    {
        int    coarsefreqest_fft_power;
        double freq_center;
        double lockingbw;
        double fb;
        double Fs;
        int    symbolspercycle;
        double signalthreshold;
        Settings()
            : coarsefreqest_fft_power(13),
              freq_center(0),    /* baseband IQ from our channelizer */
              lockingbw(2000),
              fb(600),
              Fs(48000),
              symbolspercycle(16),
              signalthreshold(0.5)
        {}
    };
    MskDemodulator();
    ~MskDemodulator();

    void setSettings(Settings settings);
    void invalidatesettings();
    void setAFC(bool state);
    void setSQL(bool state);
    void setCPUReduce(bool state);
    void setScatterPointType(ScatterPointType type);
    double getCurrentFreq();

    /* Feed int16 mono audio. */
    void feedAudio(const int16_t *samples, int num_samples, int sample_rate);
    /* Feed complex baseband IQ (already analytic — skips Hilbert). */
    void feedIQ(const double *iq_interleaved, int num_samples);

    /* Callback registration. */
    void setSoftBitsCallback(msk_soft_bits_cb cb, void *user);

    /* Signal status callback — fires when mse crosses signalthreshold. */
    typedef void (*signal_status_cb)(bool signal_good, void *user);
    void setSignalStatusCallback(signal_status_cb cb, void *user) {
        sigstat_cb = cb; sigstat_user = user; sigstat_last = true;
    }

    double getMSE() const { return mse; }
    double getEbNo() const { return ebnomeasure ? ebnomeasure->EbNo : 0; }

    /* Audio spectrum access for the web UI (real-sample FFT shows signals
     * at their actual audio frequency). */
    int get_audio_snapshot(double *out, int max_samples);
    /* Constellation points (post-matched-filter complex samples). Each
     * pair is one I/Q point. max_pairs is the capacity of `out` measured
     * in I/Q pairs (so `out` must be at least 2*max_pairs doubles).
     * Returns the number of pairs actually written. */
    int get_constellation_snapshot(double *out, int max_pairs);
    double getMixerCenterHz() { return mixer_center.GetFreqHz(); }
    double getFs() const { return Fs; }
    double getFreqCenterHz() const { return freq_center; }
    void setManualTune(double audio_hz);

private:
    msk_soft_bits_cb soft_bits_cb;
    void *soft_bits_user;
    signal_status_cb sigstat_cb;
    void *sigstat_user;
    bool sigstat_last;

    void CenterFreqChangedSlot(double freq_center);
    void FreqOffsetEstimateSlot(double freq_offset_est);
    void processAudio(const short *data, int numofsamples);

    WaveTable mixer_center;
    WaveTable mixer2;
    WaveTable st_osc;

    int spectrumnfft, bbnfft;

    std::vector<cpx_type> bbcycbuff;
    std::vector<cpx_type> bbtmpbuff;
    int bbcycbuff_ptr;

    std::vector<double> spectrumcycbuff;
    std::vector<double> spectrumtmpbuff;
    int spectrumcycbuff_ptr;

    CoarseFreqEstimate *coarsefreqestimate;

    double Fs;
    double freq_center;
    double lockingbw;
    double fb;
    double signalthreshold;

    double SamplesPerSymbol;

    FIR *matchedfilter_re;
    FIR *matchedfilter_im;

    AGC *agc;

    MSKEbNoMeasure *ebnomeasure;

    MovingAverage *pointmean;

    int lastindex;

    std::vector<cpx_type> pointbuff;
    int pointbuff_ptr;

    DiffDecode diffdecode;

    std::vector<short> RxDataBits;

    double mse;

    MovingAverage *msema;

    bool afc;

    bool sql;

    int scatterpointtype;

    std::vector<cpx_type> singlepointphasevector;

    BaceConverter bc;

    double ee;
    cpx_type pt_d;

    IIR st_iir_resonator;

    MovingAverage *marg;
    DelayThing<cpx_type> dt;

    DelayThing<cpx_type> delayedsmpl;
    Delay<double> delayt8;

    bool dcd;

    double correctionfactor;

    int coarseCounter;
    bool cpuReduce;

    Settings last_applied_settings;

    /* feedIQ mixing state (per-instance) */
    double feediq_phase;
    HilbertUSB feediq_usb;

    /* Per-instance — was static in JAERO (single-instance) */
    int freqest_countdown;
};

#endif // MSKDEMODULATOR_H
