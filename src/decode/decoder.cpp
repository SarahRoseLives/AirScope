#include "decode/decoder.h"

#include "decode/acars/acars_decoder.h"
#include "decode/acars_apps.h"

#include <cmath>

// Per-channel DDC output rate / passband.
static double chanRate(int baud)
{
    if (baud == kAcarsBaud)
        return 12500.0; // acarsdec INTRATE
    return 48000.0;
}

static double chanBw(int baud)
{
    if (baud == kAcarsBaud)
        return 8000.0; // ±4 kHz around the AM envelope's 1800 Hz subcarrier
    return 6000.0;
}

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId, MessageLog* log, AircraftTable* acTable)
    : ddc_(subRate, chanFreqHz - subCenterHz, chanRate(baud), chanBw(baud)),
      log_(log),
      acTable_(acTable),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId)
{
    ddcOut_.reserve(8192);
    if (baud == kAcarsBaud)
    {
        acars_ = std::make_unique<AcarsDecoder>(ddc_.outputRate());
        acars_->setCallback([this](const AcarsMsg& m) { onAcarsMsg(m); });
    }
}

Decoder::~Decoder() = default;

void Decoder::process(const double* iq, int nComplex)
{
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;

    if (acars_)
    {
        int n = (int)(ddcOut_.size() / 2);
        if ((int)env_.size() < n)
            env_.resize(n);
        for (int i = 0; i < n; ++i)
        {
            double re = ddcOut_[(size_t)i * 2];
            double im = ddcOut_[(size_t)i * 2 + 1];
            env_[i] = (float)std::sqrt(re * re + im * im);
        }
        acars_->process(env_.data(), n);
    }
}

void Decoder::setFreq(double chanFreqHz)
{
    chanFreqHz_ = chanFreqHz;
    ddc_.setOffset(chanFreqHz - subCenterHz_);
}

bool Decoder::locked() const
{
    return acars_ ? acars_->locked() : false;
}

double Decoder::ebno() const
{
    return acars_ ? acars_->levelDb() : 0.0;
}

uint64_t Decoder::msgCount() const
{
    return acars_ ? acars_->msgCount() : msgCount_.load();
}

void Decoder::onAcarsMsg(const AcarsMsg& a)
{
    DecodedMessage m;
    m.timeSec = a.timeSec;
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.mode = a.mode;
    m.reg = a.addr;
    m.label = a.label;
    m.blockId = a.bid;
    m.downlink = a.downlink ? 1 : 0;
    m.text = a.text;
    m.hex = a.hex;

    if (!a.label.empty() && !a.raw.empty())
    {
        AcarsAppResult app = decodeAcarsApps(a.label, a.raw, a.downlink);
        if (app.decoded)
        {
            m.decoded = app.text;
            m.hasPos = app.hasPos;
            m.lat = app.lat;
            m.lon = app.lon;
            m.alt = app.alt;
            m.icao = app.icaoHex;
            m.flight = app.flightId;
        }
    }

    if (log_)
        log_->addAndStore(m, baud_);
    if (acTable_ && m.aesId)
        acTable_->update(m, a.timeSec);
}
