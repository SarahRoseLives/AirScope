// Thread-safe logs for decoded messages and tracked aircraft.
#pragma once

#include "store/message_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct DecodedMessage
{
    double timeSec = 0.0;
    int channelId = 0;
    double freqMHz = 0.0;
    uint32_t aesId = 0;   // ICAO 24-bit address (ADS-B / ADS-C) or 0
    uint8_t gesId = 0;
    int downlink = 0;
    std::string reg;   // aircraft registration (ACARS)
    std::string label; // ACARS label
    char mode = 0;     // ACARS mode char
    char blockId = 0;  // ACARS block id char
    std::string text;  // printable rendering of the payload
    std::string hex;   // hex rendering
    std::string decoded; // libacars-decoded application text (CPDLC/ADS-C/...), empty if none
    bool   hasPos = false; // a position was extracted (ADS-C)
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;        // altitude in feet (ADS-C)
    double heading = 0.0;  // ground track in degrees true (ADS-B)
    std::string icao;      // ICAO 24-bit hex (ADS-C airframe id), if present
    std::string flight;    // flight/callsign (ADS-C flight id), if present
};

class MessageLog
{
public:
    void setStore(MessageStore* s) { store_ = s; }

    void add(const DecodedMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    // Called from the decode worker — persists to store and then in-memory.
    void addAndStore(const DecodedMessage& m, int baud)
    {
        if (store_)
            store_->storeAcars(m.timeSec, m.channelId, m.freqMHz, m.text, m.hex,
                              m.aesId, m.icao, m.reg, m.flight, m.label,
                              m.hasPos, m.lat, m.lon, m.alt, m.decoded,
                              (m.downlink != 0), baud);
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    // Copy a snapshot for rendering.
    std::vector<DecodedMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return archive_;
        return msgs_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (viewArchive_)
            return (uint64_t)archive_.size();
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
        archive_.clear();
        viewArchive_ = false;
    }

    // Load a past session into the archive buffer (replacing any prior archive).
    void setArchive(std::vector<DecodedMessage> items)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_ = std::move(items);
        viewArchive_ = true;
    }

    void clearArchive()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        archive_.clear();
        viewArchive_ = false;
    }

    bool hasArchive() const { return viewArchive_; }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<DecodedMessage> msgs_;
    std::vector<DecodedMessage> archive_;
    bool viewArchive_ = false;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
};

// One tracked aircraft, keyed by ICAO/AES, aggregated from ACARS and ADS-B.
struct AircraftEntry
{
    uint32_t aesId = 0;
    std::string icao;    // ICAO 24-bit hex
    std::string reg;     // registration (ACARS)
    std::string flight;  // flight / callsign
    bool   hasPos = false;
    double lat = 0.0;
    double lon = 0.0;
    int    alt = 0;
    double heading = 0.0;  // ground track in degrees true
    double posTime = 0.0;  // epoch sec of last position
    double lastSeen = 0.0; // epoch sec of last message
    uint64_t msgs = 0;
    double freqMHz = 0.0;
};

// Thread-safe per-aircraft tracking table. Updated from decode worker threads,
// snapshotted by the UI thread.
class AircraftTable
{
public:
    void update(const DecodedMessage& m, double nowSec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // Key by ICAO/AES when known, otherwise by registration so ACARS/VDL2
        // aircraft that carry no 24-bit address still appear on the map.
        AircraftEntry* ep = nullptr;
        if (m.aesId != 0)
            ep = &byId_[m.aesId];
        else if (!m.reg.empty())
            ep = &byReg_[m.reg];
        else
            return;
        AircraftEntry& e = *ep;
        if (m.aesId != 0)      e.aesId = m.aesId;
        if (!m.icao.empty())   e.icao = m.icao;
        if (!m.reg.empty())    e.reg = m.reg;
        if (!m.flight.empty()) e.flight = m.flight;
        if (m.hasPos)
        {
            e.hasPos = true;
            e.lat = m.lat;
            e.lon = m.lon;
            e.alt = m.alt;
            e.posTime = nowSec;
        }
        if (m.heading != 0.0) e.heading = m.heading;
        e.lastSeen = nowSec;
        e.freqMHz = m.freqMHz;
        ++e.msgs;
    }

    std::vector<AircraftEntry> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<AircraftEntry> out;
        out.reserve(byId_.size() + byReg_.size());
        for (auto& kv : byId_)
            out.push_back(kv.second);
        for (auto& kv : byReg_)
            out.push_back(kv.second);
        return out;
    }

    size_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return byId_.size() + byReg_.size();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        byId_.clear();
        byReg_.clear();
    }

    // Quick ICAO-only update (no position/flight).
    void setIcao(uint32_t aesId, const std::string& icao, double nowSec)
    {
        if (aesId == 0 || icao.empty())
            return;
        std::lock_guard<std::mutex> lk(mtx_);
        AircraftEntry& e = byId_[aesId];
        e.aesId = aesId;
        e.icao = icao;
        if (e.lastSeen < nowSec)
            e.lastSeen = nowSec;
    }

    std::string icao(uint32_t aesId) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = byId_.find(aesId);
        if (it == byId_.end()) return {};
        return it->second.icao;
    }

private:
    mutable std::mutex mtx_;
    std::map<uint32_t, AircraftEntry> byId_;
    std::map<std::string, AircraftEntry> byReg_;
};

// A recorded voice call (VHF AM voice, one file per call).
struct VoiceCallRecord
{
    double timeSec = 0.0;        // epoch of call start
    double durationSec = 0.0;    // 0 if still live
    double freqMHz = 0.0;
    int channelId = -1;
    uint32_t aesId = 0;
    std::string icao;
    std::string filename;        // just the filename, no directory
    bool recording = false;      // true = live call in progress
};

class VoiceCallLog
{
public:
    void setStore(MessageStore* s) { store_ = s; }

    void add(const VoiceCallRecord& r)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.push_back(r);
        if (items_.size() > kMax)
            items_.erase(items_.begin(), items_.begin() + (items_.size() - kMax));
        ++count_;
    }

    // Scanner: start tracking a live call (no file yet).
    void beginLive(double freqMHz, int channelId, double timeSec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& it : items_)
            if (it.channelId == channelId && it.recording)
                return; // already tracking this channel
        VoiceCallRecord r;
        r.timeSec = timeSec;
        r.freqMHz = freqMHz;
        r.channelId = channelId;
        r.recording = true;
        items_.push_back(r);
        if (items_.size() > kMax)
            items_.erase(items_.begin(), items_.begin() + (items_.size() - kMax));
        ++count_;
    }

    // Attach a recording filename to the live call for channelId (if any).
    bool setLiveFilename(int channelId, const std::string& name)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = items_.rbegin(); it != items_.rend(); ++it)
            if (it->channelId == channelId && it->recording)
            {
                const char* s = name.c_str();
                const char* slash = std::strrchr(s, '/');
#ifdef _WIN32
                const char* bslash = std::strrchr(s, '\\');
                if (bslash && bslash > slash) slash = bslash;
#endif
                it->filename = slash ? (slash + 1) : s;
                return true;
            }
        return false;
    }

    // Scanner: finalise a live call started with beginLive().
    void closeCall(int channelId, double nowSec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = items_.rbegin(); it != items_.rend(); ++it)
            if (it->channelId == channelId && it->recording)
            {
                it->recording = false;
                it->durationSec = nowSec - it->timeSec;
                if (store_ && it->durationSec > 0)
                    store_->storeVoice(it->timeSec, it->freqMHz, it->aesId,
                                       it->icao, it->durationSec, it->filename);
                return;
            }
    }

    // Called when a live recording ends: finds the entry by channelId and fills
    // in the duration + filename. If not found, adds a new completed record.
    void updateEnd(int channelId, double durationSec, const std::string& filename)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        {
            if (it->channelId == channelId && it->recording)
            {
                it->recording = false;
                it->durationSec = durationSec;
                if (!filename.empty())
                {
                    const char* s = filename.c_str();
                    const char* slash = std::strrchr(s, '/');
#ifdef _WIN32
                    const char* bslash = std::strrchr(s, '\\');
                    if (bslash && bslash > slash) slash = bslash;
#endif
                    it->filename = slash ? (slash + 1) : s;
                }
                if (store_ && it->durationSec > 0)
                    store_->storeVoice(it->timeSec, it->freqMHz, it->aesId,
                                      it->icao, it->durationSec, it->filename);
                return;
            }
        }
    }

    // Scan a directory for existing WAV/OGG files and populate.
    void scanDir(const std::string& dir);

    std::vector<VoiceCallRecord> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return items_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        items_.clear();
    }

private:
    static constexpr size_t kMax = 500;
    std::mutex mtx_;
    std::vector<VoiceCallRecord> items_;
    uint64_t count_ = 0;
    MessageStore* store_ = nullptr;
};
