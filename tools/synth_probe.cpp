// Reconstruct a complex baseband from the known-good 600 bps audio (FFT
// analytic signal + shift to DC) and feed it through feedIQ, to test whether
// the feedIQ (Hilbert-USB) path decodes a signal we KNOW decodes via feedAudio.
#include "jfft.h"
#include "jaero_demod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
extern "C" { double oqpsk_lockingbw = 0.0; }
static uint32_t rd32(const unsigned char* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t rd16(const unsigned char* p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint64_t g_dec=0, g_ac=0;
static void decCb(const uint8_t*,int,int,void*){g_dec++;}
static void acCb(const uint8_t*,int,int,uint32_t,uint8_t,uint8_t,uint8_t,int,void*){g_ac++;}

int main(int argc, char** argv)
{
    const char* path = (argc>1)?argv[1]:"build\\600.wav";
    int baud = (argc>2)?atoi(argv[2]):600;
    double fc = (argc>3)?atof(argv[3]):1000.0; // audio freq_center

    std::ifstream f(path, std::ios::binary);
    unsigned char hdr[12]; f.read((char*)hdr,12);
    int ch=1,bits=16; double rate=48000; uint64_t dataOff=0,dataSz=0;
    while(f){unsigned char c[8];f.read((char*)c,8);if(!f)break;uint32_t sz=rd32(c+4);
        if(!std::memcmp(c,"fmt ",4)){std::vector<unsigned char>fm(sz);f.read((char*)fm.data(),sz);
            ch=rd16(fm.data()+2);rate=rd32(fm.data()+4);bits=rd16(fm.data()+14);if(sz&1)f.seekg(1,std::ios::cur);}
        else if(!std::memcmp(c,"data",4)){dataOff=(uint64_t)f.tellg();dataSz=sz;break;}
        else f.seekg(sz+(sz&1),std::ios::cur);}
    f.seekg((std::streamoff)dataOff,std::ios::beg);

    int frameBytes=ch*(bits/8);
    uint64_t nF=dataSz/frameBytes;
    std::vector<unsigned char> raw((size_t)nF*frameBytes);
    f.read((char*)raw.data(),(std::streamsize)raw.size());
    nF=(uint64_t)(f.gcount()/frameBytes);

    // Pad to power of two for one big FFT.
    int Np=1; while((uint64_t)Np < nF) Np<<=1;
    std::vector<std::complex<double>> z(Np, {0,0});
    for(uint64_t i=0;i<nF;++i) z[i]=std::complex<double>((double)(int16_t)rd16(raw.data()+(size_t)i*frameBytes),0.0);

    JFFT fft; int nf=Np; fft.init(nf);
    fft.fft(z.data(), Np, JFFT::FORWARD);
    // analytic: keep DC, double positive freqs (1..Np/2-1), zero negative.
    for(int i=1;i<Np/2;++i) z[i]*=2.0;
    for(int i=Np/2;i<Np;++i) z[i]=0.0;
    fft.fft(z.data(), Np, JFFT::INVERSE);
    double inv=1.0/Np; for(auto&v:z) v*=inv;

    // shift carrier (at fc) down to baseband, normalize amplitude.
    jaero_pmsk_demod_t* d = jaero_pmsk_create(rate,(double)baud,0,nullptr,nullptr);
    jaero_pmsk_set_acars_callback(d, acCb, nullptr);
    jaero_pmsk_set_decoded_callback(d, decCb, nullptr);

    const int B=8192;
    std::vector<double> iqd((size_t)B*2);
    double ph=0, inc=-2.0*M_PI*fc/rate;
    double norm = 1.0/32768.0;
    for(uint64_t off=0; off<nF; off+=B){
        int n=(int)std::min<uint64_t>(B, nF-off);
        for(int i=0;i<n;++i){
            std::complex<double> s=z[off+i]*std::complex<double>(std::cos(ph),std::sin(ph));
            ph+=inc; if(ph<-M_PI)ph+=2*M_PI;
            iqd[i*2]=s.real()*norm; iqd[i*2+1]=s.imag()*norm;
        }
        jaero_pmsk_feed_iq(d, iqd.data(), n);
    }
    std::printf("feedIQ(reconstructed baseband): lock=%d ebno=%.1f mse=%.3f decoded_SUs=%llu ACARS=%llu\n",
        jaero_pmsk_is_locked(d), jaero_pmsk_get_ebno(d), jaero_pmsk_get_mse(d),
        (unsigned long long)g_dec, (unsigned long long)g_ac);
    jaero_pmsk_destroy(d);
    return 0;
}
