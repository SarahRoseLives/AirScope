#include "decode/vdl2/vdl2_manager.h"
#include "decode/vdl2/vdl2.h"
#include "decode/acars_apps.h"

#include <cstdlib>
#include <cstring>
#include <string>

void Vdl2Manager::onAcars(const vdl2_acars_t* m, void* user)
{
    auto* self = static_cast<Vdl2Manager*>(user);
    if (!self || !self->log_ || !m)
        return;

    DecodedMessage d;
    d.timeSec = m->timeSec;
    d.freqMHz = m->freqHz / 1e6;
    d.mode = m->mode;
    d.blockId = m->blockId;
    d.reg = m->reg;
    d.label = m->label;
    d.flight = m->flight;
    d.text = m->txt;
    d.downlink = m->downlink ? 1 : 0;
    d.hex.clear();
    d.decoded.clear();

    // Decode the embedded application (CPDLC / ADS-C / ...) when present, then
    // fall back to a plain-text position report that libacars does not handle.
    if (!d.label.empty() && !d.text.empty())
    {
        AcarsAppResult app = decodeAcarsApps(d.label, d.text, d.downlink != 0);
        if (app.decoded)
        {
            d.decoded = app.text;
            d.hasPos = app.hasPos;
            d.lat = app.lat;
            d.lon = app.lon;
            d.alt = app.alt;
            d.icao = app.icaoHex;
            if (!app.flightId.empty())
                d.flight = app.flightId;
        }
        if (!d.hasPos)
        {
            double la = 0.0, lo = 0.0;
            if (parseAcarsPosition(d.text, la, lo))
            {
                d.hasPos = true;
                d.lat = la;
                d.lon = lo;
            }
        }
    }
    if (!d.icao.empty())
        d.aesId = (uint32_t)std::strtoul(d.icao.c_str(), nullptr, 16);

    self->log_->addAndStore(d, 10500);
    if (self->acTable_)
        self->acTable_->update(d, d.timeSec);
    ++self->count_;
    ++self->chMsgs_[m->freqHz];
}

bool Vdl2Manager::start(double fs, double centerHz,
                        const std::vector<double>& freqsMHz, bool conjugate)
{
    std::vector<double> freqsHz;
    freqsHz.reserve(freqsMHz.size());
    for (double mhz : freqsMHz)
        freqsHz.push_back(mhz * 1e6);
    if (freqsHz.empty())
        return false;

    channelsMHz_ = freqsMHz;
    chMsgs_.clear();
    vdl2_set_acars_cb(&Vdl2Manager::onAcars, this);
    return vdl2_start(fs, centerHz, freqsHz.data(), (int)freqsHz.size(),
                      conjugate ? 1 : 0) == 0;
}

void Vdl2Manager::stop()
{
    vdl2_stop();
}

void Vdl2Manager::feed(const float* iq, int nComplex)
{
    vdl2_feed(iq, nComplex);
}

bool Vdl2Manager::running() const
{
    return vdl2_running() != 0;
}

uint64_t Vdl2Manager::frameCount() const
{
    return (uint64_t)vdl2_frame_count();
}

uint64_t Vdl2Manager::droppedCount() const
{
    return (uint64_t)vdl2_dropped_count();
}
