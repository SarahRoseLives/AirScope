/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef COARSEFREQESTIMATE_H
#define COARSEFREQESTIMATE_H

#include <vector>

#include "fftwrapper.h"

typedef FFTWrapper<double> FFT;
typedef std::complex<double> cpx_type;

class CoarseFreqEstimate
{
public:
    CoarseFreqEstimate();
    ~CoarseFreqEstimate();
    void setSettings(int coarsefreqest_fft_power,double lockingbw,double fb,double Fs);
    void bigchange();
    void ProcessBasebandData(const std::vector<cpx_type> &data);
    double getFreqOffsetEst() const { return freq_offset_est; }
private:
    FFT *fft;
    FFT *ifft;
    std::vector<cpx_type> out;
    std::vector<cpx_type> in;
    std::vector<double> window;
    std::vector<double> y;
    std::vector<double> z;
    double nfft;
    double Fs;
    int coarsefreqest_fft_power;
    double hzperbin;
    int startbin,stopbin;
    double lockingbw;
    double fb;
    int expectedpeakbin;
    double freq_offset_est;
    int emptyingcountdown;
};

#endif // COARSEFREQESTIMATE_H
