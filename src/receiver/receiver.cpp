#include "receiver/receiver.h"

#include "sdr/rtl_sdr_source.h"
#include "sdr/wav_file_source.h"
#ifdef HAS_AIRSPY
#include "sdr/airspy_source.h"
#endif
#ifdef HAS_SDRPLAY
#include "sdr/sdrplay_source.h"
#endif

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
    default:         return "?";
    }
}
