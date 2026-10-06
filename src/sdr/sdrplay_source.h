// SDRplay source backend using the SDRplay API (v3.x).
// Supports RSP1A / RSP1B / RSP2 / RSPduo / RSPdx single-tuner receive.
#pragma once

#include "sdr/sdr_source.h"

#include <sdrplay_api.h>

#include <atomic>
#include <string>
#include <vector>

class SdrplaySource : public SdrSource
{
public:
    SdrplaySource();
    ~SdrplaySource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;      // <0 => AGC
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override;

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Called from the SDRplay stream callback.
    void handleRx(const short* xi, const short* xq, int nSamples);

private:
    void applyGain();
    void applyTune();

    sdrplay_api_DeviceT*      dev_ = nullptr;   // selected device
    sdrplay_api_DeviceParamsT* params_ = nullptr;

    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 128.0e6;
    double sampleRate_ = 2.0e6;
    double gainDb_     = -1.0;   // <0 => AGC
    double ppm_        = 0.0;
    bool   biasTee_    = false;

    std::atomic<bool> dcBlock_{true};
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_  = 2.0e-5f;

    std::vector<float> scratch_;
    int hwVer_ = 0;
};
