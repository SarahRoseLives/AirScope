// VHF airband AM voice decoder: envelope detect + DC block + resample to
// 8 kHz + adaptive squelch, with live monitoring and per-call WAV/OGG recording.
#pragma once

#include <cstdint>
#include <string>

#include "voice/wav_writer.h"

class AudioOutput;
class VoiceCallLog;

class AmVoiceDecoder
{
public:
    explicit AmVoiceDecoder(double sampleRate);
    ~AmVoiceDecoder();

    // envelope[]: real AM envelope (|baseband|) at the DDC output rate.
    void process(const float* envelope, int n);

    void setSink(AudioOutput* s) { sink_ = s; }
    void setVoiceLog(VoiceCallLog* v, int channelId, double freqMHz)
    {
        vlog_ = v;
        channelId_ = channelId;
        freqMHz_ = freqMHz;
    }

    void setMonitored(bool on) { monitored_ = on; }
    bool monitored() const { return monitored_; }

    // Squelch threshold in dBFS (very low = always open).
    void setSquelchDb(double d) { squelchDb_ = d; }

    void setRecording(bool on, const std::string& dir, RecordFormat fmt);
    bool recordingNow() const { return recOpen_; }
    const std::string& recordingPath() const { return recPath_; }

    bool   squelchOpen() const { return sqlOpen_; }
    double signalDb() const { return sigDb_; }

private:
    void openRecord();
    void closeRecord();

    double inRate_;
    double dcState_ = 0.0;
    double dcR_;
    double phase_ = 0.0;

    double sigSq_ = 0.0;   // running power for squelch
    int    sigN_ = 0;
    double agcPeak_ = 1e-4;
    double squelchDb_ = -120.0;
    double noiseDb_ = -120.0;
    bool   sqlOpen_ = false;
    int    sqlHold_ = 0;   // samples until close
    double sigDb_ = -120.0;

    bool monitored_ = false;
    AudioOutput* sink_ = nullptr;

    bool recEnabled_ = false;
    bool recOpen_ = false;
    int  recHold_ = 0;
    std::string recDir_ = "recordings";
    RecordFormat recFmt_ = RecordFormat::WAV;
    WavWriter rec_;
    std::string recPath_;
    double recStart_ = 0.0;

    VoiceCallLog* vlog_ = nullptr;
    int    channelId_ = -1;
    double freqMHz_ = 0.0;

    int16_t pcm_[1024];
};
