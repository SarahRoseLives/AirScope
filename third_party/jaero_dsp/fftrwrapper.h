/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef FFTRWRAPPER_H
#define FFTRWRAPPER_H

#include <vector>
#include <complex>
#include "jfft.h"

template<typename T>
class FFTrWrapper
{
public:
    FFTrWrapper(int nfft,bool kissfft_scaling=true);
    ~FFTrWrapper();
    void transform(const std::vector<T> &in, std::vector< std::complex<T> > &out);
    void transform(const std::vector< std::complex<T> > &in, std::vector<T> &out);
private:
    int nfft;
    JFFT fft;
    bool kissfft_scaling;
};

#endif // FFTRWRAPPER_H
