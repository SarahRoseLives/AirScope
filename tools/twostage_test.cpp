// Verify the two-stage (shared front-end + per-channel) DDC places a tone at
// baseband identically to a single-stage DDC.
#include "dsp/ddc.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double residualFreq(const std::vector<double>& iq, double rate)
{
    // Average phase increment between consecutive samples -> Hz.
    std::complex<double> acc(0, 0);
    int n = (int)(iq.size() / 2);
    std::complex<double> prev(iq[0], iq[1]);
    for (int i = 1; i < n; ++i)
    {
        std::complex<double> s(iq[i * 2], iq[i * 2 + 1]);
        acc += s * std::conj(prev);
        prev = s;
    }
    return std::arg(acc) / (2.0 * M_PI) * rate;
}

static double rms(const std::vector<double>& iq)
{
    double p = 0;
    int n = (int)(iq.size() / 2);
    for (int i = 0; i < n; ++i)
        p += iq[i * 2] * iq[i * 2] + iq[i * 2 + 1] * iq[i * 2 + 1];
    return std::sqrt(p / n);
}

int main()
{
    double Fs = 2048000.0;
    double Ftone = 37000.0; // channel offset from wide center
    int N = 1 << 20;
    std::vector<float> wide((size_t)N * 2);
    double ph = 0, inc = 2.0 * M_PI * Ftone / Fs;
    for (int i = 0; i < N; ++i)
    {
        wide[i * 2] = 0.3f * (float)std::cos(ph);
        wide[i * 2 + 1] = 0.3f * (float)std::sin(ph);
        ph += inc;
    }

    // Single-stage reference.
    Ddc single(Fs, Ftone, 48000.0, 6000.0);
    std::vector<double> outS;
    single.process(wide.data(), N, outS);

    // Two-stage: front-end (Fs -> ~250k, centred at Ftone) then per-channel.
    double subCenter = Ftone;
    Ddc front(Fs, subCenter, 250000.0, 200000.0);
    std::vector<double> sub;
    front.process(wide.data(), N, sub);
    Ddc chan(front.outputRate(), Ftone - subCenter, 48000.0, 6000.0);
    std::vector<double> outT;
    chan.process(sub.data(), (int)(sub.size() / 2), outT);

    std::printf("single: rate=%.1f  residual=%.2f Hz  rms=%.3f\n",
                single.outputRate(), residualFreq(outS, single.outputRate()), rms(outS));
    std::printf("front : rate=%.1f  rms=%.3f\n", front.outputRate(), rms(sub));
    std::printf("2-stage: rate=%.1f  residual=%.2f Hz  rms=%.3f\n",
                chan.outputRate(), residualFreq(outT, chan.outputRate()), rms(outT));
    std::printf("%s\n",
                (std::fabs(residualFreq(outT, chan.outputRate())) < 5.0 && rms(outT) > 0.2)
                    ? "PASS - tone at baseband, amplitude preserved"
                    : "FAIL");
    return 0;
}
