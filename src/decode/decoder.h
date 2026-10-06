// A single channel decoder: VHF ACARS (2400 bps MSK) or VHF AM voice.
#pragma once

#include "dsp/ddc.h"
#include "decode/message_log.h"
#include "voice/wav_writer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AcarsDecoder;
class AmVoiceDecoder;
class AudioOutput;
struct AcarsMsg;

// Special "baud" codes selecting non-MSK channel decoders.
static constexpr int kAcarsBaud = 2400;
static constexpr int kVoiceBaud = 1;

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
            int channelId, MessageLog* log, AircraftTable* acTable,
            AudioOutput* audioSink, VoiceCallLog* voiceLog);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    bool   locked() const;
    double ebno() const;
    double mse() const { return 0.0; }

    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    baud() const { return baud_; }
    int    channelId() const { return channelId_; }
    uint64_t msgCount() const;

    bool isVoice() const { return baud_ == kVoiceBaud; }
    void setMonitored(bool on);
    bool monitored() const;
    void setRecording(bool on, const std::string& dir, RecordFormat fmt = RecordFormat::WAV);
    bool recordingNow() const;
    const std::string& recordingPath() const;

private:
    void onAcarsMsg(const AcarsMsg& m);

    Ddc ddc_;
    std::vector<double> ddcOut_;
    std::vector<float> env_;

    MessageLog* log_ = nullptr;
    AircraftTable* acTable_ = nullptr;
    AudioOutput* audioSink_ = nullptr;
    VoiceCallLog* vcallLog_ = nullptr;

    double subCenterHz_;
    double chanFreqHz_;
    int baud_;
    int channelId_;

    std::unique_ptr<AcarsDecoder> acars_;
    std::unique_ptr<AmVoiceDecoder> am_;
    std::atomic<uint64_t> msgCount_{0};
};
