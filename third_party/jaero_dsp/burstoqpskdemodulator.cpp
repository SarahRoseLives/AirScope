/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "burstoqpskdemodulator.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef SPECTRUM_FFT_POWER
#define SPECTRUM_FFT_POWER 13
#endif

static inline int qRound(double v) { return (int)(v + 0.5); }

BurstOqpskDemodulator::BurstOqpskDemodulator()
{
    soft_bits_cb = NULL;
    soft_bits_user = NULL;
    cpuReduce = false;

    spectrumnfft=pow(2,SPECTRUM_FFT_POWER);
    spectrumcycbuff.resize(spectrumnfft);
    spectrumcycbuff_ptr=0;

    mse=100;

    insertpreamble=false;

    bbnfft=pow(2,14);
    bbcycbuff.resize(bbnfft);
    bbcycbuff_ptr=0;
    bbtmpbuff.resize(bbnfft);

    Fs=48000;
    fb=10500;
    SamplesPerSymbol=2.0*Fs/fb;
    agc = new AGC(0.05,Fs);

    agc2 = new AGC(50.0/Fs,Fs);

    fftr = new FFTr(pow(2.0,(ceil(log2(  128.0*SamplesPerSymbol   )))));
    mav1= new MovingAverage(SamplesPerSymbol*128);

    rotation_bias_delay.setLength(256);
    rotation_bias_ma = new MovingAverage(256);

    //--demod (resonators and LPF hard coded for Fs==48000 and fb==10500)

    RootRaisedCosine rrc;
    rrc.design(1,55,Fs,fb/2.0);
    fir_re=new FIR(rrc.Points.size());
    fir_im=new FIR(rrc.Points.size());
    for(int i=0;i<(int)rrc.Points.size();i++)
    {
        fir_re->FIRSetPoint(i,rrc.Points[i]);
        fir_im->FIRSetPoint(i,rrc.Points[i]);
    }

    //st delays
    delays.setdelay(1);
    delayt41.setdelay(SamplesPerSymbol/4.0);
    delayt42.setdelay(SamplesPerSymbol/4.0);
    delayt8.setdelay(SamplesPerSymbol/8.0);//??

    //st 10500hz resonator at 48000fps
    st_iir_resonator.a.resize(3);
    st_iir_resonator.b.resize(3);

    //75hz
    st_iir_resonator.b[0]=0.0048847995518126464;
    st_iir_resonator.b[1]=0;
    st_iir_resonator.b[2]=-0.0048847995518126464;
    st_iir_resonator.a[0]=1;
    st_iir_resonator.a[1]=-0.3882746897971619;
    st_iir_resonator.a[2]=0.99023040089637471;
    st_iir_resonator.init();


    //st osc
    st_osc.SetFreq(fb,Fs);
    st_osc_ref.SetFreq(fb,Fs);
    st_osc_quarter.SetFreq(fb/4.0,Fs);

    //ct LPF
    ct_iir_loopfilter.a.resize(3);
    ct_iir_loopfilter.b.resize(3);
    ct_iir_loopfilter.b[0]=0.0010275610653672064;//0.0109734835527828;
    ct_iir_loopfilter.b[1]=0.0020551221307344128;//0.0219469671055655;
    ct_iir_loopfilter.b[2]=0.0010275610653672064;//0.0109734835527828;
    ct_iir_loopfilter.a[0]=1;
    ct_iir_loopfilter.a[1]=-1.9207386815577139  ;//-1.72040443455951;
    ct_iir_loopfilter.a[2]=  0.92509247310306331 ;//0.766899247885339;
    ct_iir_loopfilter.init();

    //BC
    bc.SetInNumberOfBits(1);
    bc.SetOutNumberOfBits(8);

    //rxdata
    RxDataBits.reserve(8000); // unpacked soft bits

    ebnomeasure = new OQPSKEbNoMeasure(SamplesPerSymbol*(256.0),Fs,fb);//256 symbol ave, Fs and fb

    msema = new MovingAverage(128);

    phasepointbuff.resize(1);
    phasepointbuff_ptr=0;


    //--


    pt_d=0;
    yui=0;
    sig2_last=0;
    symboltone_rotator=1;
    startstop=-1;
    vol_gain=1;
    cntr=0;
    maxval=0;
    channel_select_other=false;
    afc=true;
    sql=false;
    scatterpointtype=SPT_constellation;
    channel_stereo=false;
    freq_center=8000;
    lockingbw=10500;
    signalthreshold=0.6;
    symboltone_averotator=1;
    carrier_rotation_est=0;
    rotator=1;
    rotator_freq=0;
    ee=0.4;
    tridentbuffer_ptr=0;
    tridentbuffer_sz=0;
    startstopstart=0;
    pointbuff_ptr=0;


    //--


    Settings _settings;
    invalidatesettings();
    setSettings(_settings);


}

BurstOqpskDemodulator::~BurstOqpskDemodulator()
{
    delete agc;
    delete agc2;
    delete mav1;
    delete fftr;
    delete fir_re;
    delete fir_im;
    delete ebnomeasure;
    delete msema;
    delete rotation_bias_ma;
}

void BurstOqpskDemodulator::setAFC(bool state)
{
    afc=state;
}

void BurstOqpskDemodulator::setSQL(bool state)
{
    sql=state;
}

void BurstOqpskDemodulator::setCPUReduce(bool state)
{
    cpuReduce=state;

}

void BurstOqpskDemodulator::setScatterPointType(ScatterPointType type)
{
    scatterpointtype=type;
}

void BurstOqpskDemodulator::invalidatesettings()
{
    Fs=-1;
    fb=-1;
}

void BurstOqpskDemodulator::setSoftBitsCallback(burstoqpsk_soft_bits_cb cb, void *user)
{
    soft_bits_cb = cb;
    soft_bits_user = user;
}

void BurstOqpskDemodulator::setSettings(Settings _settings)
{
    Fs=_settings.Fs;
    lockingbw=_settings.lockingbw;
    fb=_settings.fb;
    freq_center=_settings.freq_center;
    if(freq_center>((Fs/2.0)-(lockingbw/2.0)))freq_center=((Fs/2.0)-(lockingbw/2.0));
    signalthreshold=_settings.signalthreshold;
    SamplesPerSymbol=2.0*Fs/fb;

    bbnfft=pow(2,_settings.coarsefreqest_fft_power);
    bbcycbuff.resize(bbnfft);
    bbcycbuff_ptr=0;
    bbtmpbuff.resize(bbnfft);

    channel_stereo=_settings.channel_stereo;

    mixer2.SetFreq(freq_center,Fs);

    delete agc;
    agc = new AGC(1,Fs);

    delete agc2;
    agc2 = new AGC(SamplesPerSymbol*64.0/Fs,Fs);

    hfir.setSize(2048);

    pointbuff.resize(128);
    pointbuff_ptr=0;

    bt_d1.setdelay(1.0*SamplesPerSymbol);
    bt_ma1.setLength(qRound(128.0*SamplesPerSymbol));//not sure whats best

    delete mav1;
    mav1= new MovingAverage(SamplesPerSymbol*128);

    bt_ma_diff.setdelay(SamplesPerSymbol*128);//not sure whats best

    d1.setLength((int)(SamplesPerSymbol*128.0*2.5-190));//adjust so start of burst aligns

    delete fftr;
    int N=4096*4*2;
    fftr = new FFTr(N);

    tridentbuffer_sz=qRound((256.0+16.0+16.0)*SamplesPerSymbol);//room for trident and preamble and a bit more
    tridentbuffer.resize(tridentbuffer_sz);
    tridentbuffer_ptr=0;

    d2.setLength(tridentbuffer_sz);//delay line for aligning demod to output of trident check and freq est

    in.resize(N);
    out_base.resize(N);
    out_top.resize(N);
    out_abs_diff.resize(N/2);

    pdet.setSettings((int)(SamplesPerSymbol*128.0/2.0),0.2);

    a1.setdelay(SamplesPerSymbol/2.0);
    ee=0.4;//0.4;
    symboltone_averotator=1;
    carrier_rotation_est=0;

    delete ebnomeasure;
    ebnomeasure = new OQPSKEbNoMeasure(SamplesPerSymbol*(256.0),Fs,fb);//256 symbol ave, Fs and fb

    rotator=1;

    startstopstart=SamplesPerSymbol*(1050);//(256+2000);//128);

    insertpreamble=false;

}

double  BurstOqpskDemodulator::getCurrentFreq()
{
    return mixer2.GetFreqHz();
}

void BurstOqpskDemodulator::CenterFreqChangedSlot(double freq_center)//spectrum display calls this when user changes the center freq
{
    (void)freq_center;
    //nothing
    return;
}

void BurstOqpskDemodulator::feedAudio(const int16_t *samples, int num_samples, int sample_rate)
{
    (void)sample_rate;
    writeDataSlot(samples, num_samples);
}

void BurstOqpskDemodulator::writeDataSlot(const short *data, int numofsamples)
{

    double lastmse=mse;

    //make analytical signal
    hfirbuff.resize(numofsamples);
    const short *ptr = data;
    for(int i=0;i<numofsamples;i++)
    {
        hfirbuff[i]=cpx_type(((double)(*ptr))/32768.0,0);
        ptr++;
    }
    hfir.update(hfirbuff);

    //run through each sample of analyitical signal
    for(int i=0;i<(int)hfirbuff.size();i++)
    {

        std::complex<double> cval=hfirbuff[i];

        //take orginal arm
        double dval=cval.real();

        //spectrum display for looks
        if(fabs(dval)>maxval)maxval=fabs(dval);
        spectrumcycbuff[spectrumcycbuff_ptr]=dval;
        spectrumcycbuff_ptr++;spectrumcycbuff_ptr%=spectrumnfft;

        //agc
        agc->Update(std::abs(cval));
        cval*=agc->AGCVal;

        //delayed val for trident alignment
        std::complex<double> cval_d=d1.update_dont_touch(cval);

        //delayed val for after trident check
        double val_to_demod=(d2.update_dont_touch(std::real(cval_d)));

        //create burst timing signal
        double fastarm=std::abs(bt_ma1.UpdateSigned(cval*std::conj(bt_d1.update(cval))));
        fastarm=mav1->UpdateSigned(fastarm);
        fastarm-=bt_ma_diff.update(fastarm);
        if(fastarm<0)fastarm=0;
        double bt_sig=fastarm*fastarm;
        if(bt_sig>500)bt_sig=500;

        //peak detection to start filling trident buffer and flash signal LED
        if(pdet.update(bt_sig))
        {
            tridentbuffer_ptr=0;
        }

        if(tridentbuffer_ptr<tridentbuffer_sz)//fill trident buffer
        {
            tridentbuffer[tridentbuffer_ptr]=std::real(cval_d);
            tridentbuffer_ptr++;
        }
         else if(tridentbuffer_ptr==tridentbuffer_sz)//trident buffer is now filled so now check for trident and carrier freq and phase and amplitude
         {
            tridentbuffer_ptr++;

            //base
            in.assign(tridentbuffer.begin(), tridentbuffer.begin() + qRound(128.0*SamplesPerSymbol));
            in.resize(out_base.size(), 0.0);
            fftr->transform(in,out_base);

            //top
            int top_start = qRound(128.0*SamplesPerSymbol);
            int top_len = qRound(128.0*SamplesPerSymbol);
            if(top_start + top_len <= (int)tridentbuffer.size())
                in.assign(tridentbuffer.begin() + top_start, tridentbuffer.begin() + top_start + top_len);
            else
                in.assign(top_len, 0.0);
            in.resize(out_top.size(), 0.0);
            fftr->transform(in,out_top);

            //diff
            for(int i=0;i<(int)out_abs_diff.size();i++)
            {
                out_abs_diff[i]=(std::abs(out_top[i])-std::abs(out_base[i]));
            }

            //find best trident loc
            double hzperbin=Fs/((double)out_base.size());
            double binpeakspacing=(0.25*fb)/hzperbin;
            int binpeakspacing_int=qRound(binpeakspacing);
            int firstbin=binpeakspacing_int;
            int lstbin=(int)out_base.size()/2-binpeakspacing_int;
            double maxval_local=out_abs_diff[firstbin-binpeakspacing_int]+out_abs_diff[firstbin+binpeakspacing_int]-out_abs_diff[firstbin];
            double maxvalbin_local=firstbin;
            for(int i=firstbin;i<lstbin;i++)
            {
                assert(i<(int)out_abs_diff.size());
                double testval=out_abs_diff[i-binpeakspacing_int]+out_abs_diff[i+binpeakspacing_int]-out_abs_diff[i];
                if(testval>maxval_local)
                {
                    maxval_local=testval;
                    maxvalbin_local=i;
                }
            }

            //find strongest base loc
            double minval2=std::abs(out_top[0])-std::abs(out_base[0]);
            for(int i=0;i<(int)out_base.size()/2;i++)
            {
                if((std::abs(out_top[i])-std::abs(out_base[i]))<minval2)
                {
                    minval2=std::abs(out_top[i])-std::abs(out_base[i]);
                }
            }

            //find strongest base loc
            double minval=std::abs(out_base[0]);
            double minvalbin=0;
            for(int i=0;i<(int)out_base.size()/2;i++)
            {
                if((std::abs(out_base[i]))>minval)
                {
                    minval=std::abs(out_base[i]);
                    minvalbin=i;
                }
            }

            //check that signal is strong enough and two locs are close else this is not a signal
            if((maxval_local>500.0)&&(fabs((((double)(maxvalbin_local-minvalbin)))*hzperbin)<20.0))
            {

                //set the demodulator carrier freq and phase using estimates
                double carrierphase=std::arg(out_base[(int)minvalbin])-(M_PI/4.0);
                mixer2.SetFreq(hzperbin*minvalbin);
                mixer2.SetPhaseDeg((180.0/M_PI)*carrierphase);

                //set gain given estimate
                vol_gain=1.4142*500.0/minval;

                //set when we want to store points for display
                pointbuff_ptr=-128-128;

                //reset the rest
                st_osc.SetFreq(st_osc_ref.GetFreqHz());
                st_osc.SetPhaseDeg(0);
                st_osc_ref.SetPhaseDeg(0);
                st_iir_resonator.init();
                ct_iir_loopfilter.init();
                pointbuff.assign(pointbuff.size(), cpx_type(0,0));
                startstop=startstopstart;
                cntr=0;
                rotator=1;
                insertpreamble=true;
                rotator_freq=0;
                symboltone_averotator=1;
                carrier_rotation_est=0;
                mse=0;
                msema->Zero();

            }

         }//end of trident check

        //mix
        cpx_type cval_dd=mixer2.WTCISValue()*(vol_gain*val_to_demod);

        //rrc
        cpx_type sig2=cpx_type(fir_re->FIRUpdateAndProcess(cval_dd.real()),fir_im->FIRUpdateAndProcess(cval_dd.imag()));

        //sample counting and signalstatus timout
        if(startstop>0)
        {
            startstop--;
            if(cntr<1000000)cntr++;
            if(mse<0.75)
            {
                startstop=startstopstart;
            }

        }
        if(startstop==0)
        {
            startstop--;
        }

        if((cntr>((256-10)*SamplesPerSymbol))&&insertpreamble)
        {
            RxDataBits.push_back(-1);
            insertpreamble=false;
        }

        //symbol tone in preamble
        if((cntr>SamplesPerSymbol*(128+10))&&(cntr<((256-10)*SamplesPerSymbol)))
        {

            //0 to 1
            double progress=(((double)cntr)-(SamplesPerSymbol*(128+10)))/(((256-10)*SamplesPerSymbol)-(SamplesPerSymbol*(128+10)));

            //produce symbol tone circle (symboltone_pt) and calc carrier rotation
            cpx_type symboltone_pt=sig2*symboltone_rotator*imag;
            double er=std::tanh(symboltone_pt.imag())*(symboltone_pt.real());
            symboltone_rotator=symboltone_rotator*std::exp(imag*er*0.01);
            symboltone_averotator=symboltone_averotator*0.95+0.05*symboltone_rotator;
            symboltone_pt=cpx_type((symboltone_pt.real()),a1.update(symboltone_pt.real()));
            carrier_rotation_est=std::arg(symboltone_averotator);

            //x4 pll
            double st_err=std::arg((st_osc_quarter.WTCISValue())*std::conj(symboltone_pt));
            st_err*=1.5*(1.0-progress*progress);
            st_osc_quarter.AdvanceFractionOfWave(-(1.0/(2.0*M_PI))*st_err*0.1);
            st_osc.SetPhaseDeg((st_osc_quarter.GetPhaseDeg())*4.0+(360.0*ee));

        }

        //correct carrier phase

        sig2*=symboltone_averotator;

        rotator=rotator*std::exp(imag*rotator_freq);
        sig2*=rotator;

        double sig2abs = std::abs(sig2);

        //Measure ebno
        ebnomeasure->Update(sig2abs);

        //AGC
        sig2*=agc2->Update(sig2abs);

        //clipping
        double abval=std::abs(sig2);
        if(abval>2.84)sig2=(2.84/abval)*sig2;

        //normal symbol timer
        double st_diff=delays.update(abval*abval)-(abval*abval);
        double st_d1out=delayt41.update(st_diff);
        double st_d2out=delayt42.update(st_d1out);
        double st_eta=(st_d2out-st_diff)*st_d1out;
        st_iir_resonator.update(st_eta);
        if(cntr>SamplesPerSymbol*(128+128))st_eta=st_iir_resonator.y;
        cpx_type st_m1=cpx_type(st_eta,-delayt8.update(st_eta));
        cpx_type st_out=st_osc.WTCISValue()*st_m1;
        double st_angle_error=std::arg(st_out);

        //adjust sybol timing using normal tracking
        if(cntr>SamplesPerSymbol*(128+64))//???
        {
            st_osc.IncreseFreqHz(-st_angle_error*0.00000001);//st phase shift
            st_osc.AdvanceFractionOfWave(-st_angle_error*0.01/360.0);
        }
        if(st_osc.GetFreqHz()<(st_osc_ref.GetFreqHz()-0.1))st_osc.SetFreq((st_osc_ref.GetFreqHz()-0.1));
        if(st_osc.GetFreqHz()>(st_osc_ref.GetFreqHz()+0.1))st_osc.SetFreq((st_osc_ref.GetFreqHz()+0.1));

        //sample times
        if(st_osc.IfHavePassedPoint(ee))//?? 0.4 0.8 etc
        {

            //interpol
            double pt_last=st_osc.FractionOfSampleItPassesBy;
            double pt_this=1.0-pt_last;
            cpx_type pt=pt_this*sig2+pt_last*sig2_last;

            //for arm ambiguity resolution. bias calibrated for current settings
            double twospeed=-4.0*((std::fmod((st_osc_quarter.GetPhaseDeg())*2.0+(360.0*ee*0.5),360.0)/360.0)-(0.34046+0.4111*ee));
            bool even=true;
            if(twospeed<0)even=false;
            yui++;yui%=2;
            if(cntr<((128+128)*SamplesPerSymbol))
            {
                if((even&&yui==1)||(!even&&yui==0))
                {
                    yui++;yui%=2;
                }
            }

            if(!yui)pt_d=pt;
             else
             {

                cpx_type pt_qpsk=cpx_type(pt.real(),pt_d.imag());

                //carrier tracking BPSK 2x method
                double ct_xt=tanh(pt.imag())*pt.real();
                double ct_xt_d=tanh(pt_d.real())*pt_d.imag();
                double ct_ec=ct_xt_d-ct_xt;
                if(ct_ec>M_PI)ct_ec=M_PI;
                if(ct_ec<-M_PI)ct_ec=-M_PI;
                if(ct_ec>M_PI_2)ct_ec=M_PI_2;
                if(ct_ec<-M_PI_2)ct_ec=-M_PI_2;

                if(cntr>((128+10)*SamplesPerSymbol))//???
                {
                    rotator=rotator*std::exp(imag*ct_ec*0.1);//correct carrier phase
                    if(cntr>((128+10)*SamplesPerSymbol))rotator_freq=rotator_freq+ct_ec*0.0001;//correct carrier frequency
                }

                //gui feedback (scatter points - skipped, no GUI)
                if(pointbuff_ptr>0)
                {
                    if(pointbuff_ptr<(int)pointbuff.size())
                    {
                        ASSERTCH(pointbuff,pointbuff_ptr);
                        pointbuff[pointbuff_ptr]=pt_qpsk;
                        if(pointbuff_ptr<(int)pointbuff.size())pointbuff_ptr++;
                    }
                } else pointbuff_ptr++;

                //calc MSE of the points
                if(cntr>((128+10)*SamplesPerSymbol))
                {
                    double tda=(fabs(pt_qpsk.real())-1.0);
                    double tdb=(fabs(pt_qpsk.imag())-1.0);
                    mse=msema->Update((tda*tda)+(tdb*tdb));
                }


                if(startstop>0)//if signal then may as well demodulate
                {



                    int ibit=qRound(0.75*pt_qpsk.imag()*127.0+128.0);
                    if(ibit>255)ibit=255;
                    if(ibit<0)ibit=0;

                    RxDataBits.push_back((unsigned char)ibit);

                    ibit=qRound(0.75*pt_qpsk.real()*127.0+128.0);
                    if(ibit>255)ibit=255;
                    if(ibit<0)ibit=0;

                    RxDataBits.push_back((unsigned char)ibit);

                    //return the demodulated data (soft bit)

                    if((int)RxDataBits.size() >= 32)
                    {
                        if(!sql||mse<signalthreshold||lastmse<signalthreshold)
                        {
                            if(soft_bits_cb)
                            {
                                soft_bits_cb(RxDataBits.data(), (int)RxDataBits.size(), soft_bits_user);
                            }

                        }
                        RxDataBits.clear();
                    }

                }


             }


        }
        sig2_last=sig2;

        mixer2.WTnextFrame();
        st_osc.WTnextFrame();
        st_osc_ref.WTnextFrame();
        st_osc_quarter.WTnextFrame();

    }


    return;
}
