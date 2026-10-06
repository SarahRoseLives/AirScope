#include "sdr/wav_file_source.h"

#include "util/log.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint16_t rd16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

} // namespace

WavFileSource::~WavFileSource()
{
    stop();
}

bool WavFileSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    std::ifstream f(path_, std::ios::binary);
    if (!f)
    {
        err = "Cannot open file: " + path_;
        return false;
    }

    unsigned char hdr[12];
    f.read((char*)hdr, 12);
    if (!f || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        err = "Not a RIFF/WAVE file";
        return false;
    }

    bool haveFmt = false, haveData = false;
    while (f && !(haveFmt && haveData))
    {
        unsigned char ch[8];
        f.read((char*)ch, 8);
        if (!f)
            break;
        uint32_t id = rd32(ch);
        uint32_t sz = rd32(ch + 4);
        (void)id;

        if (std::memcmp(ch, "fmt ", 4) == 0)
        {
            std::vector<unsigned char> fmt(sz);
            f.read((char*)fmt.data(), sz);
            if (sz >= 16)
            {
                channels_ = rd16(fmt.data() + 2);
                sampleRate_ = (double)rd32(fmt.data() + 4);
                bits_ = rd16(fmt.data() + 14);
            }
            haveFmt = true;
            if (sz & 1) f.seekg(1, std::ios::cur); // word alignment
        }
        else if (std::memcmp(ch, "data", 4) == 0)
        {
            dataOffset_ = (uint64_t)f.tellg();
            dataBytes_ = sz;
            haveData = true;
            // don't read the data here
        }
        else
        {
            f.seekg(sz + (sz & 1), std::ios::cur);
        }
    }

    if (!haveFmt || !haveData)
    {
        err = "Missing fmt/data chunk";
        return false;
    }
    if (channels_ < 1 || channels_ > 2 || (bits_ != 8 && bits_ != 16))
    {
        err = "Unsupported WAV (need 8/16-bit, 1/2 ch)";
        return false;
    }

    logWrite("[wav] opened %dch %d-bit %g Hz",
             channels_, bits_, sampleRate_);

    // SDR captures often exceed 4 GB, where the 32-bit data-chunk size field
    // is truncated/bogus. Use the actual remaining file length instead.
    f.clear();
    f.seekg(0, std::ios::end);
    uint64_t fileSize = (uint64_t)f.tellg();
    if (fileSize > dataOffset_)
    {
        uint64_t physical = fileSize - dataOffset_;
        if (dataBytes_ == 0 || dataBytes_ > physical)
            dataBytes_ = physical;
    }

    cb_ = std::move(cb);
    progress_.store(0.0);
    running_.store(true);
    thread_ = std::thread([this]() { playLoop(); });
    return true;
}

void WavFileSource::stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    cb_ = nullptr;
}

void WavFileSource::playLoop()
{
    std::ifstream f(path_, std::ios::binary);
    if (!f)
    {
        running_.store(false);
        return;
    }

    const int frameBytes = channels_ * (bits_ / 8);
    const uint64_t totalFrames = dataBytes_ / (uint64_t)frameBytes;
    const int kFrames = 32768;

    std::vector<unsigned char> raw((size_t)kFrames * frameBytes);
    std::vector<float> iq((size_t)kFrames * 2);

    f.seekg((std::streamoff)dataOffset_, std::ios::beg);
    uint64_t framePos = 0;

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    uint64_t played = 0;

    while (running_.load())
    {
        uint64_t remaining = totalFrames - framePos;
        if (remaining == 0)
        {
            if (loop_.load())
            {
                f.clear();
                f.seekg((std::streamoff)dataOffset_, std::ios::beg);
                framePos = 0;
                t0 = clock::now();
                played = 0;
                continue;
            }
            break;
        }

        int want = (int)std::min<uint64_t>(kFrames, remaining);
        f.read((char*)raw.data(), (std::streamsize)want * frameBytes);
        std::streamsize got = f.gcount();
        int frames = (int)(got / frameBytes);
        if (frames <= 0)
            break;

        for (int i = 0; i < frames; ++i)
        {
            const unsigned char* p = raw.data() + (size_t)i * frameBytes;
            float vi, vq;
            if (bits_ == 8)
            {
                vi = ((int)p[0] - 128) * (1.0f / 128.0f);
                vq = (channels_ == 2) ? ((int)p[1] - 128) * (1.0f / 128.0f) : 0.0f;
            }
            else
            {
                int16_t s0 = (int16_t)rd16(p);
                int16_t s1 = (channels_ == 2) ? (int16_t)rd16(p + 2) : 0;
                vi = s0 * (1.0f / 32768.0f);
                vq = (channels_ == 2) ? s1 * (1.0f / 32768.0f) : 0.0f;
            }
            iq[(size_t)i * 2] = vi;
            iq[(size_t)i * 2 + 1] = vq;
        }

        if (cb_)
            cb_(iq.data(), frames);

        framePos += frames;
        played += frames;
        progress_.store(totalFrames ? (double)framePos / (double)totalFrames : 0.0);

        // Pace to real time so the ring isn't overrun and the waterfall
        // scrolls at the capture's true rate.
        auto target = t0 + std::chrono::duration_cast<clock::duration>(
                               std::chrono::duration<double>((double)played / sampleRate_));
        std::this_thread::sleep_until(target);
    }

    running_.store(false);
}
