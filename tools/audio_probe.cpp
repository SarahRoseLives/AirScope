// Feed a mono 16-bit audio WAV straight into the JAERO continuous MSK demod
// + AeroL (no DDC), to isolate whether demod+AeroL decode known-good audio.
#include "jaero_demod.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static uint32_t rd32(const unsigned char* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t rd16(const unsigned char* p){return (uint16_t)(p[0]|(p[1]<<8));}

extern "C" { double oqpsk_lockingbw = 0.0; }

static uint64_t g_bits = 0, g_sus = 0;
static void bitsCb(const unsigned char*, int n, int, void*) { g_bits += n; }
static void acarsCb(const uint8_t* d, int len, int, uint32_t aes, uint8_t, uint8_t, uint8_t, int dl, void*) {
    g_sus++;
    std::printf("  SU #%llu  AES=%06X %s  len=%d  \"", (unsigned long long)g_sus, aes, dl?"DL":"UL", len);
    for (int i=0;i<len && i<48;++i){ unsigned char c=d[i]; putchar((c>=32&&c<127)?c:'.'); }
    std::printf("\"\n");
}

int main(int argc, char** argv)
{
    const char* path = (argc>1)?argv[1]:"build\\600.wav";
    int baud = (argc>2)?atoi(argv[2]):600;

    std::ifstream f(path, std::ios::binary);
    if(!f){ std::printf("cannot open %s\n", path); return 1; }
    unsigned char hdr[12]; f.read((char*)hdr,12);
    int ch=1,bits=16; double rate=48000; uint64_t dataOff=0,dataSz=0;
    while(f){ unsigned char c[8]; f.read((char*)c,8); if(!f)break; uint32_t sz=rd32(c+4);
        if(!std::memcmp(c,"fmt ",4)){ std::vector<unsigned char>fm(sz); f.read((char*)fm.data(),sz);
            ch=rd16(fm.data()+2); rate=rd32(fm.data()+4); bits=rd16(fm.data()+14); if(sz&1)f.seekg(1,std::ios::cur);}
        else if(!std::memcmp(c,"data",4)){ dataOff=(uint64_t)f.tellg(); dataSz=sz; break;}
        else f.seekg(sz+(sz&1),std::ios::cur); }

    std::printf("audio: %d ch %d-bit %.0f Hz, baud=%d\n", ch, bits, rate, baud);
    f.seekg((std::streamoff)dataOff, std::ios::beg);

    jaero_pmsk_demod_t* d = jaero_pmsk_create(rate, (double)baud, 0, bitsCb, nullptr);
    jaero_pmsk_set_acars_callback(d, acarsCb, nullptr);

    int frameBytes = ch*(bits/8);
    uint64_t nFrames = dataSz/frameBytes;
    const int B = 8192;
    std::vector<unsigned char> raw((size_t)B*frameBytes);
    std::vector<int16_t> mono(B);
    uint64_t done=0;
    while(done<nFrames){
        int want=(int)std::min<uint64_t>(B, nFrames-done);
        f.read((char*)raw.data(), (std::streamsize)want*frameBytes);
        int gotF=(int)(f.gcount()/frameBytes); if(gotF<=0)break;
        for(int i=0;i<gotF;++i) mono[i]=(int16_t)rd16(raw.data()+(size_t)i*frameBytes); // ch0
        jaero_pmsk_feed_audio(d, mono.data(), gotF);
        done+=gotF;
    }

    std::printf("RESULT: lock=%d ebno=%.1f mse=%.3f soft_bits=%llu SUs=%llu\n",
        jaero_pmsk_is_locked(d), jaero_pmsk_get_ebno(d), jaero_pmsk_get_mse(d),
        (unsigned long long)g_bits, (unsigned long long)g_sus);
    jaero_pmsk_destroy(d);
    return 0;
}
