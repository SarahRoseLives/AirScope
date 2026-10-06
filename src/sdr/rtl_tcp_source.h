// RTL-TCP network source: connects to a remote rtl_tcp server and streams
// raw 8-bit unsigned interleaved IQ, identical to native RTL-SDR USB format.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtlTcpSource : public SdrSource
{
public:
    RtlTcpSource() = default;
    ~RtlTcpSource() override;

    std::vector<SdrDeviceInfo> listDevices() override { return {}; }

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override;

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    void setHost(const std::string& h) { host_ = h; }
    void setPort(uint16_t p) { port_ = p; }
    std::string host() const { return host_; }
    uint16_t port() const { return port_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

private:
    void recvLoop();
    bool sendCmd(uint8_t opcode, const void* data, int dataLen);
    bool recvAll(void* buf, int len);

    uintptr_t sock_ = ~(uintptr_t)0;
    std::mutex sockMtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    std::string host_ = "127.0.0.1";
    uint16_t port_ = 1234;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 2.4e6;
    double gainDb_     = -1.0;
    double ppm_        = 0.0;
    bool   biasTee_    = false;

    // DC block (same one-pole filter as RtlSdrSource).
    float  dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float  dcRate_  = 2.0e-5f;

    std::vector<uint8_t> rbuf_;
    std::vector<float>   fbuf_;
};
