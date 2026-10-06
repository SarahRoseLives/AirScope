#include "decode/decoder.h"

// Per-channel DDC IF rate. The historical Inmarsat demods were tuned for
// ~48 kHz; the VHF decoders added in later phases will set their own target.
static constexpr double kChanRate = 48000.0;

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId)
    : ddc_(subRate, chanFreqHz - subCenterHz, kChanRate, 6000.0),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId)
{
    ddcOut_.reserve(8192);
}

Decoder::~Decoder() = default;

void Decoder::process(const double* iq, int nComplex)
{
    // Phase 1: run the DDC and discard the output. The ACARS/AM-voice decoders
    // will consume ddcOut_ here once ported.
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
}

void Decoder::setFreq(double chanFreqHz)
{
    chanFreqHz_ = chanFreqHz;
    ddc_.setOffset(chanFreqHz - subCenterHz_);
}

int Decoder::getConstellation(double*, int) const
{
    return 0;
}
