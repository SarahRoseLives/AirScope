/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "coarsefreqestimate.h"
#include <assert.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

CoarseFreqEstimate::CoarseFreqEstimate()
{
    coarsefreqest_fft_power=13;
    lockingbw=500;//Hz
    fb=125;
    Fs=8000;//Hz

    nfft=pow(2,coarsefreqest_fft_power);
    fft = new FFT(nfft,false);
    ifft = new FFT(nfft,true);
    hzperbin=Fs/((double)nfft);
    out.resize(nfft);
    in.resize(nfft);
    y.resize(nfft);
    z.resize(nfft);
    startbin=round(lockingbw/hzperbin);
    stopbin=nfft-startbin;
    expectedpeakbin=round(fb/(2.0*hzperbin));
    emptyingcountdown=1;

    window.resize(nfft);
    std::fill(window.begin(), window.end(), 0.0);
    window[0]=1;
    for(int i=1;i<=startbin;i++)
    {
        double val=cos(M_PI_2*((double)i)/((double)startbin));
        val*=val;
        if((nfft-i)<0)break;
        if(i>=nfft)break;
        window[(int)(nfft-i)]=val;
        window[i]=val;
    }
}

void CoarseFreqEstimate::setSettings(int _coarsefreqest_fft_power,double _lockingbw,double _fb,double _Fs)
{
    coarsefreqest_fft_power=_coarsefreqest_fft_power;
    lockingbw=_lockingbw;//Hz
    fb=_fb;
    Fs=_Fs;//Hz
    delete ifft;
    delete fft;

    nfft=pow(2,coarsefreqest_fft_power);
    fft = new FFT(nfft,false);
    ifft = new FFT(nfft,true);
    hzperbin=Fs/((double)nfft);
    out.resize(nfft);
    in.resize(nfft);
    y.resize(nfft);
    z.resize(nfft);
    startbin=std::max(round(lockingbw/hzperbin),1.0);
    stopbin=nfft-startbin;
    expectedpeakbin=round(fb/(2.0*hzperbin));


    //use a raised cos window to favor signals in the middle rather than to the edges
    window.resize(nfft);
    std::fill(window.begin(), window.end(), 0.0);
    window[0]=1;
    for(int i=1;i<=startbin;i++)
    {
        double val=cos(M_PI_2*((double)i)/((double)startbin));
        val*=val;
        if((nfft-i)<0)break;
        if(i>=nfft)break;
        window[(int)(nfft-i)]=val;
        window[i]=val;
    }


}

CoarseFreqEstimate::~CoarseFreqEstimate()
{
    delete ifft;
    delete fft;
}

void CoarseFreqEstimate::bigchange()
{
    emptyingcountdown=4;
    for(int i=0;i<(int)nfft;i++)y[i]=20;
}

void CoarseFreqEstimate::ProcessBasebandData(const std::vector<cpx_type> &data)
{
    //fft size must be even. 2^n is best
    assert(nfft==(int)data.size());

    //remove high frequencies then square and do fft and shift (0hz bin is at nfft/2)
    fft->transform(data,out);

//what window would be better?
if(fb!=8400)for(int i=startbin;i<=stopbin;i++)out[i]=0;//this one is a boxcar window so can be pulled easily to the sides
 else for(int i=0;i<(int)nfft;i++)out[i]*=window[i];//this one will weight ones closer to the center more

    ifft->transform(out,in);
    for(int i=0;i<(int)nfft;i++)in[i]=in[i]*in[i];
    fft->transform(in,out);
    for(int i=0;i<(int)nfft/2;i++)std::swap(out[i+(int)nfft/2],out[i]);

    //smooth
    for(int i=0;i<(int)nfft;i++)y[i]=y[i]*0.9+0.1*10*log10(fmax(abs(out[i]),1));

    //fold and look for the fold that produces a big peak at the expected peak location
    double zmax=0;
    int zmaxloc=(int)nfft/2;
    assert(hzperbin!=0);
    for(int i=round((-lockingbw/hzperbin)+((double)((int)nfft/2)));i<round((lockingbw/hzperbin)+((double)((int)nfft/2)));i++)
    {
        if((i<0)||(i>=(int)z.size()))continue;//jic
        double val=0;
        for(int j=-1;j<=1;j++)
        {
            if(((i-expectedpeakbin-j)<0)||((i+expectedpeakbin+j)>=(int)y.size()))continue;//jic
            val+=(y[i-expectedpeakbin-j]+y[i+expectedpeakbin+j]);
        }
        z[i]=val;
        if(z[i]>zmax)
        {
            zmax=z[i];
            zmaxloc=i;
        }
    }
    freq_offset_est=-((double)(zmaxloc-(int)nfft/2))*hzperbin*0.5;

    if(emptyingcountdown<=0)
    {
        /* freq_offset_est is ready */
    }
    else
    {
        emptyingcountdown--;
        freq_offset_est=0;
    }

}
