// RTL-SDR source backend using librtlsdr directly.
// Conversion / DC-blocker / gain-selection logic adapted from
// inmarsat-sniffer (GPL-3.0-or-later, CEMAXECUTER LLC).
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <thread>

struct rtlsdr_dev; // from <rtl-sdr.h>

class RtlSdrSource : public SdrSource
{
public:
    RtlSdrSource() = default;
    ~RtlSdrSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override;

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }
    // Frequency the hardware actually tuned to (read back from the device).
    double actualCenterFreq() const { return actualCenter_.load(); }

    // Tuner info (valid after start()).
    std::string tunerType() const { return tunerType_; }
    double tunerMaxFreq() const { return tunerMaxFreq_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

private:
    static void asyncTrampoline(unsigned char* buf, uint32_t len, void* ctx);
    void onAsync(unsigned char* buf, uint32_t len);
    void applyGain(); // pick nearest supported gain and apply

    rtlsdr_dev* dev_ = nullptr;
    std::thread readThread_;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 2.4e6;
    double gainDb_     = -1.0;   // <0 => auto/AGC
    double ppm_        = 0.0;
    bool   biasTee_    = false;

    std::atomic<bool>   dcBlock_{true};
    std::atomic<double> actualCenter_{0.0};

    std::string tunerType_ = "-";
    double      tunerMaxFreq_ = 0.0;

    // Gentle one-pole DC blocker: out = in - offset; offset += out * rate.
    // rate is derived from the sample rate for a low (~tens of Hz) corner so
    // the DC offset spike is removed without a visible notch on the display.
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_  = 2.0e-5f;

    std::vector<float> scratch_;
};
