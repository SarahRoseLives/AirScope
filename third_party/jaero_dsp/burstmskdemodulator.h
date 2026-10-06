/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef BURSTMSKDEMODULATOR_H
#define BURSTMSKDEMODULATOR_H

#include "DSP.h"

#include <vector>
#include <complex>
#include <cstdint>

#include "fftrwrapper.h"

typedef FFTrWrapper<double> FFTr;
typedef std::complex<double> cpx_type;

class CoarseFreqEstimate;

// Callback type: called when soft bits are ready
typedef void (*burstmsk_soft_bits_cb)(const short *bits, int num_bits, void *user);

class BurstMskDemodulator
{
public:
    enum ScatterPointType{ SPT_constellation, SPT_phaseoffseterror, SPT_phaseoffsetest, SPT_None};
    struct Settings
    {
        int coarsefreqest_fft_power;
        double freq_center;
        double lockingbw;
        double fb;
        double Fs;
        int symbolspercycle;
        double signalthreshold;
        Settings()
        {
            coarsefreqest_fft_power=13;//2^coarsefreqest_fft_power
            freq_center=1000;//Hz
            lockingbw=500;//Hz
            fb=125;//bps
            Fs=8000;//Hz
            symbolspercycle=16;
            signalthreshold=0.6;
        }
    };
    BurstMskDemodulator();
    ~BurstMskDemodulator();

    void setSettings(Settings settings);
    void invalidatesettings();
    void setAFC(bool state);
    void setSQL(bool state);
    void setCPUReduce(bool state);
    void setScatterPointType(ScatterPointType type);
    double getCurrentFreq();

    // Feed audio samples (mono, 16-bit signed, native byte order)
    void feedAudio(const int16_t *samples, int num_samples, int sample_rate);
    void feedIQ(const double *iq_interleaved, int num_samples);

    // Set callback for demodulated soft bits
    void setSoftBitsCallback(burstmsk_soft_bits_cb cb, void *user);

private:
    void CenterFreqChangedSlot(double freq_center);
    void processAudio(const short *data, int numofsamples);
    void processAnalytic();

    burstmsk_soft_bits_cb soft_bits_cb;
    void *soft_bits_user;

    WaveTable mixer_center;
    WaveTable mixer2;

    bool sql;

    int spectrumnfft,bbnfft;

    std::vector<double> spectrumcycbuff;
    std::vector<double> spectrumtmpbuff;
    int spectrumcycbuff_ptr;

    double Fs;
    double freq_center;
    double lockingbw;
    double fb;
    double symbolspercycle;
    double signalthreshold;

    double SamplesPerSymbol;

    FIR *matchedfilter_re;
    FIR *matchedfilter_im;

    AGC *agc;
    AGC *agc2;

    MSKEbNoMeasure *ebnomeasure;

    MovingAverage *pointmean;

    std::vector<cpx_type> pointbuff;
    int pointbuff_ptr;

    DiffDecode diffdecode;

    std::vector<short> RxDataBits;//unpacked

    double mse;

    MovingAverage *msema;

    bool afc;

    int scatterpointtype;

    BaceConverter bc;

    // trident detector stuff

    //hilbert
    QJHilbertFilter hfir;
    std::vector<cpx_type> hfirbuff;

    //delay lines
    Delay<cpx_type> bt_d1;
    Delay< double > bt_ma_diff;

    //MAs
    TMovingAverage< std::complex<double> > bt_ma1;
    MovingAverage *mav1;


    //Peak detection
    PeakDetector pdet;

    //delay for peak detection alignment
    DelayThing< std::complex<double> > d1;

    //delay for trident detection
    DelayThing<double> d2;

    //trident shape thing
    std::vector<double> tridentbuffer;
    int tridentbuffer_ptr;
    int tridentbuffer_sz;

    int maxvalbin;
    double trackfreq;

    //fft for trident
    FFTr *fftr;
    std::vector<cpx_type> out_base,out_top;
    std::vector<double> out_abs_diff;
    std::vector<double> in;
    double maxval;
    double vol_gain;
    int cntr;
    int agc_settle_counter;

    int startstopstart;
    int startstop;
    int numPoints;

    const cpx_type imag=cpx_type(0, 1);
    WaveTable st_osc;
    WaveTable st_osc_half;

    Delay<double> a1;

    double ee;
    cpx_type symboltone_averotator;
    cpx_type symboltone_rotator;
    double carrier_rotation_est;
    cpx_type pt_d;

    cpx_type rotator;
    double rotator_freq;

    //st
    IIR st_iir_resonator;

    Delay<double> delayt8;

    int endRotation;
    int startProcessing;
    bool dcd;

    DelayThing<cpx_type> delayedsmpl;

    bool cpuReduce;

};


#endif // BURSTMSKDEMODULATOR_H
