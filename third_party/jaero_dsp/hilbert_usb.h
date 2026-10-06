/*
 * 125-tap Hilbert FIR + 62-sample delay line = USB demod.
 * audio = delay(re) - hilbert(im), matches SDRReceiver's vfo::usb_demod().
 *
 * Used by both MskDemodulator::feedIQ and OqpskDemodulator::feedIQ to get
 * the same analytic-to-real conversion the ZMQ output path produces, which
 * measured ~1.5 dB better Eb/No than the prior `re*cos - im*sin` mixer on
 * our live RTL-SDR capture (ch12).
 */
#ifndef HILBERT_USB_H
#define HILBERT_USB_H

#include <cmath>
#include <vector>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class HilbertUSB {
public:
    static constexpr int TAPS = 125;
    static constexpr int DELAY = (TAPS - 1) / 2;   /* 62 */

    HilbertUSB() { reset(); }

    void reset() {
        design();
        for (int i = 0; i < TAPS; i++) hist[i] = 0.0f;
        for (int i = 0; i < DELAY + 1; i++) dbuf[i] = 0.0f;
        hidx = 0;
        didx = 0;
        bfo_phase = 0.0;
    }

    /* Mix baseband IQ up to freq_center with USB demod, scale to int16.
     * gain is the pre-clip multiplier (matches vfo::usb_demod: audio*gain*32768). */
    void process(const double *iq_interleaved, int n,
                 double Fs, double freq_center, double gain,
                 int16_t *out)
    {
        double phase_inc = 2.0 * M_PI * freq_center / Fs;
        for (int i = 0; i < n; i++) {
            double re = iq_interleaved[i * 2];
            double im = iq_interleaved[i * 2 + 1];

            if (phase_inc != 0.0) {
                double ca = std::cos(bfo_phase);
                double sa = std::sin(bfo_phase);
                double nr = re * ca - im * sa;
                double ni = re * sa + im * ca;
                re = nr; im = ni;
                bfo_phase += phase_inc;
                if (bfo_phase > 2.0 * M_PI) bfo_phase -= 2.0 * M_PI;
            }

            float delayed_re = delay_step((float)re);
            float hilb_im    = hilbert_step((float)im);
            double usb = (double)delayed_re - (double)hilb_im;

            double scaled = usb * gain * 32768.0;
            if (scaled >  32767.0) scaled =  32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            out[i] = (int16_t)scaled;
        }
    }

private:
    /* Hilbert coefficients: h[n] = (1 - cos(PI*(n - L/2))) / (PI*(n - L/2)) for n != L/2.
     * Half the taps are zero (every other one) and the nonzero taps are
     * antisymmetric: h[L-1-n] = -h[n]. We precompute only the nonzero odd-index
     * coefficients and use the antisymmetry to do one multiply per *pair* —
     * cutting the inner loop from 125 multiplies to 31. */
    static constexpr int PAIRS = 31;   /* 62 nonzero taps / 2 */
    static float *coeffs() {
        static float c[PAIRS];
        return c;
    }
    static bool& designed() { static bool d = false; return d; }

    float hist[TAPS];
    int   hidx;
    float dbuf[DELAY + 1];
    int   didx;
    double bfo_phase;

    static void design() {
        if (designed()) return;
        /* Hilbert kernel: h[n] = (1 - cos(PI*(n-62))) / (PI*(n-62)).
         * Zero at every EVEN n (cos(PI*even) = 1 -> numerator 0). Nonzero at
         * odd n. After SDRReceiver's reversal and normalisation, antisymmetry
         * h[124-n] = -h[n] still holds. So 62 nonzero taps form 31 pairs:
         *   (1, 123), (3, 121), ..., (61, 63). */
        float tmp[TAPS];
        float sumsq = 0;
        for (int n = 0; n < TAPS; n++) {
            if (n == TAPS / 2) tmp[n] = 0;
            else {
                double x = M_PI * (n - TAPS / 2);
                tmp[n] = (float)((1.0 - std::cos(x)) / x);
            }
            sumsq += tmp[n] * tmp[n];
        }
        float norm = std::sqrt(sumsq);
        /* reversed kernel rev[i] = tmp[TAPS-1-i] / norm; extract the 31 pair
         * coefficients from the left half (odd indices 1..61). */
        float *c = coeffs();
        for (int p = 0; p < PAIRS; p++) {
            int left = 2 * p + 1;                    /* 1, 3, ..., 61 */
            c[p] = tmp[TAPS - 1 - left] / norm;      /* rev[left] */
        }
        designed() = true;
    }

    /* Compute Hilbert convolution using antisymmetric pairing:
     *   sum = sum_p c[p] * (hist[left] - hist[right])
     * where (left, right) pair odd coefficient positions (1..61, 123..63).
     * 31 multiplies instead of 125, identical result. */
    inline float hilbert_step(float im) {
        hist[hidx] = im;
        hidx = (hidx + 1) % TAPS;
        const float *c = coeffs();
        float sum = 0;
        for (int p = 0; p < PAIRS; p++) {
            int left_idx  = hidx + (2 * p + 1);      /* 1..61 */
            int right_idx = hidx + (TAPS - 2 - 2*p); /* 123..63 */
            if (left_idx  >= TAPS) left_idx  -= TAPS;
            if (right_idx >= TAPS) right_idx -= TAPS;
            sum += c[p] * (hist[left_idx] - hist[right_idx]);
        }
        return sum;
    }

    inline float delay_step(float re) {
        dbuf[didx] = re;
        didx = (didx + 1) % (DELAY + 1);
        return dbuf[didx];
    }
};

#endif
