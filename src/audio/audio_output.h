// Simple streaming audio output for decoded voice (8 kHz mono int16).
// Thread-safe push() from the decode thread; an internal ring feeds the
// audio device callback. Backed by miniaudio (cross-platform).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AudioOutputImpl; // defined in audio_output.cpp

class AudioOutput
{
public:
    AudioOutput();
    ~AudioOutput();

    bool start(int sampleRate = 8000);
    void stop();
    bool running() const;

    // Playback device selection. listDevices() returns names with index 0 =
    // "Default (system)"; setDevice() picks by that index and live-restarts the
    // device if it is running.
    std::vector<std::string> listDevices();
    void setDevice(int index);
    int  currentDevice() const;

    // Append PCM samples (mono int16). Safe from any thread.
    void push(const int16_t* pcm, int n);
    void clear();

    float level() const; // recent output RMS (0..1) for a UI meter

    void setMuted(bool m) { muted_.store(m); }
    bool muted() const { return muted_.load(); }

private:
    std::unique_ptr<AudioOutputImpl> impl_;
    std::atomic<bool> muted_{false};
};
