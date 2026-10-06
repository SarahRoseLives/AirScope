// VHF ACARS decoder: 2400 bps AM-MSK demod + ARINC-618 framing.
//
// Ported from acarsdec (Thierry Leconte), GPL-compatible. The demod operates
// on the real AM envelope of the channelized signal at 12500 Hz, exactly as
// acarsdec does after its channelizer.
#pragma once

#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A decoded VHF ACARS message.
struct AcarsMsg
{
    char mode = 0;         // mode char (e.g. '2')
    std::string addr;      // aircraft address / registration
    std::string label;     // 2-char ACARS label
    char bid = 0;          // block id char
    std::string no;        // message sequence number (downlink only)
    std::string fid;       // flight id (downlink only)
    bool downlink = false;
    std::string text;      // printable body
    std::string raw;       // raw 7-bit body (for libacars)
    std::string hex;       // hex of the raw body
    int   err = 0;         // corrected bit errors
    float level = 0.0f;    // signal level (dB)
    double timeSec = 0.0;  // epoch seconds
};

class AcarsDecoder
{
public:
    using MsgCb = std::function<void(const AcarsMsg&)>;

    AcarsDecoder();
    explicit AcarsDecoder(double sampleRate);
    ~AcarsDecoder();

    void setCallback(MsgCb cb) { cb_ = std::move(cb); }

    // envelope[]: real AM envelope at 12500 Hz.
    void process(const float* envelope, int n);

    bool     locked() const;
    double   levelDb() const { return levelDb_; }
    uint64_t msgCount() const { return msgCount_; }

    struct Blk {
        int len = 0;
        int err = 0;
        float lvl = 0.0f;
        char txt[250] = {};
        unsigned char crc[2] = {};
    };

private:
    void putbit(float v);
    void decodeAcars();
    void resetAcars();
    void finishBlk();
    void emitParsed(Blk& b);

    std::vector<std::complex<double>> inb_;
    std::vector<float> mf_;
    double rate_ = 12500.0;
    int    flen_ = 11;
    int    flenO_ = 133;
    unsigned int idx_ = 0;
    double phi_ = 0.0;
    double df_ = 0.0;
    double clk_ = 0.0;
    double lvlSum_ = 0.0;
    int    bitCount_ = 0;
    unsigned int sym_ = 0;
    unsigned char outbits_ = 0;
    int    nbits_ = 8;
    enum State { WSYN, SYN2, SOH1, TXT, CRC1, CRC2, END };
    State  state_ = WSYN;
    std::unique_ptr<Blk> blk_;
    std::chrono::steady_clock::time_point blkTime_{};
    MsgCb  cb_;
    double levelDb_ = -120.0;
    uint64_t msgCount_ = 0;
    std::chrono::steady_clock::time_point lastMsg_{};
};
