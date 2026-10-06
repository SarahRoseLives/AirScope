#include "decode/decoder.h"

#include "decode/acars/acars_decoder.h"
#include "decode/acars_apps.h"
#include "voice/am_voice.h"

#include <cmath>

// Per-channel DDC output rate / passband.
static double chanRate(int baud)
{
    if (baud == kAcarsBaud) return 12500.0;
    if (baud == kVoiceBaud) return 8000.0;
    return 48000.0;
}

static double chanBw(int baud)
{
    if (baud == kAcarsBaud) return 8000.0;  // ±4 kHz for the 1800 Hz MSK subcarrier
    if (baud == kVoiceBaud) return 6000.0;  // ±3 kHz voice
    return 6000.0;
}

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId, MessageLog* log, AircraftTable* acTable,
                 AudioOutput* audioSink, VoiceCallLog* voiceLog)
    : ddc_(subRate, chanFreqHz - subCenterHz, chanRate(baud), chanBw(baud)),
      log_(log),
      acTable_(acTable),
      audioSink_(audioSink),
      vcallLog_(voiceLog),
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
    else if (baud == kVoiceBaud)
    {
        am_ = std::make_unique<AmVoiceDecoder>(ddc_.outputRate());
        am_->setSink(audioSink_);
        am_->setVoiceLog(vcallLog_, channelId_, chanFreqHz_ / 1e6);
    }
}

Decoder::~Decoder() = default;

void Decoder::process(const double* iq, int nComplex)
{
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;

    if (!acars_ && !am_)
        return;

    int n = (int)(ddcOut_.size() / 2);
    if ((int)env_.size() < n)
        env_.resize(n);
    for (int i = 0; i < n; ++i)
    {
        double re = ddcOut_[(size_t)i * 2];
        double im = ddcOut_[(size_t)i * 2 + 1];
        env_[i] = (float)std::sqrt(re * re + im * im);
    }
    if (acars_)
        acars_->process(env_.data(), n);
    else
        am_->process(env_.data(), n);
}

void Decoder::setFreq(double chanFreqHz)
{
    chanFreqHz_ = chanFreqHz;
    ddc_.setOffset(chanFreqHz - subCenterHz_);
    if (am_)
        am_->setVoiceLog(vcallLog_, channelId_, chanFreqHz_ / 1e6);
}

bool Decoder::locked() const
{
    if (acars_) return acars_->locked();
    if (am_)    return am_->squelchOpen();
    return false;
}

double Decoder::ebno() const
{
    if (acars_) return acars_->levelDb();
    if (am_)    return am_->signalDb();
    return 0.0;
}

uint64_t Decoder::msgCount() const
{
    return acars_ ? acars_->msgCount() : msgCount_.load();
}

void Decoder::setMonitored(bool on)
{
    if (am_) am_->setMonitored(on);
}

bool Decoder::monitored() const
{
    return am_ ? am_->monitored() : false;
}

void Decoder::setRecording(bool on, const std::string& dir, RecordFormat fmt)
{
    if (am_) am_->setRecording(on, dir, fmt);
}

bool Decoder::recordingNow() const
{
    return am_ ? am_->recordingNow() : false;
}

const std::string& Decoder::recordingPath() const
{
    static const std::string empty;
    return am_ ? am_->recordingPath() : empty;
}

void Decoder::setSquelchDb(double d)
{
    if (am_) am_->setSquelchDb(d);
}

void Decoder::setVolume(float v)
{
    if (am_) am_->setVolume(v);
}

uint64_t Decoder::voiceSamples() const
{
    return am_ ? am_->samplesOut() : 0;
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
