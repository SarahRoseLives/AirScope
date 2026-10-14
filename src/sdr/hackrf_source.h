// HackRF One source backend using libhackrf directly.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct hackrf_device;

class HackRfSource : public SdrSource
{
public:
    HackRfSource();
    ~HackRfSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;      // maps to the IF (LNA) stage
    void setBiasTee(bool on) override;     // antenna port power (bias tee)
    void setPpm(double ppm) override;

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    // HackRF-specific gain stages.
    void setLnaGain(int val);   // IF gain, 0..40 dB in 8 dB steps
    void setVgaGain(int val);   // baseband gain, 0..62 dB in 2 dB steps
    void setAmp(bool on);       // front-end RF amplifier (~+11 dB)

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Called from the libhackrf RX trampoline. `len` is the byte count
    // (2 interleaved int8 samples per complex value).
    int handleRx(const uint8_t* buf, int len);

private:
    void applyTune();

    hackrf_device* dev_ = nullptr;
    std::atomic<bool> running_{false};
    SdrSampleCb    cb_;

    double centerFreq_ = 128.0e6;
    double sampleRate_ = 8.0e6;
    double ppm_ = 0.0;

    int  lnaGain_ = 16;   // 0..40 step 8
    int  vgaGain_ = 16;   // 0..62 step 2
    bool amp_ = false;
    bool biasTee_ = false;

    std::atomic<bool> dcBlock_{true};
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-6f;

    std::vector<float> scratch_;
};
