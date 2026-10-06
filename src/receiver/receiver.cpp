#include "receiver/receiver.h"

#include "sdr/rtl_sdr_source.h"
#include "sdr/wav_file_source.h"
#ifdef HAS_AIRSPY
#include "sdr/airspy_source.h"
#endif
#ifdef HAS_SDRPLAY
#include "sdr/sdrplay_source.h"
#endif

#include <utility>

Receiver::~Receiver()
{
    if (devThread.joinable())
        devThread.join();
}

void Receiver::scanDevices()
{
    // If a scan is already running, don't block the caller (device enumeration
    // can take tens of seconds); just let the in-flight one finish.
    bool expected = false;
    if (!devicesScanning.compare_exchange_strong(expected, true))
        return;

    if (devThread.joinable())
        devThread.join(); // previous scan has finished; returns immediately

    devicesReady.store(false);
    Receiver* self = this;
    devThread = std::thread([self]() {
        std::vector<SdrDeviceInfo> list =
            self->src ? self->src->listDevices() : std::vector<SdrDeviceInfo>{};
        {
            std::lock_guard<std::mutex> lk(self->devMtx);
            self->devices = std::move(list);
        }
        self->devicesReady.store(true);
        self->devicesScanning.store(false);
    });
}

std::unique_ptr<SdrSource> makeSdrSource(int mode)
{
    switch (mode)
    {
    case kRxRtl:    return std::make_unique<RtlSdrSource>();
    case kRxWav:    return std::make_unique<WavFileSource>();
    case kRxAirspy:
#ifdef HAS_AIRSPY
        return std::make_unique<AirspySource>();
#else
        return nullptr;
#endif
    case kRxSdrplay:
#ifdef HAS_SDRPLAY
        return std::make_unique<SdrplaySource>();
#else
        return nullptr;
#endif
    case kRxDisabled: return nullptr;
    default:        return nullptr;
    }
}

const char* rxModeName(int mode)
{
    switch (mode)
    {
    case kRxRtl:     return "RTL-SDR";
    case kRxWav:     return "WAV file";
    case kRxAirspy:  return "Airspy";
    case kRxSdrplay: return "SDRplay";
    case kRxDisabled: return "Disabled";
    default:         return "?";
    }
}
