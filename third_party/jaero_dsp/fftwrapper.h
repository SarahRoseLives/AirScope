/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef FFTWRAPPER_H
#define FFTWRAPPER_H

#include <vector>
#include <complex>
#include "jfft.h"

//underlying fft still uses the type in the  kiss_fft_type in the c stuff
template<typename T>
class FFTWrapper
{
public:
    FFTWrapper(int nfft, bool inverse, bool kissfft_scaling=true);
    ~FFTWrapper();
    void transform(const std::vector< std::complex<T> > &in, std::vector< std::complex<T> > &out);
private:
    JFFT fft;
    int nfft;
    bool inverse;
    bool kissfft_scaling;
};

#endif // FFTWRAPPER_H
