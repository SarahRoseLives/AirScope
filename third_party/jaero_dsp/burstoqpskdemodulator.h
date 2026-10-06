/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef BURSTOQPSKDEMODULATOR_H
#define BURSTOQPSKDEMODULATOR_H

#include "DSP.h"
#include <vector>
#include <complex>
#include <assert.h>
#include <cstdint>

#include "fftrwrapper.h"

typedef FFTrWrapper<double> FFTr;
typedef std::complex<double> cpx_type;

// Callback type: called when soft bits are ready
typedef void (*burstoqpsk_soft_bits_cb)(const short *bits, int num_bits, void *user);

class BurstOqpskDemodulator
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
        double signalthreshold;
        bool channel_stereo;
        Settings()
        {
            coarsefreqest_fft_power=13;//2^coarsefreqest_fft_power
            freq_center=8000;//Hz
            lockingbw=10500;//Hz
            fb=10500;//bps
            Fs=48000;//Hz
            signalthreshold=0.6;
            channel_stereo=false;
        }
    };
    BurstOqpskDemodulator();
    ~BurstOqpskDemodulator();
    void setAFC(bool state);
    void setSQL(bool state);
    void setCPUReduce(bool state);
    void setSettings(Settings settings);
    void invalidatesettings();
    double getCurrentFreq();
    void setScatterPointType(ScatterPointType type);

    // Feed audio samples (mono, 16-bit signed, native byte order)
    void feedAudio(const int16_t *samples, int num_samples, int sample_rate);

    // Set callback for demodulated soft bits
    void setSoftBitsCallback(burstoqpsk_soft_bits_cb cb, void *user);

    //--L/R channel selection
    bool channel_select_other;

private:
    void CenterFreqChangedSlot(double freq_center);
    void writeDataSlot(const short *data, int numofsamples);

    burstoqpsk_soft_bits_cb soft_bits_cb;
    void *soft_bits_user;

    const cpx_type imag=cpx_type(0, 1);

    bool afc;
    bool sql;
    int scatterpointtype;

    double Fs;
    double freq_center;
    double lockingbw;
    double fb;
    double signalthreshold;

    double SamplesPerSymbol;
    bool insertpreamble;

    WaveTable mixer2;

    std::vector<double> spectrumcycbuff;
    int spectrumcycbuff_ptr;
    int spectrumnfft;

    std::vector<cpx_type> bbcycbuff;
    std::vector<cpx_type> bbtmpbuff;
    int bbcycbuff_ptr;
    int bbnfft;

    std::vector<cpx_type> pointbuff;
    int pointbuff_ptr;

//--symbol timing detection
    AGC *agc;

    //hilbert
    QJHilbertFilter hfir;
    std::vector<cpx_type> hfirbuff;

    //delay lines
    Delay< cpx_type > bt_d1;
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

    //fft for trident
    FFTr *fftr;
    std::vector<cpx_type> out_base,out_top;
    std::vector<double> out_abs_diff;
    std::vector<double> in;

//--

//--demod

    AGC *agc2;


    FIR *fir_re;
    FIR *fir_im;

    //st
    Delay<double> delays;
    Delay<double> delayt41;
    Delay<double> delayt42;
    Delay<double> delayt8;
    IIR st_iir_resonator;
    WaveTable st_osc;
    WaveTable st_osc_ref;
    WaveTable st_osc_quarter;
    Delay<double> a1;
    double ee;
    cpx_type symboltone_averotator;
    double carrier_rotation_est;

    cpx_type rotator;
    double rotator_freq;

    //ct
    IIR ct_iir_loopfilter;


    OQPSKEbNoMeasure *ebnomeasure;

    BaceConverter bc;
    std::vector<short> RxDataBits;//unpacked soft bits



    double mse;
    MovingAverage *msema;

    std::vector<cpx_type> phasepointbuff;
    int phasepointbuff_ptr;

//--




    DelayThing<cpx_type> rotation_bias_delay;
    MovingAverage *rotation_bias_ma;

    int startstopstart;

    //old static stuff
    cpx_type pt_d;
    int yui;
    cpx_type sig2_last;
    cpx_type symboltone_rotator;
    int startstop;
    double vol_gain;
    int cntr;
    double maxval;

    bool channel_stereo;

    bool cpuReduce;

};

#endif // BURSTOQPSKDEMODULATOR_H
