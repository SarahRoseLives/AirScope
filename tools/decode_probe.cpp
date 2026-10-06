// Focused probe: DDC one carrier, feed JAERO pmsk demod directly with a
// soft-bits counter + ACARS counter, to see where the chain breaks.
#include "dsp/ddc.h"
#include "jaero_demod.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static uint32_t rd32(const unsigned char* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t rd16(const unsigned char* p){return (uint16_t)(p[0]|(p[1]<<8));}

extern "C" { double oqpsk_lockingbw = 0.0; }

static uint64_t g_bits = 0;
static uint64_t g_sus = 0;
static uint64_t g_dec = 0;

static void bitsCb(const unsigned char*, int n, int, void*) { g_bits += n; }
static void acarsCb(const uint8_t*, int, int, uint32_t, uint8_t, uint8_t, uint8_t, int, void*) { g_sus++; }
static void decodedCb(const uint8_t*, int, int, void*) { g_dec++; }

int main(int argc, char** argv)
{
    const char* path = argv[1];
    double offHz = (argc > 2) ? atof(argv[2]) : -224500.0;
    int baud = (argc > 3) ? atoi(argv[3]) : 1200;
    double seconds = (argc > 4) ? atof(argv[4]) : 20.0;

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    unsigned char hdr[12]; f.read((char*)hdr,12);
    int channels=2,bits=8; double Fs=2048000; uint64_t dataOff=0;
    while (f) { unsigned char ch[8]; f.read((char*)ch,8); if(!f)break; uint32_t sz=rd32(ch+4);
        if(!std::memcmp(ch,"fmt ",4)){std::vector<unsigned char>fm(sz);f.read((char*)fm.data(),sz);
            channels=rd16(fm.data()+2);Fs=rd32(fm.data()+4);bits=rd16(fm.data()+14);if(sz&1)f.seekg(1,std::ios::cur);}
        else if(!std::memcmp(ch,"data",4)){dataOff=(uint64_t)f.tellg();break;}
        else f.seekg(sz+(sz&1),std::ios::cur); }

    int fb = channels*(bits/8);
    uint64_t nF = (uint64_t)(seconds*Fs);
    std::vector<unsigned char> raw((size_t)nF*fb);
    f.seekg((std::streamoff)dataOff,std::ios::beg);
    f.read((char*)raw.data(),(std::streamsize)raw.size());
    uint64_t got = (uint64_t)(f.gcount()/fb);

    double bw = (baud==10500)?21000.0:6000.0;
    Ddc ddc(Fs, offHz, 48000.0, bw);
    double outRate = ddc.outputRate();

    jaero_pmsk_demod_t* d = jaero_pmsk_create(outRate, (double)baud, 0, bitsCb, nullptr);
    jaero_pmsk_set_acars_callback(d, acarsCb, nullptr);
    jaero_pmsk_set_decoded_callback(d, decodedCb, nullptr);

    std::printf("Fs=%.0f offHz=%.0f baud=%d ddcOut=%.1f got=%llu frames\n",
                Fs, offHz, baud, outRate, (unsigned long long)got);

    const int B = 32768;
    std::vector<float> blk((size_t)B*2);
    std::vector<double> iqd;
    for (uint64_t off=0; off+B<=got; off+=B) {
        for (int i=0;i<B;++i){ const unsigned char* p=raw.data()+(off+i)*fb;
            blk[i*2]=((int)p[0]-128)*(1.f/128.f); blk[i*2+1]=(channels==2)?((int)p[1]-128)*(1.f/128.f):0.f; }
        iqd.clear();
        ddc.process(blk.data(), B, iqd);
        if(!iqd.empty()) jaero_pmsk_feed_iq(d, iqd.data(), (int)(iqd.size()/2));
    }

    std::printf("RESULT: lock=%d ebno=%.1f mse=%.3f  soft_bits=%llu  decoded_SUs=%llu  ACARS=%llu\n",
                jaero_pmsk_is_locked(d), jaero_pmsk_get_ebno(d), jaero_pmsk_get_mse(d),
                (unsigned long long)g_bits, (unsigned long long)g_dec, (unsigned long long)g_sus);
    jaero_pmsk_destroy(d);
    return 0;
}
