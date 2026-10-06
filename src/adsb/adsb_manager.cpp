#include "adsb/adsb_manager.h"

#include "adsb/beast_writer.h"
#include "decode/message_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

static double nowSeconds()
{
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
}

AdsbManager::AdsbManager()
{
    static bool inited = false;
    if (!inited)
    {
        adsb::init(1);
        inited = true;
    }
    ring_.assign(kRingCap, 0);
}

AdsbManager::~AdsbManager()
{
    stop();
}

void AdsbManager::configure(double sampleRateHz, double centerHz)
{
    sampleRate_ = sampleRateHz > 1.0 ? sampleRateHz : 2.4e6;
    centerHz_ = centerHz > 1.0 ? centerHz : 1090.0e6;
    rateOk_ = std::fabs(sampleRate_ - 2.4e6) < 100e3;
}

void AdsbManager::start()
{
    if (running_.load())
        return;
    running_.store(true);
    thread_ = std::thread([this]() { workerLoop(); });
}

void AdsbManager::stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

void AdsbManager::feed(const float* iq, int nComplex)
{
    if (!running_.load() || nComplex <= 0)
        return;

    std::lock_guard<std::mutex> lk(mtx_);
    for (int k = 0; k < nComplex; ++k)
    {
        int ii = (int)std::lround(iq[k * 2] * 127.0f + 127.5f);
        int qq = (int)std::lround(iq[k * 2 + 1] * 127.0f + 127.5f);
        if (ii < 0) ii = 0; else if (ii > 255) ii = 255;
        if (qq < 0) qq = 0; else if (qq > 255) qq = 255;

        ring_[write_] = adsb::g_magLut[ii][qq];
        write_ = (write_ + 1) % kRingCap;
        if (length_ < kRingCap)
            ++length_;
    }
}

AdsbManager::Stats AdsbManager::stats() const
{
    Stats s;
    s.total = total_.load();
    s.good = good_.load();
    s.bad = bad_.load();
    return s;
}

int AdsbManager::aircraftCount()
{
    return tracker_.count();
}

void AdsbManager::workerLoop()
{
    std::vector<uint16_t> mag;
    mag.reserve(kRingCap);
    std::vector<adsb::RawMessage> raws;
    raws.reserve(128);

    double lastHousekeep = 0.0;

    while (running_.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!running_.load())
            break;

        int readable = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (length_ >= kOverlap * 2)
            {
                readable = (int)(length_ - kOverlap);
                size_t start = (write_ + kRingCap - length_) % kRingCap;
                mag.resize(length_);
                if (start + length_ <= kRingCap)
                {
                    std::memcpy(mag.data(), ring_.data() + start, length_ * sizeof(uint16_t));
                }
                else
                {
                    size_t n1 = kRingCap - start;
                    std::memcpy(mag.data(), ring_.data() + start, n1 * sizeof(uint16_t));
                    std::memcpy(mag.data() + n1, ring_.data(), (length_ - n1) * sizeof(uint16_t));
                }
                length_ = kOverlap;
            }
        }
        if (readable <= 0)
            continue;

        raws.clear();
        adsb::demodulate2400(mag.data(), readable, phaseEnhance_, raws);

        for (const auto& raw : raws)
        {
            total_.fetch_add(1);
            adsb::Decoded d;
            adsb::decode(raw, d);
            if (!d.crc)
            {
                bad_.fetch_add(1);
                continue;
            }
            good_.fetch_add(1);

            double now = nowSeconds();

            if (beast_)
                beast_->write(d.raw, d.rawBytes, now, d.sigLevel);

            adsb::TrackResult tr = tracker_.update(d, now);
            (void)tr;

            if (acTable_)
            {
                DecodedMessage m;
                m.timeSec = now;
                m.freqMHz = centerHz_ / 1e6;
                m.aesId = d.icao;
                char h[8];
                std::snprintf(h, sizeof(h), "%06X", d.icao & 0xFFFFFF);
                m.icao = h;
                m.flight = tr.callsign;
                m.hex = d.hex();
                if (tr.hasPos)
                {
                    m.hasPos = true;
                    m.lat = tr.lat;
                    m.lon = tr.lon;
                    m.alt = tr.alt;
                }
                m.decoded = "";
                acTable_->update(m, now);
            }
        }

        double now = nowSeconds();
        if (now - lastHousekeep > 0.25)
        {
            tracker_.removeStale(now);
            lastHousekeep = now;
        }
    }
}
