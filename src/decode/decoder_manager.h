// Owns the active channel decoders, grouped into sub-bands. A sub-band
// decimates the wideband stream ONCE (shared front-end) to a moderate IF; its
// decoders then run cheap per-channel DDCs from that IF. Decoders far apart in
// frequency get their own sub-band. Sub-bands are spread across a worker pool.
#pragma once

#include "decode/decoder.h"
#include "decode/message_log.h"
#include "voice/wav_writer.h"
#include "dsp/ddc.h"
#include "audio/audio_output.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DecoderManager
{
public:
    struct Status
    {
        int channelId;
        double freqMHz;
        int baud;
        bool locked;
        double ebno;
        uint64_t msgs;
        bool isB = false; // set by the caller for the secondary receiver
        bool monitored = false;
        bool isVoice = false;
    };

    ~DecoderManager() { stop(); }

    void configure(double Fs, double centerHz);
    void start();
    void stop();

    // Set before start(): disable this manager's audio device (so only one of
    // several managers opens the speaker), and cap the worker-thread count.
    void setAudioEnabled(bool on) { audioEnabled_ = on; }
    void setMaxWorkers(int n) { maxWorkers_ = (n > 0) ? n : 1; }

    void feed(const float* iq, int nComplex);

    int addDecoder(double freqHz, int baud, uint32_t aesId = 0);
    void removeDecoder(int channelId);
    void setDecoderFreq(int channelId, double freqHz);
    void removeAll();
    int  decoderCount();
    int  workerCount() const { return (int)workers_.size(); }
    int  subbandCount();

    std::vector<Status> status();
    uint64_t drops() const { return drops_.load(); }
    MessageLog& log() { return log_; }
    AircraftTable& aircraftTable() { return acTable_; }
    const AircraftTable& aircraftTable() const { return acTable_; }
    VoiceCallLog& voiceCallLog() { return voiceCallLog_; }

    // Forward cpuReduce to all decoders (currently inert).
    void setCpuReduce(bool on) { (void)on; }

    // Persistence: forward the SQLite store to all message logs.
    void setMessageStore(MessageStore* s);

    // Voice: route one AM-voice channel to the speakers, and record calls.
    void setVoiceMonitor(int channelId);
    int  voiceMonitor() const { return voiceMonitorId_; }
    void setRecording(bool on, const std::string& dir);
    void setRecordFormat(RecordFormat fmt);
    bool recording() const { return recordOn_; }
    int  recordingCount();

    // Voice squelch threshold (dBFS); very low = always open.
    void setSquelchDb(double d);

    // Audio output device selection (index 0 = system default).
    std::vector<std::string> audioDevices() { return audio_.listDevices(); }
    void setAudioDevice(int index) { audio_.setDevice(index); }
    int  audioDevice() { return audio_.currentDevice(); }
    void setVoiceMute(bool m) { audio_.setMuted(m); }
    bool voiceMuted() const { return audio_.muted(); }
    float audioLevel() { return audio_.level(); }

private:
    struct SubBand
    {
        SubBand(double Fs, double wideCenterHz, double center, double rateTarget,
                double bw)
            : centerHz(center), frontEnd(Fs, center - wideCenterHz, rateTarget, bw)
        {
            subRate = frontEnd.outputRate();
        }
        double centerHz;
        double subRate = 0.0;
        Ddc frontEnd;                 // Fs -> subRate (shared by all decoders)
        std::vector<double> subIQ;    // scratch (worker thread only)
        std::vector<std::shared_ptr<Decoder>> decoders;
    };

    struct Worker
    {
        std::thread thread;
        std::mutex qMtx;
        std::condition_variable cv;
        std::deque<std::shared_ptr<const std::vector<float>>> queue;

        std::mutex dMtx; // guards subbands
        std::vector<std::shared_ptr<SubBand>> subbands;
        std::atomic<int> count{0};   // total decoders on this worker
        std::atomic<int> weight{0};  // weighted load
    };

    void workerLoop(Worker* w);

    double Fs_ = 0.0;
    double centerHz_ = 0.0;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> run_{false};

    std::mutex idMtx_;
    int nextId_ = 1;

    std::atomic<uint64_t> drops_{0};
    static constexpr size_t kMaxQueue = 192;
    MessageLog log_;
    AircraftTable acTable_;
    VoiceCallLog voiceCallLog_;
    AudioOutput audio_;
    bool audioEnabled_ = true;
    int maxWorkers_ = 8;
    int voiceMonitorId_ = -1;
    bool recordOn_ = false;
    std::string recordDir_ = "recordings";
    RecordFormat recordFmt_ = RecordFormat::WAV;
    double squelchDb_ = -120.0;
};
