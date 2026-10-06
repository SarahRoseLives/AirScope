#include "voice/am_voice.h"

#include "audio/audio_output.h"
#include "decode/message_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

static constexpr double kOutRate = 8000.0;

AmVoiceDecoder::AmVoiceDecoder(double sampleRate)
    : inRate_(sampleRate > 1000.0 ? sampleRate : 8000.0)
{
    // One-pole DC blocker (~100 Hz corner) to strip the AM carrier average.
    dcR_ = 2.0 * M_PI * 100.0 / inRate_;
    if (dcR_ > 0.5) dcR_ = 0.5;
}

AmVoiceDecoder::~AmVoiceDecoder()
{
    closeRecord();
}

void AmVoiceDecoder::setRecording(bool on, const std::string& dir, RecordFormat fmt)
{
    recEnabled_ = on;
    if (!dir.empty()) recDir_ = dir;
    recFmt_ = fmt;
    if (!on && recOpen_)
        closeRecord();
}

void AmVoiceDecoder::process(const float* env, int n)
{
    int nout = 0;

    for (int i = 0; i < n; ++i)
    {
        double x = env[i];

        // DC block.
        dcState_ += (x - dcState_) * dcR_;
        double y = x - dcState_;

        // Squelch keys off the carrier / IF envelope level (mean amplitude),
        // NOT the demodulated audio: keying on audio would close the squelch
        // on any pause in modulation and chop the audio up.
        sigSq_ += x;
        sigN_++;

        // Resample to 8 kHz (fractional phase accumulator).
        phase_ += kOutRate;
        if (phase_ < inRate_)
            continue;
        phase_ -= inRate_;

        // Update squelch / level roughly every 20 ms.
        if (sigN_ >= 160)
        {
            double lvl = sigSq_ / (double)sigN_;
            sigDb_ = 20.0 * std::log10(lvl + 1e-9);
            sigSq_ = 0.0;
            sigN_ = 0;

            // Manual threshold squelch with a little hysteresis. A very low
            // threshold (the default) keeps the channel permanently open,
            // which is what you want for continuous broadcasts.
            if (sigDb_ > squelchDb_ + 2.0)
            {
                sqlOpen_ = true;
                sqlHold_ = 30; // ~0.6 s hang
            }
            else if (sigDb_ < squelchDb_ - 2.0)
            {
                if (sqlHold_ > 0) --sqlHold_;
                if (sqlHold_ == 0) sqlOpen_ = false;
            }
        }

        // Simple peak AGC to keep loudness consistent, normalised near full
        // scale, then scaled by the user volume.
        double a = std::fabs(y);
        if (a > agcPeak_) agcPeak_ = a;
        else              agcPeak_ *= 0.99995;
        if (agcPeak_ < 1e-5) agcPeak_ = 1e-5;
        double gain = 0.9 / agcPeak_;
        if (gain > 500.0) gain = 500.0;
        double s = y * gain * (double)volume_;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;

        pcm_[nout++] = (int16_t)std::lround(s * 32000.0);
        if (nout >= (int)(sizeof(pcm_) / sizeof(pcm_[0])))
        {
            // Flush a full buffer mid-call.
            if (sqlOpen_ && monitored_ && sink_)
                sink_->push(pcm_, nout);
            if (sqlOpen_ && recEnabled_ && recOpen_)
                rec_.write(pcm_, nout);
            nout = 0;
        }
    }

    if (nout <= 0)
        return;

    if (sqlOpen_)
    {
        if (monitored_ && sink_)
            sink_->push(pcm_, nout);
        if (recEnabled_)
        {
            if (!recOpen_)
                openRecord();
            rec_.write(pcm_, nout);
        }
        recHold_ = 0;
    }
    else if (recEnabled_ && recOpen_)
    {
        recHold_ += nout;
        if (recHold_ > (int)kOutRate) // ~1 s of silence ends the call
            closeRecord();
    }
}

void AmVoiceDecoder::openRecord()
{
    std::error_code ec;
    std::filesystem::create_directories(recDir_, ec);

    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

    char name[320];
    std::snprintf(name, sizeof(name), "%s/%.4fMHz_ch%d_%s%s",
                  recDir_.c_str(), freqMHz_, channelId_, ts,
                  recFmt_ == RecordFormat::OGG ? ".ogg" : ".wav");

    rec_.setFormat(recFmt_);
    if (!rec_.open(name, (int)kOutRate, 1))
        return;
    recOpen_ = true;
    recPath_ = name;
    recStart_ = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;

    if (vlog_)
        vlog_->add({recStart_, 0.0, freqMHz_, channelId_, 0, "", name, true});
}

void AmVoiceDecoder::closeRecord()
{
    if (!recOpen_)
        return;
    rec_.close();
    double now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
    if (vlog_)
        vlog_->updateEnd(channelId_, now - recStart_, recPath_);
    recOpen_ = false;
}
