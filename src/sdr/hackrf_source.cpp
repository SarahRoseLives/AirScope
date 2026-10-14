#include "sdr/hackrf_source.h"

#include <libhackrf/hackrf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
void ensureHackRfInit()
{
    static std::once_flag once;
    std::call_once(once, []() {
        if (hackrf_init() != HACKRF_SUCCESS)
            std::fprintf(stderr, "[hackrf] hackrf_init() failed\n");
    });
}

int rxTrampoline(hackrf_transfer* transfer)
{
    auto* self = static_cast<HackRfSource*>(transfer->rx_ctx);
    return self->handleRx(transfer->buffer, transfer->valid_length);
}
} // namespace

HackRfSource::HackRfSource()
{
    ensureHackRfInit();
}

HackRfSource::~HackRfSource()
{
    stop();
}

std::vector<SdrDeviceInfo> HackRfSource::listDevices()
{
    ensureHackRfInit();
    std::vector<SdrDeviceInfo> out;
    hackrf_device_list_t* list = hackrf_device_list();
    if (!list)
        return out;
    for (int i = 0; i < list->devicecount; ++i)
    {
        SdrDeviceInfo info;
        info.index = i;
        info.name = "HackRF";
        if (list->serial_numbers && list->serial_numbers[i])
            info.serial = list->serial_numbers[i];
        out.push_back(info);
    }
    hackrf_device_list_free(list);
    return out;
}

bool HackRfSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    ensureHackRfInit();

    hackrf_device_list_t* list = hackrf_device_list();
    if (!list || list->devicecount <= 0)
    {
        if (list) hackrf_device_list_free(list);
        err = "No HackRF device found";
        return false;
    }
    if (deviceIndex < 0 || deviceIndex >= list->devicecount)
        deviceIndex = 0;

    int r = hackrf_device_list_open(list, deviceIndex, &dev_);
    hackrf_device_list_free(list);
    if (r != HACKRF_SUCCESS || !dev_)
    {
        err = "HackRF open failed: " + std::string(hackrf_error_name((hackrf_error)r));
        dev_ = nullptr;
        return false;
    }

    hackrf_set_sample_rate(dev_, sampleRate_);
    hackrf_set_freq(dev_, (uint64_t)std::llround(centerFreq_ * (1.0 + ppm_ / 1e6)));
    hackrf_set_lna_gain(dev_, (uint32_t)lnaGain_);
    hackrf_set_vga_gain(dev_, (uint32_t)vgaGain_);
    hackrf_set_amp_enable(dev_, amp_ ? 1 : 0);
    hackrf_set_antenna_enable(dev_, biasTee_ ? 1 : 0);

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);

    r = hackrf_start_rx(dev_, &rxTrampoline, this);
    if (r != HACKRF_SUCCESS)
    {
        err = "HackRF start_rx failed: " + std::string(hackrf_error_name((hackrf_error)r));
        hackrf_close(dev_);
        dev_ = nullptr;
        cb_ = nullptr;
        return false;
    }
    running_.store(true);
    return true;
}

void HackRfSource::stop()
{
    if (dev_)
    {
        hackrf_stop_rx(dev_);
        hackrf_close(dev_);
        dev_ = nullptr;
    }
    running_.store(false);
    cb_ = nullptr;
}

int HackRfSource::handleRx(const uint8_t* buf, int len)
{
    if (!running_.load() || !cb_ || len <= 0)
        return 0;

    int n = len / 2; // complex samples
    if ((int)scratch_.size() < n * 2)
        scratch_.resize((size_t)n * 2);
    float* out = scratch_.data();
    const int8_t* s = (const int8_t*)buf;

    if (dcBlock_.load())
    {
        float offRe = dcOffRe_, offIm = dcOffIm_;
        const float rate = dcRate_;
        for (int i = 0; i < n; ++i)
        {
            float re = s[i * 2] * (1.0f / 128.0f);
            float im = s[i * 2 + 1] * (1.0f / 128.0f);
            float ore = re - offRe;
            offRe += ore * rate;
            float oim = im - offIm;
            offIm += oim * rate;
            out[i * 2] = ore;
            out[i * 2 + 1] = oim;
        }
        dcOffRe_ = offRe;
        dcOffIm_ = offIm;
    }
    else
    {
        for (int i = 0; i < n * 2; ++i)
            out[i] = s[i] * (1.0f / 128.0f);
    }

    cb_(out, n);
    return 0;
}

void HackRfSource::applyTune()
{
    if (dev_)
        hackrf_set_freq(dev_, (uint64_t)std::llround(centerFreq_ * (1.0 + ppm_ / 1e6)));
}

void HackRfSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    applyTune();
}

void HackRfSource::setPpm(double ppm)
{
    ppm_ = ppm;
    applyTune();
}

void HackRfSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (dev_)
        hackrf_set_sample_rate(dev_, hz);
}

void HackRfSource::setGain(double db)
{
    // Generic single-knob fallback: map onto the IF (LNA) stage.
    setLnaGain((int)std::lround(db));
}

void HackRfSource::setLnaGain(int val)
{
    val = std::clamp(val, 0, 40);
    val = (val / 8) * 8; // 8 dB steps
    lnaGain_ = val;
    if (dev_)
        hackrf_set_lna_gain(dev_, (uint32_t)val);
}

void HackRfSource::setVgaGain(int val)
{
    val = std::clamp(val, 0, 62);
    val = (val / 2) * 2; // 2 dB steps
    vgaGain_ = val;
    if (dev_)
        hackrf_set_vga_gain(dev_, (uint32_t)val);
}

void HackRfSource::setAmp(bool on)
{
    amp_ = on;
    if (dev_)
        hackrf_set_amp_enable(dev_, on ? 1 : 0);
}

void HackRfSource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (dev_)
        hackrf_set_antenna_enable(dev_, on ? 1 : 0);
}
