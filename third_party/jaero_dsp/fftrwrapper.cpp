/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "fftrwrapper.h"
#include <assert.h>

template<class T>
FFTrWrapper<T>::FFTrWrapper(int nfft, bool kissfft_scaling)
{
    this->kissfft_scaling=kissfft_scaling;
    this->nfft=nfft;
    /* JFFT's real FFT uses a complex FFT of size N/2 for N real samples.
     * The vector overload auto-reinits, but we init with N/2 upfront. */
    int half = nfft / 2;
    fft.init(half);
}

template<class T>
FFTrWrapper<T>::~FFTrWrapper()
{

}

template<class T>
void FFTrWrapper<T>::transform(const std::vector<T> &in, std::vector< std::complex<T> > &out)
{
    assert((int)in.size()==nfft);
    if((int)out.size() != nfft) out.resize(nfft);
    /* Use the vector overload which handles JFFT's size convention */
    std::vector<T> in_mut(in);
    fft.fft_real(in_mut, out);
    //for forward kissfft sets the remaining conj complex to 0
    if(kissfft_scaling)for(int i=(int)out.size()/2+1;i<(int)out.size();i++)out[i]=0;
}

template<class T>
void FFTrWrapper<T>::transform(const std::vector< std::complex<T> > &in, std::vector<T> &out)
{
    assert((int)in.size()==nfft);
    if((int)out.size() != nfft) out.resize(nfft);
    std::vector< std::complex<T> > in_mut(in);
    fft.ifft_real(in_mut, out);
    //for forward kissfft scales by nfft
    if(kissfft_scaling)for(int i=0;i<(int)out.size();i++)out[i]*=(double)out.size();
}

template class FFTrWrapper<double>;
