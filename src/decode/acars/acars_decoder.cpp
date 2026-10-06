#include "decode/acars/acars_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "acars_syndrom.h"

namespace {

constexpr int    MAXPERR  = 3;
constexpr int    MFLTOVER = 12;
constexpr double PLLG     = 38e-4;
constexpr double PLLC     = 0.52;

constexpr unsigned char SYN = 0x16;
constexpr unsigned char SOH = 0x01;
constexpr unsigned char STX = 0x02;
constexpr unsigned char ETX = 0x83;
constexpr unsigned char ETB = 0x97;
constexpr unsigned char DLE = 0x7f;

// Recursively try to repair up to `pn` single-bit parity errors so the CRC
// matches. Mirrors acarsdec fixprerr().
int fixprerr(AcarsDecoder::Blk& blk, unsigned short crc, int* pr, int pn)
{
    if (pn > 0)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (fixprerr(blk, crc ^ syndrom[i + 8 * (blk.len - *pr + 1)], pr + 1, pn - 1))
            {
                blk.txt[*pr] ^= (1 << i);
                return 1;
            }
        }
        return 0;
    }
    if (crc == 0)
        return 1;
    for (int i = 0; i < 2 * 8; ++i)
        if (syndrom[i] == crc)
            return 1;
    return 0;
}

// Try to repair 1-2 bit errors anywhere in the block given the CRC syndrome.
int fixdberr(AcarsDecoder::Blk& blk, unsigned short crc)
{
    for (int i = 0; i < 2 * 8; ++i)
        if (syndrom[i] == crc)
            return 1;

    for (int k = 0; k < blk.len; ++k)
    {
        int bo = 8 * (blk.len - k + 1);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
            {
                if (i == j) continue;
                if ((crc ^ syndrom[i + bo] ^ syndrom[j + bo]) == 0)
                {
                    blk.txt[k] ^= (1 << i);
                    blk.txt[k] ^= (1 << j);
                    return 1;
                }
            }
    }
    return 0;
}

} // namespace

AcarsDecoder::AcarsDecoder()
    : AcarsDecoder(12500.0)
{
}

AcarsDecoder::AcarsDecoder(double sampleRate)
    : rate_(sampleRate > 1000.0 ? sampleRate : 12500.0)
{
    flen_ = (int)(rate_ / 1200.0) + 1;          // samples per 1200 Hz half-symbol
    flenO_ = flen_ * MFLTOVER + 1;
    inb_.assign((size_t)flen_, std::complex<double>(0.0, 0.0));

    // Matched filter for the 1200 Hz half-symbol (windowed raised cosine).
    mf_.resize((size_t)flenO_);
    for (int i = 0; i < flenO_; ++i)
    {
        float v = (float)std::cos(2.0 * M_PI * 600.0 / rate_ / MFLTOVER *
                                  (i - (flenO_ - 1) / 2.0));
        mf_[i] = (v < 0.0f) ? 0.0f : v;
    }
}

AcarsDecoder::~AcarsDecoder() = default;

void AcarsDecoder::resetAcars()
{
    state_ = WSYN;
    df_ = 0.0;
    nbits_ = 1;
    blk_.reset();
}

void AcarsDecoder::putbit(float v)
{
    outbits_ >>= 1;
    if (v > 0)
        outbits_ |= 0x80;

    nbits_--;
    if (nbits_ <= 0)
        decodeAcars();
}

void AcarsDecoder::process(const float* dm, int len)
{
    const float* h = mf_.data();
    int idx = (int)idx_;
    double p = phi_;

    for (int n = 0; n < len; ++n)
    {
        // VCO at 1800 Hz (the ACARS MSK subcarrier), freq-corrected by the PLL.
        double s = 1800.0 / rate_ * 2.0 * M_PI + df_;
        p += s;
        if (p >= 2.0 * M_PI) p -= 2.0 * M_PI;

        // Mixer: complex baseband of the 1800 Hz subcarrier.
        float in = dm[n];
        inb_[idx] = std::complex<double>((double)in, 0.0) *
                    std::exp(std::complex<double>(0.0, -p));
        idx = (idx + 1) % flen_;

        // Bit clock.
        clk_ += s;
        if (clk_ >= 3.0 * M_PI / 2.0 - s / 2.0)
        {
            clk_ -= 3.0 * M_PI / 2.0;

            // Matched filter (12x oversampled, phase selected by the clock).
            int o = (int)(MFLTOVER * (clk_ / s + 0.5));
            if (o > MFLTOVER) o = MFLTOVER;
            std::complex<double> v(0.0, 0.0);
            for (int j = 0, oo = o; j < flen_; ++j, oo += MFLTOVER)
                v += (double)h[oo] * inb_[(j + idx) % flen_];

            double lvl = std::abs(v);
            v /= (lvl + 1e-8);
            lvlSum_ += lvl * lvl / 4.0;
            bitCount_++;

            double vo, dphi;
            if (sym_ & 1)
            {
                vo = v.imag();
                dphi = (vo >= 0) ? -v.real() : v.real();
            }
            else
            {
                vo = v.real();
                dphi = (vo >= 0) ? v.imag() : -v.imag();
            }
            if (sym_ & 2)
                putbit((float)-vo);
            else
                putbit((float)vo);
            sym_++;

            // PLL loop filter.
            df_ = PLLC * df_ + (1.0 - PLLC) * PLLG * dphi;
        }
    }

    idx_ = (unsigned)idx;
    phi_ = p;
}

void AcarsDecoder::decodeAcars()
{
    unsigned char r = outbits_;

    switch (state_)
    {
    case WSYN:
        if (r == SYN) { state_ = SYN2; nbits_ = 8; return; }
        if (r == (unsigned char)~SYN) { sym_ ^= 2; state_ = SYN2; nbits_ = 8; return; }
        nbits_ = 1;
        return;

    case SYN2:
        if (r == SYN) { state_ = SOH1; nbits_ = 8; return; }
        if (r == (unsigned char)~SYN) { sym_ ^= 2; nbits_ = 8; return; }
        resetAcars();
        return;

    case SOH1:
        if (r == SOH)
        {
            if (!blk_)
                blk_ = std::make_unique<Blk>();
            blkTime_ = std::chrono::steady_clock::now();
            blk_->len = 0;
            blk_->err = 0;
            state_ = TXT;
            nbits_ = 8;
            lvlSum_ = 0.0;
            bitCount_ = 0;
            return;
        }
        resetAcars();
        return;

    case TXT:
        blk_->txt[blk_->len] = (char)r;
        blk_->len++;
        if ((numbits[(unsigned char)r] & 1) == 0)
        {
            blk_->err++;
            if (blk_->err > MAXPERR + 1) { resetAcars(); return; }
        }
        if (r == ETX || r == ETB) { state_ = CRC1; nbits_ = 8; return; }
        if (blk_->len > 20 && r == DLE)
        {
            blk_->len -= 3;
            blk_->crc[0] = (unsigned char)blk_->txt[blk_->len];
            blk_->crc[1] = (unsigned char)blk_->txt[blk_->len + 1];
            blk_->lvl = 10.0f * (float)std::log10(lvlSum_ / (bitCount_ > 0 ? bitCount_ : 1));
            emitParsed(*blk_);
            blk_.reset();
            df_ = 0.0;
            state_ = WSYN;
            nbits_ = 1;
            return;
        }
        if (blk_->len > 240) { resetAcars(); return; }
        nbits_ = 8;
        return;

    case CRC1:
        blk_->crc[0] = r;
        state_ = CRC2;
        nbits_ = 8;
        return;

    case CRC2:
        blk_->crc[1] = r;
        finishBlk();
        state_ = END;
        nbits_ = 8;
        return;

    case END:
        resetAcars();
        nbits_ = 8;
        return;
    }
}

void AcarsDecoder::finishBlk()
{
    if (!blk_)
        return;
    Blk& b = *blk_;
    b.lvl = 10.0f * (float)std::log10(lvlSum_ / (bitCount_ > 0 ? bitCount_ : 1));

    if (b.len < 13)
    {
        blk_.reset();
        return;
    }

    // Force STX at byte 12.
    b.txt[12] &= (ETX | STX);
    b.txt[12] |= (ETX & STX);

    // Parity check.
    int pn = 0;
    int pr[MAXPERR];
    for (int i = 0; i < b.len; ++i)
    {
        if ((numbits[(unsigned char)b.txt[i]] & 1) == 0)
        {
            if (pn < MAXPERR)
                pr[pn] = i;
            pn++;
        }
    }
    if (pn > MAXPERR)
    {
        blk_.reset();
        return;
    }
    b.err = pn;

    // CRC check.
    unsigned short crc = 0;
    for (int i = 0; i < b.len; ++i)
        update_crc(crc, (unsigned char)b.txt[i]);
    update_crc(crc, b.crc[0]);
    update_crc(crc, b.crc[1]);

    // Try to fix errors.
    if (pn)
    {
        if (fixprerr(b, crc, pr, pn) == 0) { blk_.reset(); return; }
    }
    else if (crc)
    {
        if (fixdberr(b, crc) == 0) { blk_.reset(); return; }
    }

    // Redo parity check and strip the parity bit.
    pn = 0;
    for (int i = 0; i < b.len; ++i)
    {
        if ((numbits[(unsigned char)b.txt[i]] & 1) == 0)
            pn++;
        b.txt[i] &= 0x7f;
    }
    if (pn)
    {
        blk_.reset();
        return;
    }

    emitParsed(b);
    blk_.reset();
}

void AcarsDecoder::emitParsed(Blk& b)
{
    AcarsMsg msg;
    msg.level = b.lvl;
    msg.err = b.err;
    msg.timeSec = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;

    int k = 0;
    msg.mode = b.txt[k++];

    for (int i = 0; i < 7; ++i, ++k)
        if (b.txt[k] != '.')
            msg.addr.push_back(b.txt[k]);

    k++; // ACK/NAK
    char l0 = b.txt[k++];
    char l1 = b.txt[k++];
    if (l1 == 0x7f) l1 = 'd';
    msg.label.push_back(l0);
    msg.label.push_back(l1);

    msg.bid = b.txt[k++];
    msg.downlink = (msg.bid >= '0' && msg.bid <= '9');

    unsigned char bs = (unsigned char)b.txt[k++];
    if (bs != 0x03)
    {
        if (msg.downlink)
        {
            for (int i = 0; i < 4 && k < b.len - 1; ++i, ++k)
                msg.no.push_back(b.txt[k]);
            for (int i = 0; i < 6 && k < b.len - 1; ++i, ++k)
                msg.fid.push_back(b.txt[k]);
        }
        int txtLen = b.len - k - 1;
        if (txtLen > 0)
        {
            msg.raw.assign(b.txt + k, (size_t)txtLen);
            for (int i = 0; i < txtLen; ++i)
            {
                unsigned char c = (unsigned char)b.txt[k + i];
                msg.text.push_back((c >= 0x20 && c < 0x7f) ? (char)c : ' ');
                char hx[4];
                std::snprintf(hx, sizeof(hx), "%02X", c);
                msg.hex += hx;
            }
            while (!msg.text.empty() && msg.text.back() == ' ')
                msg.text.pop_back();
        }
    }

    if (cb_)
        cb_(msg);
    ++msgCount_;
    lastMsg_ = std::chrono::steady_clock::now();
    levelDb_ = b.lvl;
}

bool AcarsDecoder::locked() const
{
    if (lastMsg_.time_since_epoch().count() == 0)
        return false;
    return std::chrono::steady_clock::now() - lastMsg_ < std::chrono::seconds(3);
}
