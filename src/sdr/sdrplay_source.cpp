#include "sdr/sdrplay_source.h"

#include <sdrplay_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

std::mutex g_apiMtx;
bool g_opened = false;

// Call sdrplay_api_Open() exactly once for the process. The API is a
// singleton and its calls are not thread-safe, so all calls are serialized.
bool ensureOpen()
{
    std::lock_guard<std::mutex> lk(g_apiMtx);
    if (g_opened)
        return true;
    if (sdrplay_api_Open() != sdrplay_api_Success)
        return false;
    float ver = 0.0f;
    if (sdrplay_api_ApiVersion(&ver) == sdrplay_api_Success)
    {
        if (std::fabs(ver - SDRPLAY_API_VERSION) > 0.001f)
            std::fprintf(stderr, "[sdrplay] API version %.2f (built against %.2f)\n",
                         (double)ver, (double)SDRPLAY_API_VERSION);
    }
    g_opened = true;
    return true;
}

const char* hwName(unsigned char hwVer)
{
    switch (hwVer)
    {
    case SDRPLAY_RSP1_ID:    return "RSP1";
    case SDRPLAY_RSP1A_ID:   return "RSP1A";
    case SDRPLAY_RSP1B_ID:   return "RSP1B";
    case SDRPLAY_RSP2_ID:    return "RSP2";
    case SDRPLAY_RSPduo_ID:  return "RSPduo";
    case SDRPLAY_RSPdx_ID:   return "RSPdx";
    case SDRPLAY_RSPdxR2_ID: return "RSPdx R2";
    default:                 return "SDRplay";
    }
}

sdrplay_api_Bw_MHzT bwForRate(double fsHz)
{
    if (fsHz <= 0.25e6) return sdrplay_api_BW_0_200;
    if (fsHz <= 0.30e6) return sdrplay_api_BW_0_300;
    if (fsHz <= 0.60e6) return sdrplay_api_BW_0_600;
    if (fsHz <= 2.0e6)  return sdrplay_api_BW_1_536;
    if (fsHz <= 6.0e6)  return sdrplay_api_BW_6_000;
    return sdrplay_api_BW_8_000;
}

void streamACb(short* xi, short* xq, sdrplay_api_StreamCbParamsT*, unsigned int numSamples,
               unsigned int, void* cbContext)
{
    static_cast<SdrplaySource*>(cbContext)->handleRx(xi, xq, (int)numSamples);
}

void eventCb(sdrplay_api_EventT, sdrplay_api_TunerSelectT, sdrplay_api_EventParamsT*, void*)
{
}

} // namespace

SdrplaySource::SdrplaySource() = default;

SdrplaySource::~SdrplaySource()
{
    stop();
}

std::vector<SdrDeviceInfo> SdrplaySource::listDevices()
{
    std::vector<SdrDeviceInfo> out;
    if (!ensureOpen())
        return out;

    std::lock_guard<std::mutex> lk(g_apiMtx);
    sdrplay_api_LockDeviceApi();
    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES]{};
    unsigned int n = 0;
    if (sdrplay_api_GetDevices(devs, &n, SDRPLAY_MAX_DEVICES) == sdrplay_api_Success)
    {
        for (unsigned int i = 0; i < n; ++i)
        {
            if (!devs[i].valid)
                continue;
            SdrDeviceInfo info;
            info.index = (int)out.size();
            info.name = hwName(devs[i].hwVer);
            info.serial = devs[i].SerNo;
            out.push_back(info);
        }
    }
    sdrplay_api_UnlockDeviceApi();
    return out;
}

bool SdrplaySource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    if (!ensureOpen())
    {
        err = "SDRplay API open failed (is the sdrplay service running?)";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_apiMtx);
        sdrplay_api_LockDeviceApi();
        sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES]{};
        unsigned int n = 0;
        sdrplay_api_ErrT r = sdrplay_api_GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
        if (r != sdrplay_api_Success)
        {
            sdrplay_api_UnlockDeviceApi();
            err = "SDRplay GetDevices failed: " + std::string(sdrplay_api_GetErrorString(r));
            return false;
        }
        int validCount = 0;
        sdrplay_api_DeviceT* pick = nullptr;
        for (unsigned int i = 0; i < n; ++i)
        {
            if (!devs[i].valid)
                continue;
            if (validCount == deviceIndex) { pick = &devs[i]; break; }
            ++validCount;
        }
        if (!pick)
        {
            sdrplay_api_UnlockDeviceApi();
            err = "No SDRplay device at index " + std::to_string(deviceIndex);
            return false;
        }

        dev_ = new sdrplay_api_DeviceT(*pick);
        hwVer_ = pick->hwVer;
        dev_->tuner = sdrplay_api_Tuner_A;
        sdrplay_api_DisableHeartbeat();
        r = sdrplay_api_SelectDevice(dev_);
        sdrplay_api_UnlockDeviceApi();
        if (r != sdrplay_api_Success)
        {
            delete dev_;
            dev_ = nullptr;
            err = "SDRplay SelectDevice failed: " + std::string(sdrplay_api_GetErrorString(r)) +
                  " (device busy?)";
            return false;
        }

        r = sdrplay_api_GetDeviceParams(dev_->dev, &params_);
        if (r != sdrplay_api_Success || !params_)
        {
            sdrplay_api_ReleaseDevice(dev_);
            delete dev_;
            dev_ = nullptr;
            err = "SDRplay GetDeviceParams failed";
            return false;
        }

        params_->devParams->fsFreq.fsHz = sampleRate_;
        params_->devParams->ppm = ppm_;
        params_->rxChannelA->tunerParams.rfFreq.rfHz = centerFreq_;
        params_->rxChannelA->tunerParams.bwType = bwForRate(sampleRate_);
        params_->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
        params_->rxChannelA->ctrlParams.decimation.enable = 0;
        params_->rxChannelA->ctrlParams.decimation.decimationFactor = 1;
        params_->rxChannelA->ctrlParams.dcOffset.DCenable = 1;
        params_->rxChannelA->ctrlParams.dcOffset.IQenable = 1;
        params_->rxChannelA->rsp1aTunerParams.biasTEnable = biasTee_ ? 1 : 0;
        applyGain();

        sdrplay_api_CallbackFnsT fns{};
        fns.StreamACbFn = &streamACb;
        fns.StreamBCbFn = nullptr;
        fns.EventCbFn = &eventCb;

        cb_ = std::move(cb);
        dcOffRe_ = dcOffIm_ = 0.0f;
        dcRate_ = (float)(50.0 / sampleRate_);

        r = sdrplay_api_Init(dev_->dev, &fns, this);
        if (r != sdrplay_api_Success)
        {
            sdrplay_api_ReleaseDevice(dev_);
            delete dev_;
            dev_ = nullptr;
            params_ = nullptr;
            cb_ = nullptr;
            err = "SDRplay Init failed: " + std::string(sdrplay_api_GetErrorString(r));
            return false;
        }
    }

    running_.store(true);
    return true;
}

void SdrplaySource::stop()
{
    if (dev_)
    {
        std::lock_guard<std::mutex> lk(g_apiMtx);
        if (running_.load())
            sdrplay_api_Uninit(dev_->dev);
        sdrplay_api_ReleaseDevice(dev_);
        delete dev_;
        dev_ = nullptr;
        params_ = nullptr;
    }
    running_.store(false);
    cb_ = nullptr;
}

void SdrplaySource::handleRx(const short* xi, const short* xq, int n)
{
    if (!running_.load() || !cb_ || n <= 0)
        return;

    if ((int)scratch_.size() < n * 2)
        scratch_.resize((size_t)n * 2);
    float* out = scratch_.data();

    if (dcBlock_.load())
    {
        float offRe = dcOffRe_, offIm = dcOffIm_;
        const float rate = dcRate_;
        for (int i = 0; i < n; ++i)
        {
            float re = xi[i] * (1.0f / 32768.0f);
            float im = xq[i] * (1.0f / 32768.0f);
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
        for (int i = 0; i < n; ++i)
        {
            out[i * 2] = xi[i] * (1.0f / 32768.0f);
            out[i * 2 + 1] = xq[i] * (1.0f / 32768.0f);
        }
    }

    cb_(out, n);
}

void SdrplaySource::applyGain()
{
    if (!params_)
        return;
    sdrplay_api_GainT& g = params_->rxChannelA->tunerParams.gain;
    if (gainDb_ < 0.0)
    {
        params_->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
        params_->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;
    }
    else
    {
        params_->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        // More gain => lower reduction. gRdB valid range ~20..59.
        int gr = (int)std::lround(59.0 - gainDb_);
        g.gRdB = std::clamp(gr, (int)sdrplay_api_NORMAL_MIN_GR, 59);
        g.LNAstate = 0;
    }
}

void SdrplaySource::applyTune()
{
    if (!params_)
        return;
    params_->rxChannelA->tunerParams.rfFreq.rfHz = centerFreq_ * (1.0 + ppm_ / 1e6);
    std::lock_guard<std::mutex> lk(g_apiMtx);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
    if (ppm_ != 0.0)
    {
        params_->devParams->ppm = ppm_;
        sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                           sdrplay_api_Update_Dev_Ppm, sdrplay_api_Update_Ext1_None);
    }
}

void SdrplaySource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (running_.load())
        applyTune();
}

void SdrplaySource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (running_.load())
        applyTune();
}

void SdrplaySource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (!running_.load() || !params_)
        return;
    params_->devParams->fsFreq.fsHz = hz;
    params_->rxChannelA->tunerParams.bwType = bwForRate(hz);
    std::lock_guard<std::mutex> lk(g_apiMtx);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Dev_Fs, sdrplay_api_Update_Ext1_None);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setGain(double db)
{
    gainDb_ = db;
    if (!running_.load() || !params_)
        return;
    applyGain();
    std::lock_guard<std::mutex> lk(g_apiMtx);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (!running_.load() || !params_)
        return;
    params_->rxChannelA->rsp1aTunerParams.biasTEnable = on ? 1 : 0;
    std::lock_guard<std::mutex> lk(g_apiMtx);
    sdrplay_api_Update(dev_->dev, sdrplay_api_Tuner_A,
                       sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
}
