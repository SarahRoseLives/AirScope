#include "adsb/adsb_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace adsb
{

// ---------------------------------------------------------------------------
// CRC (Mode S polynomial 0xFFF409)
// ---------------------------------------------------------------------------
static constexpr uint32_t kGeneratorPoly = 0xFFF409;

struct ErrorInfo
{
    uint32_t syndrome = 0;
    int      errors = 0;
    int      bits[2] = {-1, -1};
};

static uint32_t g_crcTable[256];
static uint32_t g_singleBitSyndrome[kLongMsgBits];
static std::vector<ErrorInfo> g_shortErrors;
static std::vector<ErrorInfo> g_longErrors;

uint16_t g_magLut[256][256];

static int combinations(int n, int k)
{
    if (k == 0 || k == n) return 1;
    if (k > n) return 0;
    int result = 1;
    for (int i = 1; i <= k; ++i)
    {
        result = result * n / i;
        --n;
    }
    return result;
}

static int searchErrorInfo(const std::vector<ErrorInfo>& table, uint32_t syndrome)
{
    int lo = 0, hi = (int)table.size() - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (table[mid].syndrome < syndrome) lo = mid + 1;
        else if (table[mid].syndrome > syndrome) hi = mid - 1;
        else return mid;
    }
    return -1;
}

// Forward decl.
static int prepareSubTable(std::vector<ErrorInfo>& table, int n, int offset, int startBit,
                           int endBit, const ErrorInfo& base, int errorBit, int maxErrors);

static int flagCollisions(std::vector<ErrorInfo>& table, int offset, int startBit, int endBit,
                          uint32_t baseSyndrome, int errorBit, int firstError, int lastError)
{
    int count = 0;
    if (errorBit > lastError) return 0;
    for (int i = startBit; i < endBit; ++i)
    {
        uint32_t syn = baseSyndrome ^ g_singleBitSyndrome[i + offset];
        if (errorBit >= firstError)
        {
            int idx = searchErrorInfo(table, syn);
            if (idx >= 0 && table[idx].errors != -1)
            {
                ++count;
                table[idx].errors = -1;
            }
        }
        count += flagCollisions(table, offset, i + 1, endBit, syn, errorBit + 1, firstError,
                                lastError);
    }
    return count;
}

static int prepareSubTable(std::vector<ErrorInfo>& table, int n, int offset, int startBit,
                           int endBit, const ErrorInfo& base, int errorBit, int maxErrors)
{
    if (errorBit >= maxErrors) return n;
    for (int i = startBit; i < endBit; ++i)
    {
        table[n] = base;
        table[n].syndrome ^= g_singleBitSyndrome[i + offset];
        table[n].errors = errorBit + 1;
        table[n].bits[errorBit] = i;
        ++n;
        n = prepareSubTable(table, n, offset, i + 1, endBit, table[n - 1], errorBit + 1,
                            maxErrors);
    }
    return n;
}

static std::vector<ErrorInfo> prepareErrorTable(int bits, int maxCorrect, int maxDetect)
{
    std::vector<ErrorInfo> table;
    if (maxCorrect == 0) return table;

    int maxSize = 0;
    for (int i = 1; i <= maxCorrect; ++i)
        maxSize += combinations(bits - 5, i);
    table.assign((size_t)maxSize, ErrorInfo{});

    ErrorInfo entry;
    int used = prepareSubTable(table, 0, kLongMsgBits - bits, 5, bits, entry, 0, maxCorrect);
    table.resize((size_t)used);

    std::sort(table.begin(), table.end(),
              [](const ErrorInfo& a, const ErrorInfo& b) { return a.syndrome < b.syndrome; });

    // Duplicate syndromes are ambiguous: drop all of them.
    int dst = 0;
    for (int i = 0; i < used; )
    {
        int j = i;
        while (j + 1 < used && table[j + 1].syndrome == table[i].syndrome) ++j;
        if (j == i)
        {
            if (dst != i) table[dst] = table[i];
            ++dst;
        }
        i = j + 1;
    }
    table.resize((size_t)dst);

    if (maxDetect > maxCorrect)
    {
        int flagged = flagCollisions(table, kLongMsgBits - bits, 5, bits, 0, 1, maxCorrect + 1,
                                     maxDetect);
        if (flagged > 0)
        {
            int d = 0;
            for (size_t i = 0; i < table.size(); ++i)
            {
                if (table[i].errors != -1)
                {
                    if (d != (int)i) table[d] = table[i];
                    ++d;
                }
            }
            table.resize((size_t)d);
        }
    }
    return table;
}

void setFixBits(int fixBits)
{
    if (fixBits < 0) fixBits = 0;
    if (fixBits > 2) fixBits = 2;
    g_shortErrors = prepareErrorTable(kShortMsgBits, fixBits, fixBits);
    g_longErrors = prepareErrorTable(kLongMsgBits, fixBits, fixBits);
}

static uint32_t crcChecksum(const uint8_t* msg, int bits)
{
    int n = bits / 8;
    uint32_t rem = 0;
    for (int i = 0; i < n - 3; ++i)
    {
        rem = (rem << 8) ^ g_crcTable[msg[i] ^ (uint8_t)((rem & 0xFF0000) >> 16)];
        rem &= 0xFFFFFF;
    }
    rem = rem ^ ((uint32_t)msg[n - 3] << 16) ^ ((uint32_t)msg[n - 2] << 8) ^ (uint32_t)msg[n - 1];
    return rem;
}

static const ErrorInfo* crcDiagnose(uint32_t syndrome, int bitlen)
{
    if (syndrome == 0)
    {
        static const ErrorInfo noErrors;
        return &noErrors;
    }
    const std::vector<ErrorInfo>& table =
        (bitlen == kShortMsgBits) ? g_shortErrors : g_longErrors;
    if (table.empty()) return nullptr;
    int idx = searchErrorInfo(table, syndrome);
    return (idx >= 0) ? &table[idx] : nullptr;
}

static void crcFix(uint8_t* msg, const ErrorInfo* info)
{
    for (int i = 0; i < info->errors; ++i)
        msg[info->bits[i] >> 3] ^= (uint8_t)(1 << (7 - (info->bits[i] & 7)));
}

void buildMagLut()
{
    for (int i = 0; i < 256; ++i)
        for (int q = 0; q < 256; ++q)
        {
            double si = (double)(i - 127);
            double sq = (double)(q - 127);
            double mag = std::sqrt(si * si + sq * sq) * 360.0;
            if (mag > 65535.0) mag = 65535.0;
            g_magLut[i][q] = (uint16_t)mag;
        }
}

void init(int fixBits)
{
    for (int i = 0; i < 256; ++i)
    {
        uint32_t c = (uint32_t)i << 16;
        for (int j = 0; j < 8; ++j)
        {
            if (c & 0x800000) c = (c << 1) ^ kGeneratorPoly;
            else c = c << 1;
        }
        g_crcTable[i] = c & 0x00FFFFFF;
    }

    uint8_t msg[kLongMsgBytes] = {0};
    for (int i = 0; i < kLongMsgBits; ++i)
    {
        msg[i / 8] ^= (uint8_t)(1 << (7 - (i & 7)));
        g_singleBitSyndrome[i] = crcChecksum(msg, kLongMsgBits);
        msg[i / 8] ^= (uint8_t)(1 << (7 - (i & 7)));
    }

    buildMagLut();
    setFixBits(fixBits);
}

// ---------------------------------------------------------------------------
// Demodulator (dump1090 / goadsb demod-2400 algorithm)
// ---------------------------------------------------------------------------
static inline int slicePhase0(const uint16_t* m) { return 5 * (int)m[0] - 3 * (int)m[1] - 2 * (int)m[2]; }
static inline int slicePhase1(const uint16_t* m) { return 4 * (int)m[0] - (int)m[1] - 3 * (int)m[2]; }
static inline int slicePhase2(const uint16_t* m) { return 3 * (int)m[0] + (int)m[1] - 4 * (int)m[2]; }
static inline int slicePhase3(const uint16_t* m) { return 2 * (int)m[0] + 3 * (int)m[1] - 5 * (int)m[2]; }
static inline int slicePhase4(const uint16_t* m) { return (int)m[0] + 5 * (int)m[1] - 5 * (int)m[2] - (int)m[3]; }

static inline int absInt(int x) { return x < 0 ? -x : x; }

static inline int corrPhase0(const uint16_t* m) { return slicePhase0(m) * 26; }
static inline int corrPhase1(const uint16_t* m) { return slicePhase1(m) * 38; }
static inline int corrPhase2(const uint16_t* m) { return slicePhase2(m) * 38; }
static inline int corrPhase3(const uint16_t* m) { return slicePhase3(m) * 26; }
static inline int corrPhase4(const uint16_t* m) { return slicePhase4(m) * 19; }

static int correlateCheck0(const uint16_t* m)
{
    return absInt(corrPhase0(m)) + absInt(corrPhase2(m + 2)) + absInt(corrPhase4(m + 4)) +
           absInt(corrPhase1(m + 7)) + absInt(corrPhase3(m + 9));
}
static int correlateCheck1(const uint16_t* m)
{
    return absInt(corrPhase1(m)) + absInt(corrPhase3(m + 2)) + absInt(corrPhase0(m + 5)) +
           absInt(corrPhase2(m + 7)) + absInt(corrPhase4(m + 9));
}
static int correlateCheck2(const uint16_t* m)
{
    return absInt(corrPhase2(m)) + absInt(corrPhase4(m + 2)) + absInt(corrPhase1(m + 5)) +
           absInt(corrPhase3(m + 7)) + absInt(corrPhase0(m + 10));
}
static int correlateCheck3(const uint16_t* m)
{
    return absInt(corrPhase3(m)) + absInt(corrPhase0(m + 3)) + absInt(corrPhase2(m + 5)) +
           absInt(corrPhase4(m + 7)) + absInt(corrPhase1(m + 10));
}
static int correlateCheck4(const uint16_t* m)
{
    return absInt(corrPhase4(m)) + absInt(corrPhase1(m + 3)) + absInt(corrPhase3(m + 5)) +
           absInt(corrPhase0(m + 8)) + absInt(corrPhase2(m + 10));
}

static inline uint8_t bitv(int val, int pos) { return val > 0 ? (uint8_t)(1 << pos) : (uint8_t)0; }

static inline int messageLenByType(uint8_t df)
{
    switch (df)
    {
    case 0: case 4: case 5: case 11: return kShortMsgBits;
    default: return kLongMsgBits;
    }
}

static int messageScore(const uint8_t* msg, int bits)
{
    if (bits < kShortMsgBits) return -1;

    uint8_t df = msg[0] >> 3;
    bool validDF = false;
    switch (df)
    {
    case 0: case 4: case 5: case 11:
        if (bits == kShortMsgBits) validDF = true;
        break;
    case 16: case 17: case 18: case 20: case 21: case 22: case 24:
        if (bits == kLongMsgBits) validDF = true;
        break;
    }
    if (!validDF) return -2;

    uint32_t syndrome = crcChecksum(msg, bits);
    if (syndrome == 0) return 1000;

    // Only repair the two ADS-B extended-squitter DFs. Their parity is a pure
    // CRC, so a small syndrome really does mean a correctable bit error. Every
    // other DF overlays the ICAO address or interrogator code on the parity
    // (DF0/4/5/11/16/20/21), so "repairing" those just invents frames out of
    // noise.
    if (df != 17 && df != 18) return -1;

    const ErrorInfo* info = crcDiagnose(syndrome, bits);
    if (info != nullptr && info->errors > 0) return 350 + 150 * info->errors;
    return -1;
}

void demodulate2400(const uint16_t* mag, int mlen, std::vector<RawMessage>& out)
{
    uint8_t msg1[kLongMsgBytes] = {0};
    uint8_t msg2[kLongMsgBytes] = {0};

    for (int j = 0; j + 20 <= mlen; ++j)
    {
        const uint16_t* preamble = mag + j;

        if (!(preamble[0] < preamble[1] && preamble[12] > preamble[13]))
            continue;

        int high;
        uint32_t baseSignal, baseNoise;

        if (preamble[1] > preamble[2] && preamble[2] < preamble[3] && preamble[3] > preamble[4] &&
            preamble[8] < preamble[9] && preamble[9] > preamble[10] && preamble[10] < preamble[11])
        {
            high = ((int)preamble[1] + preamble[3] + preamble[9] + preamble[11] + preamble[12]) / 4;
            baseSignal = (uint32_t)preamble[1] + preamble[3] + preamble[9];
            baseNoise = (uint32_t)preamble[5] + preamble[6] + preamble[7];
        }
        else if (preamble[1] > preamble[2] && preamble[2] < preamble[3] && preamble[3] > preamble[4] &&
                 preamble[8] < preamble[9] && preamble[9] > preamble[10] && preamble[11] < preamble[12])
        {
            high = ((int)preamble[1] + preamble[3] + preamble[9] + preamble[12]) / 4;
            baseSignal = (uint32_t)preamble[1] + preamble[3] + preamble[9] + preamble[12];
            baseNoise = (uint32_t)preamble[5] + preamble[6] + preamble[7] + preamble[8];
        }
        else if (preamble[1] > preamble[2] && preamble[2] < preamble[3] && preamble[4] > preamble[5] &&
                 preamble[8] < preamble[9] && preamble[10] > preamble[11] && preamble[11] < preamble[12])
        {
            high = ((int)preamble[1] + preamble[3] + preamble[4] + preamble[9] + preamble[10] +
                    preamble[12]) / 4;
            baseSignal = (uint32_t)preamble[1] + preamble[12];
            baseNoise = (uint32_t)preamble[6] + preamble[7];
        }
        else if (preamble[1] > preamble[2] && preamble[3] < preamble[4] && preamble[4] > preamble[5] &&
                 preamble[9] < preamble[10] && preamble[10] > preamble[11] && preamble[11] < preamble[12])
        {
            high = ((int)preamble[1] + preamble[4] + preamble[10] + preamble[12]) / 4;
            baseSignal = (uint32_t)preamble[1] + preamble[4] + preamble[10] + preamble[12];
            baseNoise = (uint32_t)preamble[5] + preamble[6] + preamble[7] + preamble[8];
        }
        else if (preamble[2] > preamble[3] && preamble[3] < preamble[4] && preamble[4] > preamble[5] &&
                 preamble[9] < preamble[10] && preamble[10] > preamble[11] && preamble[11] < preamble[12])
        {
            high = ((int)preamble[1] + preamble[2] + preamble[4] + preamble[10] + preamble[12]) / 4;
            baseSignal = (uint32_t)preamble[4] + preamble[10] + preamble[12];
            baseNoise = (uint32_t)preamble[6] + preamble[7] + preamble[8];
        }
        else
        {
            continue;
        }

        if (baseSignal * 2 < 3 * baseNoise)
            continue;

        if (preamble[5] >= (uint16_t)high || preamble[6] >= (uint16_t)high ||
            preamble[7] >= (uint16_t)high || preamble[8] >= (uint16_t)high ||
            preamble[14] >= (uint16_t)high || preamble[15] >= (uint16_t)high ||
            preamble[16] >= (uint16_t)high || preamble[17] >= (uint16_t)high ||
            preamble[18] >= (uint16_t)high)
            continue;

        // Try all five sample phases (dump1090-fa behaviour): the single-phase
        // preselection used by older dump1090/goadsb misses messages.
        int firstPhase = 4;
        int lastPhase = 8;

        uint8_t bestMsg[kLongMsgBytes] = {0};
        int bestScore = -1;
        bool useMsg1 = true;

        for (int tryPhase = firstPhase; tryPhase <= lastPhase; ++tryPhase)
        {
            uint8_t* curMsg = useMsg1 ? msg1 : msg2;
            const uint16_t* phasePtr = mag + (j + 19 + tryPhase / 5);
            int phase = tryPhase % 5;
            int bytelen = kLongMsgBytes;

            for (int i = 0; i < bytelen; ++i)
            {
                uint8_t b = 0;
                switch (phase)
                {
                case 0:
                    b = (uint8_t)(bitv(slicePhase0(phasePtr), 7) | bitv(slicePhase2(phasePtr + 2), 6) |
                                  bitv(slicePhase4(phasePtr + 4), 5) | bitv(slicePhase1(phasePtr + 7), 4) |
                                  bitv(slicePhase3(phasePtr + 9), 3) | bitv(slicePhase0(phasePtr + 12), 2) |
                                  bitv(slicePhase2(phasePtr + 14), 1) | bitv(slicePhase4(phasePtr + 16), 0));
                    phase = 1; phasePtr += 19; break;
                case 1:
                    b = (uint8_t)(bitv(slicePhase1(phasePtr), 7) | bitv(slicePhase3(phasePtr + 2), 6) |
                                  bitv(slicePhase0(phasePtr + 5), 5) | bitv(slicePhase2(phasePtr + 7), 4) |
                                  bitv(slicePhase4(phasePtr + 9), 3) | bitv(slicePhase1(phasePtr + 12), 2) |
                                  bitv(slicePhase3(phasePtr + 14), 1) | bitv(slicePhase0(phasePtr + 17), 0));
                    phase = 2; phasePtr += 19; break;
                case 2:
                    b = (uint8_t)(bitv(slicePhase2(phasePtr), 7) | bitv(slicePhase4(phasePtr + 2), 6) |
                                  bitv(slicePhase1(phasePtr + 5), 5) | bitv(slicePhase3(phasePtr + 7), 4) |
                                  bitv(slicePhase0(phasePtr + 10), 3) | bitv(slicePhase2(phasePtr + 12), 2) |
                                  bitv(slicePhase4(phasePtr + 14), 1) | bitv(slicePhase1(phasePtr + 17), 0));
                    phase = 3; phasePtr += 19; break;
                case 3:
                    b = (uint8_t)(bitv(slicePhase3(phasePtr), 7) | bitv(slicePhase0(phasePtr + 3), 6) |
                                  bitv(slicePhase2(phasePtr + 5), 5) | bitv(slicePhase4(phasePtr + 7), 4) |
                                  bitv(slicePhase1(phasePtr + 10), 3) | bitv(slicePhase3(phasePtr + 12), 2) |
                                  bitv(slicePhase0(phasePtr + 15), 1) | bitv(slicePhase2(phasePtr + 17), 0));
                    phase = 4; phasePtr += 19; break;
                case 4:
                    b = (uint8_t)(bitv(slicePhase4(phasePtr), 7) | bitv(slicePhase1(phasePtr + 3), 6) |
                                  bitv(slicePhase3(phasePtr + 5), 5) | bitv(slicePhase0(phasePtr + 8), 4) |
                                  bitv(slicePhase2(phasePtr + 10), 3) | bitv(slicePhase4(phasePtr + 12), 2) |
                                  bitv(slicePhase1(phasePtr + 15), 1) | bitv(slicePhase3(phasePtr + 17), 0));
                    phase = 0; phasePtr += 20; break;
                }

                curMsg[i] = b;

                if (i == 0)
                {
                    int df = b >> 3;
                    switch (df)
                    {
                    case 0: case 4: case 5: case 11: bytelen = kShortMsgBytes; break;
                    case 16: case 17: case 18: case 20: case 21: case 24: break;
                    default: bytelen = 1; break;
                    }
                }
            }

            int score = messageScore(curMsg, bytelen * 8);
            if (score > bestScore)
            {
                bestScore = score;
                std::memcpy(bestMsg, useMsg1 ? msg1 : msg2, kLongMsgBytes);
                useMsg1 = !useMsg1;
            }
        }

        if (bestScore < 0)
            continue;

        int msglen = messageLenByType(bestMsg[0] >> 3);

        uint64_t sigSum = 0;
        int signalLen = msglen * 12 / 5;
        for (int k = 0; k < signalLen && j + 19 + k < mlen + 32; ++k)
        {
            uint32_t m = mag[j + 19 + k];
            sigSum += (uint64_t)m * (uint64_t)m;
        }
        double sigLevel = (double)sigSum / 65535.0 / 65535.0 / (double)signalLen;

        RawMessage rm;
        std::memcpy(rm.data, bestMsg, kLongMsgBytes);
        rm.score = bestScore;
        rm.sigLevel = sigLevel;
        rm.bits = msglen;
        out.push_back(rm);

        j += msglen * 12 / 5;
    }
}

// ---------------------------------------------------------------------------
// Message decode
// ---------------------------------------------------------------------------
// ME is the 7-byte (56-bit) extended squitter payload me[0..6], MSB-first.
static inline uint64_t meBits(const uint8_t* me)
{
    return ((uint64_t)me[0] << 48) | ((uint64_t)me[1] << 40) | ((uint64_t)me[2] << 32) |
           ((uint64_t)me[3] << 24) | ((uint64_t)me[4] << 16) | ((uint64_t)me[5] << 8) |
           (uint64_t)me[6];
}

static void decodeAircraftID(Decoded& dm, const uint8_t* me)
{
    static const char charset[] = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
    uint32_t chars1 = ((uint32_t)me[1] << 16) | ((uint32_t)me[2] << 8) | (uint32_t)me[3];
    uint32_t chars2 = ((uint32_t)me[4] << 16) | ((uint32_t)me[5] << 8) | (uint32_t)me[6];

    dm.callsign.clear();
    for (int i = 0; i < 8; ++i)
    {
        uint32_t ch;
        if (i < 4) ch = (chars1 >> ((3 - i) * 6)) & 0x3F;
        else       ch = (chars2 >> ((7 - i) * 6)) & 0x3F;
        if (ch < sizeof(charset) - 1)
            dm.callsign += charset[ch];
    }
    while (!dm.callsign.empty() && dm.callsign.back() == ' ')
        dm.callsign.pop_back();
}

// Mode A/C (Gillham) gray-code decode for the rare non-Q altitude field.
static int modeAToModeC(unsigned int modeA)
{
    unsigned int fiveHundreds = 0, oneHundreds = 0;
    if ((modeA & 0xFFFF8889) || ((modeA & 0x000000F0) == 0))
        return -9999;
    if (modeA & 0x0010) oneHundreds ^= 0x007;
    if (modeA & 0x0020) oneHundreds ^= 0x003;
    if (modeA & 0x0040) oneHundreds ^= 0x001;
    if ((oneHundreds & 5) == 5) oneHundreds ^= 2;
    if (oneHundreds > 5) return -9999;
    if (modeA & 0x0002) fiveHundreds ^= 0x0FF;
    if (modeA & 0x0004) fiveHundreds ^= 0x07F;
    if (modeA & 0x1000) fiveHundreds ^= 0x03F;
    if (modeA & 0x2000) fiveHundreds ^= 0x01F;
    if (modeA & 0x4000) fiveHundreds ^= 0x00F;
    if (modeA & 0x0100) fiveHundreds ^= 0x007;
    if (modeA & 0x0200) fiveHundreds ^= 0x003;
    if (modeA & 0x0400) fiveHundreds ^= 0x001;
    if (fiveHundreds & 1) oneHundreds = 6 - oneHundreds;
    return (int)(fiveHundreds * 5) + (int)oneHundreds - 13;
}

static int32_t decodeAC12Field(uint32_t ac12)
{
    if (ac12 & 0x10) // Q bit set: 25 ft increments
    {
        int n = (int)(((ac12 & 0x0FE0) >> 1) | (ac12 & 0x000F));
        return (int32_t)n * 25 - 1000;
    }
    int n = (int)(((ac12 & 0x0FC0) << 1) | (ac12 & 0x003F));
    n = modeAToModeC((unsigned int)n);
    if (n < -12) n = 0;
    else n = n * 100;
    return (int32_t)n;
}

static int32_t decodeBaroAltitude(const uint8_t* me)
{
    uint32_t ac12 = ((uint32_t)me[1] << 4) | ((uint32_t)me[2] >> 4);
    return decodeAC12Field(ac12 & 0x0FFF);
}

static int32_t decodeGNSSAltitude(const uint8_t* me)
{
    uint32_t ac12 = ((uint32_t)me[1] << 4) | ((uint32_t)me[2] >> 4);
    return (int32_t)std::lround((double)(ac12 & 0x0FFF) * 3.28084); // metres -> ft
}

static void decodeAirbornePosition(Decoded& dm, const uint8_t* me, bool gnss)
{
    uint64_t v = meBits(me);
    uint32_t cprLat = (uint32_t)((v >> 17) & 0x1FFFF);
    uint32_t cprLon = (uint32_t)(v & 0x1FFFF);

    dm.cprEven = (me[2] & 0x04) == 0; // ME bit 22 = CPR format
    dm.altitude = gnss ? decodeGNSSAltitude(me) : decodeBaroAltitude(me);
    dm.lat = (double)cprLat / 131072.0;
    dm.lon = (double)cprLon / 131072.0;
}

static double surfaceSpeed(uint8_t mov)
{
    if (mov == 0) return -1.0; // not available
    if (mov == 1) return 0.0;
    if (mov <= 12) return std::ldexp(0.125, (int)mov - 2);
    if (mov == 13) return 128.0; // >= 128 kt bucket
    if (mov <= 17) return std::ldexp(0.125, (int)(18 - mov)); // decelerating
    return 0.0;
}

static void decodeSurfacePosition(Decoded& dm, const uint8_t* me)
{
    uint64_t v = meBits(me);
    uint32_t cprLat = (uint32_t)((v >> 17) & 0x1FFFF);
    uint32_t cprLon = (uint32_t)(v & 0x1FFFF);

    uint8_t mov = (uint8_t)(((me[0] & 0x07) << 4) | (me[1] >> 4));
    uint32_t trk = ((uint32_t)(me[1] & 0x0F) << 3) | ((uint32_t)me[2] >> 5);
    dm.speed = surfaceSpeed(mov);
    dm.heading = (double)trk * 360.0 / 128.0;
    dm.cprEven = (me[2] & 0x04) == 0;
    dm.lat = (double)cprLat / 131072.0;
    dm.lon = (double)cprLon / 131072.0;
}

static void decodeVelocitySubsonic(Decoded& dm, const uint8_t* me, uint8_t subtype)
{
    uint32_t ewRaw = ((uint32_t)(me[1] & 0x03) << 8) | (uint32_t)me[2];
    uint32_t nsRaw = ((uint32_t)(me[3] & 0x7F) << 3) | ((uint32_t)me[4] >> 5);

    double ewv = 0, nsv = 0;
    if (ewRaw > 0)
    {
        ewv = (double)(ewRaw - 1);
        if (me[1] & 0x04) ewv = -ewv;
    }
    if (nsRaw > 0)
    {
        nsv = (double)(nsRaw - 1);
        if (me[3] & 0x80) nsv = -nsv;
    }
    if (subtype == 2) { ewv *= 4; nsv *= 4; }

    if (ewRaw > 0 || nsRaw > 0)
    {
        dm.speed = std::sqrt(ewv * ewv + nsv * nsv);
        dm.heading = std::atan2(ewv, nsv) * 180.0 / M_PI;
        if (dm.heading < 0) dm.heading += 360;
    }

    uint32_t vr = (((uint32_t)(me[4] & 0x07) << 6) | ((uint32_t)me[5] >> 2));
    if (vr > 0)
    {
        int32_t rate = (int32_t)(vr - 1) * 64;
        if (me[4] & 0x08) rate = -rate;
        dm.vertRate = rate;
    }
}

static void decodeVelocitySupersonic(Decoded& dm, const uint8_t* me, uint8_t subtype)
{
    uint32_t airSpeed = ((uint32_t)(me[3] & 0x7F) << 3) | ((uint32_t)me[4] >> 5);
    if (airSpeed > 0)
    {
        --airSpeed;
        if (subtype == 4) airSpeed <<= 2;
        dm.speed = (double)airSpeed;
    }
    if (me[1] & 0x04)
        dm.heading = (double)((((uint32_t)(me[1] & 0x03) << 8) | (uint32_t)me[2]) * 360) / 1024.0;
}

static void decodeExtendedSquitter(Decoded& dm, const uint8_t* me)
{
    dm.meType = me[0] >> 3;
    dm.meSubType = me[0] & 0x07;

    if (dm.meType >= 1 && dm.meType <= 4)       decodeAircraftID(dm, me);
    else if (dm.meType >= 5 && dm.meType <= 8)  decodeSurfacePosition(dm, me);
    else if (dm.meType >= 9 && dm.meType <= 18) decodeAirbornePosition(dm, me, false);
    else if (dm.meType == 19)
    {
        if (dm.meSubType == 1 || dm.meSubType == 2) decodeVelocitySubsonic(dm, me, dm.meSubType);
        else if (dm.meSubType == 3 || dm.meSubType == 4) decodeVelocitySupersonic(dm, me, dm.meSubType);
    }
    else if (dm.meType >= 20 && dm.meType <= 22) decodeAirbornePosition(dm, me, true);
}

void decode(const RawMessage& raw, Decoded& out)
{
    uint8_t msg[kLongMsgBytes];
    std::memcpy(msg, raw.data, kLongMsgBytes);

    out = Decoded{};
    out.bits = raw.bits;
    out.sigLevel = raw.sigLevel;
    out.df = msg[0] >> 3;

    int nbytes = raw.bits / 8;

    if (raw.bits == kShortMsgBits)
    {
        out.ca = msg[0] & 0x07;
        out.icao = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];
        if (out.df == 11)
            out.squawk = (uint16_t)((((uint16_t)msg[4] & 0x7F) << 6) | ((uint16_t)msg[5] >> 2));
        std::memcpy(out.raw, msg, kLongMsgBytes);
        out.rawBytes = nbytes;
        out.crc = true; // short frames validated by DF only
        return;
    }

    out.ca = msg[0] & 0x07;
    out.icao = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];

    uint32_t syndrome = crcChecksum(msg, raw.bits);
    bool corrected = false;
    if (syndrome == 0)
    {
        out.crc = true;
    }
    else
    {
        const ErrorInfo* info = crcDiagnose(syndrome, raw.bits);
        if (info != nullptr && info->errors > 0)
        {
            crcFix(msg, info);
            out.icao = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];
            out.crc = true;
            corrected = true;
        }
        else
        {
            std::memcpy(out.raw, msg, kLongMsgBytes);
            out.rawBytes = nbytes;
            return; // bad CRC
        }
    }

    const uint8_t* me = msg + 4;
    if (out.df == 17 || out.df == 18)
    {
        decodeExtendedSquitter(out, me);
        // A repaired frame whose type code is reserved is almost certainly a
        // noise phantom: drop it.
        if (corrected && (out.meType == 0 || out.meType > 22))
            out.crc = false;
    }

    std::memcpy(out.raw, msg, kLongMsgBytes);
    out.rawBytes = nbytes;
}

std::string Decoded::hex() const
{
    static const char* d = "0123456789ABCDEF";
    int n = rawBytes > 0 ? rawBytes : (bits / 8);
    std::string s;
    s.reserve((size_t)n * 2);
    for (int i = 0; i < n; ++i)
    {
        s += d[(raw[i] >> 4) & 0xF];
        s += d[raw[i] & 0xF];
    }
    return s;
}

} // namespace adsb
