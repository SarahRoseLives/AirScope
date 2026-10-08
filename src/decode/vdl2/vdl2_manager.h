// C++ wrapper around the VDL2 engine: routes decoded ACARS-over-VDL2 messages
// into a MessageLog. The engine itself is process-wide (dumpvdl2 core), so only
// one Vdl2Manager may be running at a time.
#pragma once

#include "decode/message_log.h"
#include "decode/vdl2/vdl2.h"

#include <cstdint>
#include <map>
#include <vector>

class Vdl2Manager
{
public:
    void setMessageLog(MessageLog* log) { log_ = log; }
    void setAircraftTable(AircraftTable* t) { acTable_ = t; }

    // freqsMHz: VDL2 channel frequencies. Returns true if the engine started.
    bool start(double fs, double centerHz, const std::vector<double>& freqsMHz, bool conjugate);
    void stop();

    void feed(const float* iq, int nComplex);
    bool running() const;
    uint64_t msgCount() const { return count_; }
    uint64_t frameCount() const;
    uint64_t droppedCount() const;

    // Active channel frequencies (MHz) and per-channel ACARS message counts.
    void setChannels(const std::vector<double>& f) { channelsMHz_ = f; chMsgs_.clear(); }
    const std::vector<double>& channelsMHz() const { return channelsMHz_; }
    uint64_t channelMsgs(uint32_t freqHz) const
    {
        auto it = chMsgs_.find(freqHz);
        return it == chMsgs_.end() ? 0 : it->second;
    }

private:
    static void onAcars(const vdl2_acars_t* msg, void* user);

    MessageLog* log_ = nullptr;
    AircraftTable* acTable_ = nullptr;
    uint64_t count_ = 0;
    std::vector<double> channelsMHz_;
    std::map<uint32_t, uint64_t> chMsgs_;
};
