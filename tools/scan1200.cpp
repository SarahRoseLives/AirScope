// Scan a capture for decodable 1200/600 MSK P-channels: find narrow carriers,
// run the real DDC + pmsk demod at each, report ebno/mse/decoded-SUs.
#include "dsp/ddc.h"
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
    const char* path = argv[1];
    int baud = (argc>2)?atoi(argv[2]):1200;
    double centerMHz = (argc>3)?atof(argv[3]):1546.0794;
    double seconds = (argc>4)?atof(argv[4]):12.0;

    std::ifstream f(path, std::ios::binary);
    if(!f){ std::printf("cannot open %s\n", path); return 1; }
    unsigned char hdr[12]; f.read((char*)hdr,12);
    int ch=2,bits=8; double Fs=2048000; uint64_t dataOff=0;
    while(f){unsigned char c[8];f.read((char*)c,8);if(!f)break;uint32_t sz=rd32(c+4);
        if(!std::memcmp(c,"fmt ",4)){std::vector<unsigned char>fm(sz);f.read((char*)fm.data(),sz);
            ch=rd16(fm.data()+2);Fs=rd32(fm.data()+4);bits=rd16(fm.data()+14);if(sz&1)f.seekg(1,std::ios::cur);}
        else if(!std::memcmp(c,"data",4)){dataOff=(uint64_t)f.tellg();break;}
        else f.seekg(sz+(sz&1),std::ios::cur);}
    int fb=ch*(bits/8);
    uint64_t nF=(uint64_t)(seconds*Fs);
    std::vector<unsigned char> raw((size_t)nF*fb);
    f.seekg((std::streamoff)dataOff,std::ios::beg);
    f.read((char*)raw.data(),(std::streamsize)raw.size());
    uint64_t got=(uint64_t)(f.gcount()/fb);
    std::vector<float> iq((size_t)got*2);
    for(uint64_t i=0;i<got;++i){const unsigned char* p=raw.data()+i*fb;
        iq[i*2]=((int)p[0]-128)*(1.f/128.f); iq[i*2+1]=(ch==2)?((int)p[1]-128)*(1.f/128.f):0.f;}
    std::printf("file %.0f Hz, %llu frames, baud=%d, center=%.4f MHz\n", Fs,(unsigned long long)got,baud,centerMHz);

    int N=16384; JFFT fft; int nf=N; fft.init(nf);
    std::vector<double> win(N); for(int i=0;i<N;++i){double x=2.0*M_PI*i/(N-1);win[i]=0.42-0.5*cos(x)+0.08*cos(2*x);}
    std::vector<double> acc(N,0.0); std::vector<std::complex<double>> bf(N); int blk=0;
    for(uint64_t off=0;off+N<=got;off+=N){for(int i=0;i<N;++i)bf[i]=std::complex<double>(iq[(off+i)*2],iq[(off+i)*2+1])*win[i];
        fft.fft(bf.data(),N,JFFT::FORWARD); for(int i=0;i<N;++i){int s=(i+N/2)%N;acc[i]+=std::norm(bf[s]);} ++blk;}
    for(auto&v:acc)v=10.0*log10(v/blk/N+1e-20);
    std::vector<double> srt=acc; std::nth_element(srt.begin(),srt.begin()+N/2,srt.end()); double med=srt[N/2];
    int guard=(int)(8000.0/(Fs/N));
    std::vector<std::pair<double,int>> peaks;
    for(int i=2;i<N-2;++i){if(abs(i-N/2)<guard)continue;
        if(acc[i]>med+8&&acc[i]>=acc[i-1]&&acc[i]>=acc[i+1]&&acc[i]>acc[i-2]&&acc[i]>acc[i+2])peaks.push_back({acc[i],i});}
    std::sort(peaks.begin(),peaks.end(),[](auto&a,auto&b){return a.first>b.first;});
    int ntest=std::min((int)peaks.size(),16);
    std::printf("floor=%.1f, testing %d peaks at baud %d\n", med, ntest, baud);

    for(int k=0;k<ntest;++k){
        double offHz=(peaks[k].second-N/2)*Fs/N;
        Ddc ddc(Fs, offHz, 48000.0, 6000.0);
        g_dec=0; g_ac=0;
        jaero_pmsk_demod_t* d=jaero_pmsk_create(ddc.outputRate(),(double)baud,0,nullptr,nullptr);
        jaero_pmsk_set_decoded_callback(d,decCb,nullptr); jaero_pmsk_set_acars_callback(d,acCb,nullptr);
        const int B=32768; std::vector<double> od;
        for(uint64_t off=0;off+B<=got;off+=B){od.clear();ddc.process(&iq[off*2],B,od);if(!od.empty())jaero_pmsk_feed_iq(d,od.data(),(int)(od.size()/2));}
        std::printf("  %.4f MHz (%+.1f kHz) %.1f dB  lock=%d ebno=%.1f mse=%.3f  SUs=%llu ACARS=%llu\n",
            centerMHz+offHz/1e6, offHz/1e3, peaks[k].first, jaero_pmsk_is_locked(d),
            jaero_pmsk_get_ebno(d), jaero_pmsk_get_mse(d), (unsigned long long)g_dec,(unsigned long long)g_ac);
        jaero_pmsk_destroy(d);
    }
    return 0;
}
