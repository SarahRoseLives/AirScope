// End-to-end check of the WAV IQ player on a real capture file:
// open -> playback thread -> ring -> FFT -> spectrum stats.

#include "dsp/iq_ring.h"
#include "dsp/jfft.h"
#include "sdr/wav_file_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1]
                                  : "D:\\baseband_1543658118Hz_22-19-08_16-06-2026.wav";

    IqRing ring(1u << 21);
    WavFileSource wav;
    wav.setPath(path);
    wav.setLoop(false);

    std::string err;
    bool ok = wav.start(0, [&](const float* iq, int n) { ring.push(iq, (size_t)n); }, err);
    if (!ok)
    {
        std::printf("start failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("opened: %d ch, %d-bit, %.1f kHz\n", wav.channels(), wav.bits(),
                wav.sampleRate() / 1e3);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int N = 8192;
    std::vector<std::complex<double>> iq(N);
    if (!ring.latest(iq.data(), N))
    {
        std::printf("no samples arrived\n");
        wav.stop();
        return 1;
    }

    std::vector<double> win(N);
    for (int i = 0; i < N; ++i)
    {
        double x = 2.0 * M_PI * i / (N - 1);
        win[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
        iq[i] *= win[i];
    }

    JFFT fft;
    int nf = N;
    fft.init(nf);
    fft.fft(iq.data(), N, JFFT::FORWARD);

    std::vector<float> db(N);
    for (int i = 0; i < N; ++i)
    {
        int src = (i + N / 2) % N;
        double p = std::norm(iq[src]) / N;
        db[i] = (float)(10.0 * std::log10(p + 1e-20));
    }

    std::vector<float> s = db;
    std::nth_element(s.begin(), s.begin() + (int)(0.30 * N), s.end());
    float floorDb = s[(int)(0.30 * N)];
    float peakDb = *std::max_element(db.begin(), db.end());
    int strong = 0;
    for (float v : db)
        if (v > floorDb + 10.0f)
            ++strong;

    std::printf("floor(p30)=%.1f dB  peak=%.1f dB  bins>floor+10=%d / %d\n",
                floorDb, peakDb, strong, N);
    std::printf("%s\n", (peakDb > floorDb + 10.0f)
                            ? "PASS - real RF structure present"
                            : "FAIL - looks flat");

    wav.stop();
    return 0;
}
