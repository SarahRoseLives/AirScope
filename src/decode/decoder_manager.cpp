#include "decode/decoder_manager.h"

#include "voice/wav_writer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// Shared front-end IF rate and passband. ~250 kHz IF -> ±150 kHz usable
// coverage per sub-band; decoders within ±135 kHz of a sub-band centre share it.
static constexpr double kSubRateTarget = 250000.0;
static constexpr double kSubBW = 200000.0;

void DecoderManager::configure(double Fs, double centerHz)
{
    Fs_ = Fs;
    centerHz_ = centerHz;
}

void DecoderManager::start()
{
    if (run_.load())
        return;

    unsigned hw = std::thread::hardware_concurrency();
    int nWorkers = (hw > 2) ? (int)hw : 1;
    if (nWorkers > maxWorkers_)
        nWorkers = maxWorkers_;
    if (nWorkers < 1)
        nWorkers = 1;

    run_.store(true);
    workers_.clear();
    for (int i = 0; i < nWorkers; ++i)
        workers_.push_back(std::make_unique<Worker>());
    for (auto& w : workers_)
        w->thread = std::thread([this, wp = w.get()]() { workerLoop(wp); });

    if (audioEnabled_)
        audio_.start(8000); // 8 kHz mono voice output
}

void DecoderManager::stop()
{
    run_.store(false);
    for (auto& w : workers_)
        w->cv.notify_all();
    for (auto& w : workers_)
        if (w->thread.joinable())
            w->thread.join();
    workers_.clear();
    if (audioEnabled_)
        audio_.stop();
    voiceMonitorId_ = -1;
}

void DecoderManager::feed(const float* iq, int nComplex)
{
    if (!run_.load())
        return;
    // One shared block — all workers reference the same memory, zero per-worker copies.
    auto shared = std::make_shared<const std::vector<float>>(iq, iq + (size_t)nComplex * 2);
    for (auto& w : workers_)
    {
        if (w->count.load() == 0)
            continue;
        {
            std::lock_guard<std::mutex> lk(w->qMtx);
            if (w->queue.size() >= kMaxQueue)
            {
                w->queue.pop_front();
                drops_.fetch_add(1);
            }
            w->queue.push_back(shared);
        }
        w->cv.notify_one();
    }
}

int DecoderManager::addDecoder(double freqHz, int baud, uint32_t aesId)
{
    (void)aesId;
    if (Fs_ <= 0.0 || workers_.empty())
        return -1;

    int id;
    {
        std::lock_guard<std::mutex> lk(idMtx_);
        id = nextId_++;
    }

    // 1) Find the best existing sub-band that covers this frequency.
    //    If a sub-band is overloaded (>4 decoders), prefer a shadow
    //    sub-band on a lighter worker to spread the load.
    Worker* bestW = nullptr;
    std::shared_ptr<SubBand> bestSb;
    int bestLoad = 0x7FFFFFFF; // total decoders on that worker
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
        {
            if (std::fabs(freqHz - sb->centerHz) < 0.40 * sb->subRate)
            {
                int cnt = (int)sb->decoders.size();
                if (cnt >= 4)
                {
                    int eff = w->weight.load() + (int)w->subbands.size() * 10 + cnt * 8;
                    if (eff < bestLoad)
                    {
                        bestW = w.get();
                        bestSb = sb;
                        bestLoad = eff;
                    }
                }
                else
                {
                    if (cnt < bestLoad)
                    {
                        bestW = w.get();
                        bestSb = sb;
                        bestLoad = cnt;
                    }
                }
            }
        }
    }

    if (bestW)
    {
        std::lock_guard<std::mutex> lk(bestW->dMtx);
        // Re-lookup the sub-band (it might have been removed between the two locks).
        for (auto& sb : bestW->subbands)
        {
            if (sb != bestSb) continue;
            auto dec = std::make_shared<Decoder>(
                sb->subRate, sb->centerHz, freqHz, baud, id, &log_, &acTable_,
                &audio_, &voiceCallLog_);
            if (baud == kVoiceBaud)
            {
                if (voiceMonitorId_ < 0) { dec->setMonitored(true); voiceMonitorId_ = id; }
                dec->setRecording(recordOn_, recordDir_, recordFmt_);
                dec->setSquelchDb(squelchDb_);
                dec->setVolume(volume_);
            }
            sb->decoders.push_back(dec);
            bestW->count.fetch_add(1);
            bestW->weight.fetch_add(1);
            return id;
        }
        // Sub-band was removed between locks — fall through to creation.
    }

    // 2) No covering or non-overloaded sub-band -> create a new one.
    Worker* best = workers_[0].get();
    int bestEff = best->weight.load() + (int)best->subbands.size() * 5;
    for (auto& w : workers_)
    {
        int eff = w->weight.load() + (int)w->subbands.size() * 5;
        if (eff < bestEff) { best = w.get(); bestEff = eff; }
    }

    std::lock_guard<std::mutex> lk(best->dMtx);
    auto sb = std::make_shared<SubBand>(Fs_, centerHz_, freqHz, kSubRateTarget, kSubBW);
    auto dec = std::make_shared<Decoder>(
        sb->subRate, sb->centerHz, freqHz, baud, id, &log_, &acTable_,
        &audio_, &voiceCallLog_);
    if (baud == kVoiceBaud)
    {
        if (voiceMonitorId_ < 0) { dec->setMonitored(true); voiceMonitorId_ = id; }
        dec->setRecording(recordOn_, recordDir_, recordFmt_);
        dec->setSquelchDb(squelchDb_);
        dec->setVolume(volume_);
    }
    sb->decoders.push_back(dec);
    best->subbands.push_back(std::move(sb));
    best->count.fetch_add(1);
    best->weight.fetch_add(1);
    return id;
}

void DecoderManager::removeDecoder(int channelId)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto sbIt = w->subbands.begin(); sbIt != w->subbands.end(); ++sbIt)
        {
            auto& decs = (*sbIt)->decoders;
            for (auto it = decs.begin(); it != decs.end(); ++it)
                if ((*it)->channelId() == channelId)
                {
                    decs.erase(it);
                    w->count.fetch_sub(1);
                    w->weight.fetch_sub(1);
                    if (decs.empty())
                        w->subbands.erase(sbIt); // drop now-empty sub-band
                    if (channelId == voiceMonitorId_)
                        voiceMonitorId_ = -1;
                    return;
                }
        }
    }
}

void DecoderManager::setDecoderFreq(int channelId, double freqHz)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->channelId() == channelId)
                {
                    d->setFreq(freqHz); // stays within the sub-band's IF window
                    return;
                }
    }
}

void DecoderManager::setMessageStore(MessageStore* s)
{
    log_.setStore(s);
    voiceCallLog_.setStore(s);
}

void DecoderManager::setVoiceMonitor(int channelId)
{
    voiceMonitorId_ = channelId;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                d->setMonitored(d->isVoice() && d->channelId() == channelId);
    }
    audio_.clear();
}

void DecoderManager::setRecording(bool on, const std::string& dir)
{
    recordOn_ = on;
    if (!dir.empty()) recordDir_ = dir;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->isVoice())
                    d->setRecording(on, recordDir_, recordFmt_);
    }
}

void DecoderManager::setRecordFormat(RecordFormat fmt)
{
    recordFmt_ = fmt;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->isVoice())
                    d->setRecording(recordOn_, recordDir_, fmt);
    }
}

int DecoderManager::recordingCount()
{
    int n = 0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->isVoice() && d->recordingNow())
                    ++n;
    }
    return n;
}

void DecoderManager::setSquelchDb(double d)
{
    squelchDb_ = d;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& dec : sb->decoders)
                if (dec->isVoice())
                    dec->setSquelchDb(d);
    }
}

void DecoderManager::setVolume(float v)
{
    volume_ = v;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& dec : sb->decoders)
                if (dec->isVoice())
                    dec->setVolume(v);
    }
}

// Scan a directory for WAV/OGG voice recordings and populate VoiceCallLog.
void VoiceCallLog::scanDir(const std::string& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return;

    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        std::string name = entry.path().filename().string();
        std::string ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos)
            ext = name.substr(dot);
        if (ext != ".wav" && ext != ".ogg")
            continue;

        // New format (ICAO first):  "ABCDEF_20250627_143021_1546.0625MHz_ch7.wav"
        // New format (no ICAO):     "20250627_143021_1546.0625MHz_ch7.wav"
        VoiceCallRecord r;
        r.recording = false;
        r.filename = name;
        bool haveIcao = false;
        std::string icaoHex;
        size_t tsStart = 0;

        auto isHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); };
        if (name.size() > 7 && isHex(name[0]) && isHex(name[1]) && isHex(name[2]) &&
            isHex(name[3]) && isHex(name[4]) && isHex(name[5]) && name[6] == '_')
        {
            icaoHex = name.substr(0, 6);
            haveIcao = true;
            tsStart = 7;
        }

        if (name.size() < tsStart + 16) continue;
        std::tm tm{};
        char ts[16];
        std::memcpy(ts, name.c_str() + tsStart, 15);
        ts[15] = 0;
        int yr, mo, dy, hr, mn, sc;
        if (std::sscanf(ts, "%4d%2d%2d_%2d%2d%2d", &yr, &mo, &dy, &hr, &mn, &sc) != 6)
            continue;
        tm.tm_year = yr - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = dy;
        tm.tm_hour = hr;
        tm.tm_min = mn;
        tm.tm_sec = sc;
        tm.tm_isdst = -1;
        time_t t = std::mktime(&tm);
        if (t != (time_t)-1)
            r.timeSec = (double)t;

        auto u1 = name.find('_', tsStart + 15);
        if (u1 == std::string::npos) continue;
        auto mhz = name.find("MHz", u1);
        if (mhz == std::string::npos) continue;
        std::string freqStr = name.substr(u1 + 1, mhz - u1 - 1);
        r.freqMHz = std::atof(freqStr.c_str());

        auto ch = name.find("_ch", mhz);
        if (ch != std::string::npos)
            r.channelId = std::atoi(name.c_str() + ch + 3);

        if (haveIcao)
        {
            r.aesId = (uint32_t)std::strtoul(icaoHex.c_str(), nullptr, 16);
            r.icao = icaoHex;
        }
        else if (ch != std::string::npos)
        {
            auto tag = name.rfind('_');
            if (tag != std::string::npos && tag > ch + 3)
            {
                std::string icaoPart = name.substr(tag + 1, dot - tag - 1);
                if (icaoPart.size() == 6)
                {
                    r.aesId = (uint32_t)std::strtoul(icaoPart.c_str(), nullptr, 16);
                    r.icao = icaoPart;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            bool dup = false;
            for (auto& ex : items_)
                if (ex.filename == name) { dup = true; break; }
            if (dup) continue;
            items_.push_back(r);
            if (items_.size() > kMax)
                items_.erase(items_.begin());
            ++count_;
        }
    }
}

void DecoderManager::removeAll()
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        w->subbands.clear();
        w->count.store(0);
        std::lock_guard<std::mutex> ql(w->qMtx);
        w->queue.clear();
    }
}

int DecoderManager::decoderCount()
{
    int n = 0;
    for (auto& w : workers_)
        n += w->count.load();
    return n;
}

int DecoderManager::subbandCount()
{
    int n = 0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        n += (int)w->subbands.size();
    }
    return n;
}

std::vector<DecoderManager::Status> DecoderManager::status()
{
    std::vector<Status> out;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                out.push_back({d->channelId(), d->freqMHz(), d->baud(),
                               d->locked(), d->ebno(), d->msgCount(), false,
                               d->monitored(), d->isVoice()});
    }
    std::sort(out.begin(), out.end(),
              [](const Status& a, const Status& b) { return a.freqMHz < b.freqMHz; });
    return out;
}

void DecoderManager::workerLoop(Worker* w)
{
    while (run_.load())
    {
        std::shared_ptr<const std::vector<float>> blockPtr;
        {
            std::unique_lock<std::mutex> lk(w->qMtx);
            w->cv.wait(lk, [&]() { return !run_.load() || !w->queue.empty(); });
            if (!run_.load())
                break;
            blockPtr = w->queue.front();
            w->queue.pop_front();
        }
        const std::vector<float>& block = *blockPtr;
        int nWide = (int)(block.size() / 2);

        // Snapshot sub-band + decoder shared_ptrs under dMtx, then release
        // before processing so the UI thread never blocks on status().
        struct Snap { std::shared_ptr<SubBand> sb; std::vector<std::shared_ptr<Decoder>> decs; };
        std::vector<Snap> snap;
        {
            std::lock_guard<std::mutex> lk(w->dMtx);
            snap.reserve(w->subbands.size());
            for (auto& sb : w->subbands)
            {
                Snap s;
                s.sb = sb;
                s.decs = sb->decoders; // shared_ptr copies keep decoders alive
                snap.push_back(std::move(s));
            }
        }
        // Process outside dMtx — UI thread can now read status instantly.
        for (auto& s : snap)
        {
            s.sb->subIQ.clear();
            s.sb->frontEnd.process(block.data(), nWide, s.sb->subIQ);
            int nSub = (int)(s.sb->subIQ.size() / 2);
            if (nSub <= 0) continue;
            for (auto& d : s.decs)
                d->process(s.sb->subIQ.data(), nSub);
        }
    }
}
