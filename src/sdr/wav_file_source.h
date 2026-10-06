// WAV file IQ source. Plays back 8-bit (unsigned) or 16-bit (signed) PCM WAV
// captures as complex IQ, feeding the same ring -> FFT -> waterfall path as a
// live SDR. Stereo = I (ch0) / Q (ch1); mono = real signal (Q = 0).
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <thread>

class WavFileSource : public SdrSource
{
public:
    WavFileSource() = default;
    ~WavFileSource() override;

    void setPath(const std::string& path) { path_ = path; }
    void setLoop(bool on) { loop_.store(on); }
    bool loop() const { return loop_.load(); }

    std::vector<SdrDeviceInfo> listDevices() override { return {}; }

    void setCenterFreq(double hz) override { centerFreq_ = hz; }
    void setSampleRate(double hz) override { sampleRate_ = hz; }
    void setGain(double) override {}
    void setBiasTee(bool) override {}
    void setPpm(double) override {}

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    int    channels() const { return channels_; }
    int    bits() const { return bits_; }
    double progress() const { return progress_.load(); } // 0..1

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

private:
    void playLoop();

    std::string path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> loop_{true};
    std::atomic<double> progress_{0.0};
    SdrSampleCb cb_;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 2.4e6;

    // Parsed WAV layout.
    int      channels_  = 2;
    int      bits_      = 8;
    uint64_t dataOffset_ = 0;
    uint64_t dataBytes_  = 0;
};
