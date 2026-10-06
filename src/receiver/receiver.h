// A Receiver couples one SdrSource with its own spectrum view and decode
// channel bank. AirScope runs several receivers concurrently (e.g. an Airspy
// for voice, an RTL for 1090 ADS-B, an SDRplay for ACARS). Only receivers with
// showSpectrum get an interactive spectrum/waterfall; ADS-B does not.
#pragma once

#include "dsp/iq_ring.h"
#include "dsp/jfft.h"
#include "gui/waterfall.h"
#include "sdr/sdr_source.h"
#include "decode/decoder_manager.h"
#include "decode/band_plan.h"

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Receiver source modes.
constexpr int kRxRtl     = 0;
constexpr int kRxWav     = 1;
constexpr int kRxAirspy  = 2;
constexpr int kRxSdrplay = 3;

struct SpectrumView
{
    IqRing ring{1u << 21};
    JFFT   fft;
    Waterfall waterfall;
    std::vector<std::complex<double>> iq;
    std::vector<double> window;
    std::vector<float> inst;
    std::vector<float> avg;
    std::vector<float> sortbuf;
    std::vector<float> freqMHz;
    int   curN = 0;
    float rmsDbfs = -120.0f;
    float frameDbMin = 0.0f, frameDbMax = -120.0f;
    double viewXminMHz = 0.0, viewXmaxMHz = 0.0;
    bool   resetView = true;
    float  specLeftInset = 0.0f, specRightInset = 0.0f;
    bool   fftSkip = false;
};

// Create the concrete SdrSource for a receiver mode, or nullptr if that source
// is not compiled in / available.
std::unique_ptr<SdrSource> makeSdrSource(int mode);

// Human-readable source-mode label.
const char* rxModeName(int mode);

struct Receiver
{
    int mode = kRxRtl;
    std::unique_ptr<SdrSource> src;

    SpectrumView   view;
    DecoderManager decoders;

    // ---- per-receiver tuner configuration ----
    int    deviceIndex = 0;
    double centerMHz   = 128.0;
    int    rateIdx     = 4;        // RTL sample-rate index (kRates)
    bool   autoGain    = true;
    float  gainDb      = 40.0f;
    bool   biasTee     = false;
    float  ppm         = 0.0f;

    // Airspy
    int  apRateIdx = 1;   // kAirspyRates index (1 = 3 MHz)
    int  apGainMode = 0;  // 0=Sensitivity 1=Linear 2=Free
    int  apSense = 10, apLinear = 10;
    int  apLna = 8, apMixer = 8, apVga = 4;
    bool apLnaAgc = false, apMixerAgc = false, apBias = false;

    // SDRplay
    int  spRateIdx = 2;   // kSdrplayRates index (2 = 6 MHz)
    bool spAgc = true;
    int  spGRdB = 40;
    bool spBias = false;

    // WAV
    char wavPath[512] = "";
    bool wavLoop = true;

    // ---- role ----
    bool showSpectrum = true;   // voice/ACARS want a spectrum; ADS-B doesn't

    // ---- band plan ----
    bool     showBandPlan = false;
    int      bandPlanIdx = 0;
    BandPlan bandPlanLoaded;

    // ---- runtime ----
    std::string status;
    std::vector<SdrDeviceInfo> devices;
    std::vector<std::pair<double,int>> savedDecoders; // freqMHz, baud
    uint64_t lastFeedCount = 0;
    double   lastConfiguredFs = 0.0;

    bool running() const { return src && src->running(); }
    double sampleRate() const { return src ? src->sampleRate() : 0.0; }
    double centerFreq() const { return src ? src->centerFreq() : 0.0; }
};

// SDRplay sample rates (MHz) and labels.
constexpr double kSdrplayRates[] = {2.0e6, 3.0e6, 4.0e6, 5.0e6, 6.0e6, 7.0e6, 8.0e6};
constexpr const char* kSdrplayRateLabels[] = {"2.0", "3.0", "4.0", "5.0", "6.0", "7.0", "8.0"};
constexpr int kSdrplayNumRates = (int)(sizeof(kSdrplayRates) / sizeof(kSdrplayRates[0]));
