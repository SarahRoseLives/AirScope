// Digital downconverter: NCO mix to baseband + cascaded FIR decimation.
// Extracts one narrow channel from a wideband IQ stream and produces
// interleaved double IQ at ~targetRate for a JAERO demodulator.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class FirDecimator
{
public:
    void design(int factor, double inRate, double cutoffHz, int taps)
    {
        factor_ = factor;
        if (taps < factor * 2)
            taps = factor * 2;
        if ((taps & 1) == 0)
            ++taps; // odd length for symmetric linear phase
        h_.resize(taps);
        double fc = cutoffHz / inRate; // normalized (0..0.5)
        double sum = 0.0;
        int M = taps - 1;
        for (int n = 0; n < taps; ++n)
        {
            double x = n - M / 2.0;
            double sinc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
            double w = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / M); // Hamming
            h_[n] = sinc * w;
            sum += h_[n];
        }
        for (double& v : h_)
            v /= sum; // unity DC gain

        // Power-of-two ring buffer so we can wrap with a mask instead of a
        // (slow) per-sample integer modulo.
        size_t cap = 1;
        while (cap < (size_t)taps)
            cap <<= 1;
        mask_ = cap - 1;
        delay_.assign(cap, std::complex<double>(0, 0));
        wpos_ = 0;
        count_ = 0;
    }

    // Push one input sample; if an output is produced, sets *out and returns true.
    bool push(std::complex<double> in, std::complex<double>* out)
    {
        delay_[wpos_ & mask_] = in;
        ++wpos_;
        if (++count_ < factor_)
            return false;
        count_ = 0;

        std::complex<double> acc(0, 0);
        int L = (int)h_.size();
        uint64_t idx = wpos_ - 1; // most-recent sample
        for (int k = 0; k < L; ++k)
            acc += delay_[(idx - (uint64_t)k) & mask_] * h_[k];
        *out = acc;
        return true;
    }

private:
    int factor_ = 1;
    int count_ = 0;
    uint64_t wpos_ = 0;
    uint64_t mask_ = 0;
    std::vector<double> h_;
    std::vector<std::complex<double>> delay_;
};

class Ddc
{
public:
    // Fs: wideband input rate. offsetHz: channel freq - center freq.
    // targetRate: desired channel rate. channelBwHz: passband to keep.
    Ddc(double Fs, double offsetHz, double targetRate, double channelBwHz)
    {
        Fs_ = Fs;
        double inc = -2.0 * M_PI * offsetHz / Fs;
        rot_ = std::complex<double>(std::cos(inc), std::sin(inc));
        osc_ = std::complex<double>(1.0, 0.0);

        int totalDecim = (int)std::lround(Fs / targetRate);
        if (totalDecim < 1)
            totalDecim = 1;

        // Prefer a decimation whose prime factors are all small (<=7) so it
        // splits into clean multi-stage filters. A large prime factor (e.g. 43
        // for 2.048 MHz -> 48 kHz) would force one huge stage with poor
        // anti-aliasing, folding out-of-band noise into the channel.
        auto smallFactors = [](int n) {
            for (int p : {2, 3, 5, 7})
                while (n % p == 0)
                    n /= p;
            return n == 1;
        };
        if (totalDecim > 1 && !smallFactors(totalDecim))
        {
            for (int delta = 1; delta <= totalDecim / 2; ++delta)
            {
                if (totalDecim - delta >= 1 && smallFactors(totalDecim - delta))
                {
                    totalDecim -= delta;
                    break;
                }
                if (smallFactors(totalDecim + delta))
                {
                    totalDecim += delta;
                    break;
                }
            }
        }

        // Factor the total decimation into stages of <= 8.
        std::vector<int> factors;
        int rem = totalDecim;
        while (rem > 1)
        {
            int f = 1;
            for (int cand = 8; cand >= 2; --cand)
                if (rem % cand == 0)
                {
                    f = cand;
                    break;
                }
            if (f == 1)
            {
                factors.push_back(rem); // leftover prime-ish factor
                rem = 1;
            }
            else
            {
                factors.push_back(f);
                rem /= f;
            }
        }
        if (factors.empty())
            factors.push_back(1);

        double rate = Fs_;
        stages_.resize(factors.size());
        for (size_t i = 0; i < factors.size(); ++i)
        {
            double outRate = rate / factors[i];
            bool last = (i + 1 == factors.size());
            // Anti-alias for the stage; the final stage tightens to the channel.
            double cutoff = last ? std::min(channelBwHz * 0.5, outRate * 0.45)
                                 : outRate * 0.40;
            // Enough taps for solid stopband (the reference uses ~63/stage);
            // weak intermediate filters fold wideband noise into the channel.
            int taps = last ? 24 * factors[i] + 128 : 16 * factors[i] + 64;
            stages_[i].design(factors[i], rate, cutoff, taps);
            rate = outRate;
        }
        outRate_ = rate;
    }

    double outputRate() const { return outRate_; }

    // Retune the NCO to a new channel offset (Hz from center). Safe to call
    // from another thread only while the consumer isn't in process().
    void setOffset(double offsetHz)
    {
        double inc = -2.0 * M_PI * offsetHz / Fs_;
        rot_ = std::complex<double>(std::cos(inc), std::sin(inc));
    }

    // Process nComplex interleaved float IQ samples; append produced channel
    // samples as interleaved doubles to 'out'.
    void process(const float* iq, int nComplex, std::vector<double>& out)
    {
        processT(iq, nComplex, out);
    }
    // Same, but consume an interleaved double stream (for chaining a per-channel
    // DDC onto a shared front-end's decimated output).
    void process(const double* iq, int nComplex, std::vector<double>& out)
    {
        processT(iq, nComplex, out);
    }

private:
    template <typename T>
    void processT(const T* iq, int nComplex, std::vector<double>& out)
    {
        for (int i = 0; i < nComplex; ++i)
        {
            std::complex<double> s((double)iq[i * 2], (double)iq[i * 2 + 1]);
            s *= osc_;          // NCO mix to baseband
            osc_ *= rot_;       // rotate phasor
            if ((++oscCount_ & 1023) == 0)
                osc_ /= std::abs(osc_); // periodic renormalization

            feedStages(s, 0, out);
        }
    }

    void feedStages(std::complex<double> s, size_t stage, std::vector<double>& out)
    {
        if (stage >= stages_.size())
        {
            out.push_back(s.real());
            out.push_back(s.imag());
            return;
        }
        std::complex<double> o;
        if (stages_[stage].push(s, &o))
            feedStages(o, stage + 1, out);
    }

    double Fs_ = 0, outRate_ = 0;
    std::complex<double> osc_{1.0, 0.0}, rot_{1.0, 0.0};
    uint64_t oscCount_ = 0;
    std::vector<FirDecimator> stages_;
};
