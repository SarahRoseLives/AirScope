// Offline ACARS decoder self-test: feeds a multi-channel 16-bit PCM WAV
// (each channel an AM envelope at 12.5 kHz) through AcarsDecoder.
// Build: g++ -std=c++17 -I src tools/acars_selftest.cpp \
//            src/decode/acars/acars_decoder.cpp -o /tmp/acars_selftest
#include "decode/acars/acars_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Wav
{
    int channels = 0, bits = 0, rate = 0;
    std::vector<int16_t> data; // interleaved
};

static bool readWav(const char* path, Wav& w)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(n);
    if (std::fread(buf.data(), 1, n, f) != (size_t)n) { std::fclose(f); return false; }
    std::fclose(f);

    if (n < 12 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4))
        return false;

    size_t p = 12;
    while (p + 8 <= buf.size())
    {
        char id[5] = {};
        std::memcpy(id, buf.data() + p, 4);
        uint32_t sz = *(uint32_t*)(buf.data() + p + 4);
        p += 8;
        if (!std::memcmp(id, "fmt ", 4) && sz >= 16)
        {
            uint16_t fmt = *(uint16_t*)(buf.data() + p);
            (void)fmt;
            w.channels = *(uint16_t*)(buf.data() + p + 2);
            w.rate = *(uint32_t*)(buf.data() + p + 4);
            w.bits = *(uint16_t*)(buf.data() + p + 14);
        }
        else if (!std::memcmp(id, "data", 4))
        {
            size_t bytes = sz;
            if (p + bytes > buf.size()) bytes = buf.size() - p;
            size_t ns = bytes / 2;
            w.data.resize(ns);
            std::memcpy(w.data.data(), buf.data() + p, ns * 2);
            return w.channels > 0 && w.bits == 16;
        }
        p += sz + (sz & 1);
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.wav\n", argv[0]); return 2; }
    Wav w;
    if (!readWav(argv[1], w))
    {
        std::fprintf(stderr, "failed to read %s\n", argv[1]);
        return 1;
    }
    std::printf("WAV: %d ch, %d-bit, %d Hz, %zu samples\n",
                w.channels, w.bits, w.rate, w.data.size());

    int total = 0;
    for (int c = 0; c < w.channels; ++c)
    {
        AcarsDecoder dec;
        dec.setCallback([c, &total](const AcarsMsg& m) {
            ++total;
            std::printf("[ch%d] %c %-7s lbl=%.2s bid=%c %s\n     %s\n",
                        c, m.mode, m.addr.c_str(), m.label.c_str(), m.bid,
                        m.downlink ? "DL" : "UL", m.text.c_str());
        });
        std::vector<float> env;
        for (size_t i = (size_t)c; i < w.data.size(); i += w.channels)
            env.push_back((float)w.data[i] / 32768.0f);
        dec.process(env.data(), (int)env.size());
        std::printf("ch%d: %llu message(s)\n", c, (unsigned long long)dec.msgCount());
    }
    std::printf("TOTAL: %d\n", total);
    return total > 0 ? 0 : 3;
}
