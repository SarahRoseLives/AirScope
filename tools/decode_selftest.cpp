// Headless decode test: read a real capture, find the strongest narrow
// carriers, run the Decoder on each at 600 and 1200 baud, report lock/Eb-N0/SUs.
#include "decode/decoder.h"
#include "decode/message_log.h"
#include "jfft.h"

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

static uint32_t rd32(const unsigned char* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0]|(p[1]<<8)); }

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1]
        : "D:\\baseband_1545692000Hz_22-44-23_16-06-2026.wav";
    double seconds = (argc > 2) ? atof(argv[2]) : 8.0;

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    unsigned char hdr[12]; f.read((char*)hdr, 12);
    int channels = 2, bits = 8; double Fs = 2048000; uint64_t dataOff = 0;
    while (f) {
        unsigned char ch[8]; f.read((char*)ch, 8); if (!f) break;
        uint32_t sz = rd32(ch + 4);
        if (!std::memcmp(ch, "fmt ", 4)) {
            std::vector<unsigned char> fmt(sz); f.read((char*)fmt.data(), sz);
            channels = rd16(fmt.data()+2); Fs = rd32(fmt.data()+4); bits = rd16(fmt.data()+14);
            if (sz & 1) f.seekg(1, std::ios::cur);
        } else if (!std::memcmp(ch, "data", 4)) { dataOff = (uint64_t)f.tellg(); break; }
        else f.seekg(sz + (sz & 1), std::ios::cur);
    }
    std::printf("file: %d ch %d-bit %.0f Hz\n", channels, bits, Fs);

    int frameBytes = channels * (bits / 8);
    uint64_t nFrames = (uint64_t)(seconds * Fs);
    std::vector<float> iq((size_t)nFrames * 2);
    f.seekg((std::streamoff)dataOff, std::ios::beg);
    std::vector<unsigned char> raw((size_t)nFrames * frameBytes);
    f.read((char*)raw.data(), (std::streamsize)raw.size());
    uint64_t got = (uint64_t)(f.gcount() / frameBytes);
    for (uint64_t i = 0; i < got; ++i) {
        const unsigned char* p = raw.data() + i * frameBytes;
        iq[i*2]   = ((int)p[0] - 128) * (1.0f/128.0f);
        iq[i*2+1] = (channels==2) ? ((int)p[1]-128)*(1.0f/128.0f) : 0.0f;
    }
    std::printf("loaded %llu frames\n", (unsigned long long)got);

    // Averaged power spectrum to locate carriers.
    int N = 16384;
    JFFT fft; int nf = N; fft.init(nf);
    std::vector<double> win(N);
    for (int i=0;i<N;++i){ double x=2.0*M_PI*i/(N-1); win[i]=0.42-0.5*cos(x)+0.08*cos(2*x); }
    std::vector<double> acc(N, 0.0);
    std::vector<std::complex<double>> buf(N);
    int blocks = 0;
    for (uint64_t off = 0; off + N <= got; off += N) {
        for (int i=0;i<N;++i) buf[i] = std::complex<double>(iq[(off+i)*2], iq[(off+i)*2+1]) * win[i];
        fft.fft(buf.data(), N, JFFT::FORWARD);
        for (int i=0;i<N;++i){ int s=(i+N/2)%N; acc[i]+=std::norm(buf[s]); }
        ++blocks;
    }
    for (auto& v : acc) v = 10.0*log10(v/blocks/N + 1e-20);
    std::vector<double> sorted = acc;
    std::nth_element(sorted.begin(), sorted.begin()+N/2, sorted.end());
    double med = sorted[N/2];

    // Find local-max peaks well above floor, away from DC.
    int guard = (int)(8000.0 / (Fs/N)); // ignore +/-8 kHz around DC
    std::vector<std::pair<double,int>> peaks;
    for (int i=2;i<N-2;++i){
        if (abs(i-N/2) < guard) continue;
        if (acc[i] > med+10 && acc[i]>=acc[i-1] && acc[i]>=acc[i+1] && acc[i]>acc[i-2] && acc[i]>acc[i+2])
            peaks.push_back({acc[i], i});
    }
    std::sort(peaks.begin(), peaks.end(), [](auto&a, auto&b){return a.first>b.first;});
    std::printf("floor=%.1f dB, %d candidate peaks\n", med, (int)peaks.size());

    int ntest = std::min((int)peaks.size(), 8);
    for (int baud : {1200, 10500}) {
        std::printf("=== baud %d ===\n", baud);
        for (int k=0;k<ntest;++k){
            int bin = peaks[k].second;
            double offHz = (bin - N/2) * Fs / N;
            for (int conj = 0; conj < 2; ++conj) {
                MessageLog log;
                Decoder dec(Fs, 0.0, offHz, baud, 1, &log);
                const int B = 32768;
                std::vector<float> blk((size_t)B * 2);
                for (uint64_t off=0; off+B<=got; off+=B) {
                    for (int i=0;i<B;++i){
                        blk[i*2]   = iq[(off+i)*2];
                        blk[i*2+1] = conj ? -iq[(off+i)*2+1] : iq[(off+i)*2+1];
                    }
                    dec.process(blk.data(), B);
                }
                std::printf("  off %+8.1f kHz  %.1f dB  conj=%d  lock=%d  ebno=%.1f  SUs=%llu\n",
                    offHz/1e3, peaks[k].first, conj, dec.locked()?1:0, dec.ebno(),
                    (unsigned long long)log.count());
            }
        }
    }
    return 0;
}
