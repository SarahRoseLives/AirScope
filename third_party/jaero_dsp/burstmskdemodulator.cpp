/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "burstmskdemodulator.h"
#include "coarsefreqestimate.h"

#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>

static inline int qRound(double v) { return (int)(v + 0.5); }

BurstMskDemodulator::BurstMskDemodulator()
{
    soft_bits_cb = NULL;
    soft_bits_user = NULL;

    afc=true;
    cpuReduce=false;

    Fs=48000;

    double freq_center=1000;
    lockingbw=1800;
    fb=1200;
    symbolspercycle=8;//about 8 for 50bps, 16 or 24 ish for 125 and up seems ok
    signalthreshold=0.6;//lower is less sensitive

    SamplesPerSymbol=Fs/fb;

    scatterpointtype=SPT_constellation;

    matchedfilter_re = new FIR(2*SamplesPerSymbol);
    matchedfilter_im = new FIR(2*SamplesPerSymbol);
    for(int i=0;i<2*SamplesPerSymbol;i++)
    {
        matchedfilter_re->FIRSetPoint(i,sin(M_PI*i/(2.0*SamplesPerSymbol))/(2.0*SamplesPerSymbol));
        matchedfilter_im->FIRSetPoint(i,sin(M_PI*i/(2.0*SamplesPerSymbol))/(2.0*SamplesPerSymbol));
    }

    agc = new AGC(1,Fs);
    agc2 = new AGC(SamplesPerSymbol*128.0/Fs,Fs);

    ebnomeasure = new MSKEbNoMeasure(0.15*Fs);//2 second ave //SamplesPerSymbol*125);//125 symbol averaging

    pointmean = new MovingAverage(100);

    mixer_center.SetFreq(freq_center,Fs);
    mixer2.SetFreq(freq_center,Fs);

    spectrumnfft=pow(2,13);
    spectrumcycbuff.resize(spectrumnfft);
    spectrumcycbuff_ptr=0;
    spectrumtmpbuff.resize(spectrumnfft);

    pointbuff.resize(100);
    pointbuff_ptr=0;
    mse=10.0;

    RxDataBits.reserve(1000);//unpacked

    // burst stuff

    fftr = new FFTr(pow(2.0,(ceil(log2(  128.0*SamplesPerSymbol   )))));
    mav1= new MovingAverage(SamplesPerSymbol*128);

    tridentbuffer_sz=qRound((256.0+16.0+16.0)*SamplesPerSymbol);//room for trident and preamble and a bit more
    tridentbuffer.resize(tridentbuffer_sz);
    tridentbuffer_ptr=0;

    d2.setLength(tridentbuffer_sz);//delay line for aligning demod to output of trident check and freq est

    startstop=-1;
    numPoints = 0;
    agc_settle_counter = 0;

    st_osc.SetFreq(fb/2,Fs);
    st_osc_half.SetFreq(fb/2.0,Fs);

    pt_d=0;
    msema = new MovingAverage(75);

    //st 1200hz resonator at 48000fps 4hz bw
    st_iir_resonator.a.resize(3);
    st_iir_resonator.b.resize(3);

    //st 600hz resonator at 48000fps 4hz bw
    st_iir_resonator.a[0]=1;
    st_iir_resonator.a[1]=-1.993312819378528;
    st_iir_resonator.a[2]=0.999476538254407;
    st_iir_resonator.b[0]=2.617308727964618e-04;
    st_iir_resonator.b[1]=0;
    st_iir_resonator.b[2]=-2.617308727964618e-04;

    ee=0.025;

    st_iir_resonator.init();
    delayt8.setdelay((SamplesPerSymbol)/2.0);

    startProcessing = 120;
    dcd = false;
    delayedsmpl.setLength(SamplesPerSymbol);

    maxvalbin = 0;
    trackfreq = 0;
    maxval = 0;
    vol_gain = 1;
    cntr = 0;
    sql = false;
    endRotation = 0;
    rotator = 1;
    rotator_freq = 0;
    symboltone_averotator = 1;
    symboltone_rotator = 1;
    carrier_rotation_est = 0;
    bbnfft = 0;
}


void BurstMskDemodulator::setAFC(bool state)
{
    afc=state;
}

void BurstMskDemodulator::setSQL(bool state)
{
    sql=state;
}
void BurstMskDemodulator::setCPUReduce(bool state)
{
    cpuReduce=state;
}
void BurstMskDemodulator::setScatterPointType(ScatterPointType type)
{
    scatterpointtype=type;
}

double  BurstMskDemodulator::getCurrentFreq()
{
    return mixer_center.GetFreqHz();
}

void BurstMskDemodulator::invalidatesettings()
{
    Fs=-1;
    fb=-1;
}

void BurstMskDemodulator::setSoftBitsCallback(burstmsk_soft_bits_cb cb, void *user)
{
    soft_bits_cb = cb;
    soft_bits_user = user;
}

void BurstMskDemodulator::setSettings(Settings _settings)
{
    Fs=_settings.Fs;
    lockingbw=_settings.lockingbw;
    fb=_settings.fb;

    if(fb>Fs)fb=Fs;//incase

    freq_center=_settings.freq_center;
    if(freq_center>((Fs/2.0)-(lockingbw/2.0)))freq_center=((Fs/2.0)-(lockingbw/2.0));
    symbolspercycle=_settings.symbolspercycle;
    signalthreshold=_settings.signalthreshold;
    SamplesPerSymbol=int(Fs/fb);
    bbnfft=pow(2,_settings.coarsefreqest_fft_power);
    mixer_center.SetFreq(freq_center,Fs);
    mixer2.SetFreq(freq_center,Fs);
    delete matchedfilter_re;
    delete matchedfilter_im;
    matchedfilter_re = new FIR(2*SamplesPerSymbol);
    matchedfilter_im = new FIR(2*SamplesPerSymbol);
    for(int i=0;i<2*SamplesPerSymbol;i++)
    {
        matchedfilter_re->FIRSetPoint(i,sin(M_PI*i/(2.0*SamplesPerSymbol))/(2.0*SamplesPerSymbol));
        matchedfilter_im->FIRSetPoint(i,sin(M_PI*i/(2.0*SamplesPerSymbol))/(2.0*SamplesPerSymbol));
    }

    delete agc;
    agc = new AGC(1,Fs);

    delete agc2;

    delete ebnomeasure;
    //keep the averaging time very short to allow for R packets
    ebnomeasure = new MSKEbNoMeasure(0.15*Fs);

    hfir.setSize(2048);

    pointbuff.resize(100);

    pointbuff_ptr=0;
    mse=10.0;

    a1.setdelay(SamplesPerSymbol/2);

    symboltone_averotator=1;
    rotator=1;

    cntr = 0;

    if(fb >= 1200)
    {

        bt_d1.setdelay(1.0*SamplesPerSymbol);
        bt_ma1.setLength(qRound(126.0*SamplesPerSymbol));//not sure whats best

        delete mav1;
        mav1= new MovingAverage(SamplesPerSymbol*126);
        bt_ma_diff.setdelay(SamplesPerSymbol*126);//not sure whats best
        pdet.setSettings((int)(SamplesPerSymbol*126.0/2.0),0.1);

        delete fftr;
        int N=4096*4*2;
        fftr = new FFTr(N);

        // changed trident from 120 to 194, half of 74 + 120
        tridentbuffer_sz=qRound((200.0)*SamplesPerSymbol);//room for trident and preamble and a bit more
        tridentbuffer.resize(tridentbuffer_sz);
        tridentbuffer_ptr=0;

        //set this delay so it lines up to end of the peak detector and thus starts at the start tone of the burst
        d1.setLength(((int) 289 * SamplesPerSymbol) + 20);//adjust so start of burst aligns
        d2.setLength(qRound(72+120.0)*SamplesPerSymbol);//delay line for aligning demod to output of trident check and freq est, was 120

        in.resize(N);
        out_base.resize(N);
        out_top.resize(N);
        out_abs_diff.resize(N/2);

        startstopstart=SamplesPerSymbol*(500);

        endRotation = (120+37)*SamplesPerSymbol;

        //st 600hz resonator at 48000fps 4hz bw
        st_iir_resonator.a.resize(3);
        st_iir_resonator.b.resize(3);

        st_iir_resonator.a[0]=1;
        st_iir_resonator.a[1]=-1.993312819378528;
        st_iir_resonator.a[2]=0.999476538254407;
        st_iir_resonator.b[0]=2.617308727964618e-04;
        st_iir_resonator.b[1]=0;
        st_iir_resonator.b[2]=-2.617308727964618e-04;

        ee=0.025;

        st_iir_resonator.init();


        startProcessing = 120;
        agc2 = new AGC(SamplesPerSymbol*128.0/Fs,Fs);

        delayt8.setdelay((SamplesPerSymbol)/2.0);
    }
    else
    {


        //600........
        delete mav1;
        mav1= new MovingAverage(SamplesPerSymbol*150);

        bt_ma_diff.setdelay(SamplesPerSymbol*150);//not sure whats best

        bt_d1.setdelay(1.0*SamplesPerSymbol);
        bt_ma1.setLength(qRound(150.0*SamplesPerSymbol));//not sure whats best

        pdet.setSettings((int)(SamplesPerSymbol*150.0/2.0),0.2);


        delete fftr;
        int N=4096*4*2;
        fftr = new FFTr(N);


        tridentbuffer_sz=qRound((224)*SamplesPerSymbol);//room for trident and preamble and a bit more
        tridentbuffer.resize(tridentbuffer_sz);
        tridentbuffer_ptr=0;

        d1.setLength(((int) 397 * SamplesPerSymbol) + 20);//adjust so start of burst aligns
        d2.setLength(qRound((72+150.0)*SamplesPerSymbol));//delay line for aligning demod to output of trident check and freq est

        in.resize(N);
        out_base.resize(N);
        out_top.resize(N);
        out_abs_diff.resize(N/2);

        startstopstart=SamplesPerSymbol*(500);


        //st 300hz resonator at 48000fps 5hz bw
        st_iir_resonator.a.resize(3);
        st_iir_resonator.b.resize(3);

        st_iir_resonator.a[0]=1;
        st_iir_resonator.a[1]= -1.991228154418550;
        st_iir_resonator.a[2]=0.997385427096603;
        st_iir_resonator.b[0]=0.001307286451699;
        st_iir_resonator.b[1]=0;
        st_iir_resonator.b[2]=-0.001307286451699;

        st_iir_resonator.init();
        ee=0.015;

        startProcessing = 150;
        endRotation = (startProcessing+56)*SamplesPerSymbol;


        agc2 = new AGC(SamplesPerSymbol*128.0/Fs,Fs);


        delayt8.setdelay((SamplesPerSymbol)/2.0);

    }


    st_osc.SetFreq(fb/2.0,Fs);
    st_osc_half.SetFreq(fb/2.0,Fs);

    dcd = false;
    delayedsmpl.setLength(SamplesPerSymbol);

}

void BurstMskDemodulator::CenterFreqChangedSlot(double freq_center)//spectrum display calls this when user changes the center freq
{
    if(freq_center<(0.75*fb))freq_center=0.75*fb;
    if(freq_center>(Fs/2.0-0.75*fb))freq_center=Fs/2.0-0.75*fb;
    mixer_center.SetFreq(freq_center,Fs);
    if(afc)mixer2.SetFreq(mixer_center.GetFreqHz());
    if((mixer2.GetFreqHz()-mixer_center.GetFreqHz())>(lockingbw/2.0))
    {
        mixer2.SetFreq(mixer_center.GetFreqHz()+(lockingbw/2.0));
    }
    if((mixer2.GetFreqHz()-mixer_center.GetFreqHz())<(-lockingbw/2.0))
    {
        mixer2.SetFreq(mixer_center.GetFreqHz()-(lockingbw/2.0));
    }
}

BurstMskDemodulator::~BurstMskDemodulator()
{
    delete matchedfilter_re;
    delete matchedfilter_im;
    delete agc;
    delete agc2;
    delete ebnomeasure;
    delete pointmean;
    delete mav1;
    delete fftr;
    delete msema;
}

void BurstMskDemodulator::feedAudio(const int16_t *samples, int num_samples, int sample_rate)
{
    (void)sample_rate;
    processAudio(samples, num_samples);
}

void BurstMskDemodulator::feedIQ(const double *iq_interleaved, int num_samples)
{
    /* Feed complex IQ directly — skip Hilbert transform.
     * The signal is already analytic (complex baseband from channelizer),
     * mixed up to freq_center Hz. No mirror image. */
    hfirbuff.resize(num_samples);
    for (int i = 0; i < num_samples; i++)
        hfirbuff[i] = cpx_type(iq_interleaved[i*2], iq_interleaved[i*2+1]);

    /* Skip hfir.update() — signal is already analytic.
     * Jump straight into the processing loop (same code path as processAudio
     * but after the Hilbert transform step). */
    processAnalytic();
}


void BurstMskDemodulator::processAudio(const short *ptr, int numofsamples)
{
    //make analytical signal
    hfirbuff.resize(numofsamples);

    for(int i=0;i<numofsamples;i++)
    {
        hfirbuff[i]=cpx_type(((double)(ptr[i]))/32768.0,0);
    }

    hfir.update(hfirbuff);

    processAnalytic();
}

void BurstMskDemodulator::processAnalytic()
{
    //run through each sample of analyitical signal
    for(int i=0;i<(int)hfirbuff.size();i++)
    {

        cpx_type cval=hfirbuff[i];

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
        double val_to_demod=d2.update_dont_touch(std::real(cval_d));

        //create burst timing signal
        cpx_type bt_corr = cval*std::conj(bt_d1.update(cval));
        cpx_type bt_avg = bt_ma1.UpdateSigned(bt_corr);
        double fastarm=std::abs(bt_avg);
        fastarm=mav1->UpdateSigned(fastarm);
        double bt_diff_val = bt_ma_diff.update(fastarm);
        fastarm -= bt_diff_val;
        if(fastarm<0)fastarm=0;
        double bt_sig=fastarm*fastarm;
        if(bt_sig>500)bt_sig=500;

        //peak detection to start filling trident buffer
        agc_settle_counter++;
        bool _pd = pdet.update(bt_sig);
        bool _periodic = (agc_settle_counter > (int)(2.0*Fs)) &&
                         (agc_settle_counter % (int)(0.3*Fs) == 0) &&
                         (tridentbuffer_ptr > tridentbuffer_sz);
        /* Suppress periodic re-lock when signal has been acquired.
         * Once the demod has locked (startstop > 0 and cntr past initial),
         * only allow re-lock if MSE is very bad (> 0.95 = total loss). */
        if(_periodic && startstop > 0 && cntr > (int)(startProcessing * SamplesPerSymbol) && mse < 0.95)
            _periodic = false;
        if(_pd || _periodic)
        {
            /* Emit burst marker to reset aerol's muw counter */
            RxDataBits.clear();
            RxDataBits.push_back(-1);
            if(soft_bits_cb)
                soft_bits_cb(RxDataBits.data(), (int)RxDataBits.size(), soft_bits_user);
            RxDataBits.clear();
        }
        if(_pd || _periodic)
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

            /* Skip if trident buffer hasn't been filled with real data */
            double _tbmax = 0;
            for (int _k = 0; _k < tridentbuffer_sz; _k++)
                if (fabs(tridentbuffer[_k]) > _tbmax) _tbmax = fabs(tridentbuffer[_k]);

            int size_base = 126;
            int size_top = 74;

            if(fb < 1200)
            {

                size_base = 150;
                size_top = 74;
            }

            if (_tbmax < 1e-6) { /* trident buffer empty, skip */ }
            else {

            //base
            in.assign(tridentbuffer.begin(), tridentbuffer.begin() + qRound(size_base*SamplesPerSymbol));
            in.resize(out_base.size(), 0.0);
            fftr->transform(in,out_base);

            //top
            int top_start = qRound(size_base*SamplesPerSymbol);
            int top_len = qRound(size_top*SamplesPerSymbol);
            if(top_start + top_len <= (int)tridentbuffer.size())
                in.assign(tridentbuffer.begin() + top_start, tridentbuffer.begin() + top_start + top_len);
            else
                in.assign(top_len, 0.0);
            in.resize(out_top.size(), 0.0);

            fftr->transform(in,out_top);

            double hzperbin=Fs/((double)out_base.size());

            // the distance between center freq bin and real/imag arm freq
            int peakspacingbins=qRound((0.5*fb)/hzperbin);

            //find strongest base loc, this is the start tone of the preamble
            int minvalbin=0;
            double minval = 0;
            for(int i=0;i< (int)out_base.size()/2 ;i++)
            {


                if(std::abs(out_base[i]) > minval)
                {
                    minval=std::abs(out_base[i]);
                    minvalbin=i;
                }

            }

            // find max top position. This is the 0-1 alternating part of the preable.
            double maxtop = 0;
            int maxtoppos = 0;
            int maxtopposhigh =0;
            double maxtophigh =0;

            //fixed bug fft output half is the conjugate complex so dont look at hence why /2
            for(int i=0; i < (int)out_top.size()/2; i++)
            {

                if(i > 50)
                {
                    if((i < minvalbin - (peakspacingbins/2)) &&  std::abs(out_top[i]) > maxtop)
                    {

                        maxtop = std::abs(out_top[i]);
                        maxtoppos = i;
                    }

                    if((i > minvalbin + (peakspacingbins/2)) &&  std::abs(out_top[i]) > maxtophigh)
                    {

                        maxtophigh = std::abs(out_top[i]);
                        maxtopposhigh = i;
                    }
                }
            }

            // ok so now we have the center bin, maxtoppos and one of the side peaks, should be to the left of the main peak, minvalbin
            int distfrompeak = std::abs(maxtoppos - minvalbin);

            // check if the side peak is within the expected range +/- 5%
            if(minval>500.0 && std::abs(distfrompeak-peakspacingbins) < std::abs(peakspacingbins/5) && !(dcd) && !(cntr>0 && cntr<(500*SamplesPerSymbol)))
            {
                //set gain given estimate
                vol_gain=1.4142*(500.0/(minval/3));

                double carrierphase=std::arg(out_base[minvalbin])-(M_PI/4.0);
                mixer2.SetPhaseDeg((180.0/M_PI)*carrierphase);
                mixer2.SetFreq(((maxtopposhigh+maxtoppos)/2)*hzperbin);

                CenterFreqChangedSlot(((maxtopposhigh+maxtoppos)/2)*hzperbin);

                pointbuff.assign(pointbuff.size(), cpx_type(0,0));
                pointmean->Zero();
                pointbuff_ptr=0;
                startstop=startstopstart;
                cntr=0;
                RxDataBits.clear();
                // indicate start of burst
                RxDataBits.push_back(-1);

                mse=0;
                msema->Zero();

                symboltone_averotator=1;
                symboltone_rotator=1;

                rotator=1;
                rotator_freq=0;
                carrier_rotation_est=0;

                st_iir_resonator.init();

                maxvalbin = 0;
                st_osc.SetPhaseDeg(0);
                st_osc_half.SetPhaseDeg(0);

            }
            } /* end if _tbmax */
        }//end of trident check

        //sample counting and signalstatus timout
        if(startstop>0)
        {

            if(cntr>=(startProcessing*SamplesPerSymbol))
            {
                startstop--;
            }
            if(cntr<1000000)cntr++;

            if(mse<signalthreshold)
            {
                startstop=startstopstart;
            }


        }

        if(startstop==0)
        {
            startstop--;

            cntr = 0;
            mse=1;

        }


        /* Always run the demod after initial carrier lock */
        if(cntr<1000000)cntr++;
        {

            cval= mixer2.WTCISValue()*(val_to_demod)*vol_gain;

            cpx_type sig2 = cpx_type(matchedfilter_re->FIRUpdateAndProcess(cval.real()),matchedfilter_im->FIRUpdateAndProcess(cval.imag()));

            if(cntr>(startProcessing*SamplesPerSymbol) && cntr<endRotation)
            {

                cpx_type symboltone_pt=sig2*symboltone_rotator*imag;
                double er=std::tanh(symboltone_pt.imag())*(symboltone_pt.real());
                symboltone_rotator=symboltone_rotator*std::exp(imag*er*0.5);
                symboltone_averotator=symboltone_averotator*0.999+0.001*symboltone_rotator;

                symboltone_pt=cpx_type((symboltone_pt.real()),a1.update(symboltone_pt.real()));

                double progress = (double)cntr-(SamplesPerSymbol*(startProcessing));
                double goal = endRotation-(SamplesPerSymbol*startProcessing);
                progress = progress/goal;

                //x4 pll
                double st_err=std::arg((st_osc_half.WTCISValue())*std::conj(symboltone_pt));

                st_err*=0.5*(1.0-progress*progress);
                st_osc_half.AdvanceFractionOfWave(-(1.0/(2.0*M_PI))*st_err*0.05);
                st_osc.SetPhaseDeg(st_osc_half.GetPhaseDeg()+(360.0*(1.0-ee)));
            }

            sig2*=symboltone_averotator;
            rotator=rotator*std::exp(imag*rotator_freq);
            sig2*=rotator;


            //Measure ebno
            ebnomeasure->Update(std::abs(sig2));

            //AGC
            sig2*=agc2->Update(std::abs(sig2));

            //clipping
            double abval=std::abs(sig2);
            if(abval>2.84)sig2=(2.84/abval)*sig2;

            //normal symbol timer
            cpx_type pt_d = delayedsmpl.update_dont_touch(sig2);
            cpx_type pt_msk=cpx_type(sig2.real(), pt_d.imag());

            double st_eta = std::abs(pt_msk);
            st_eta=st_iir_resonator.update(st_eta);

            cpx_type st_m1=cpx_type(st_eta,-delayt8.update(st_eta));
            cpx_type st_out=st_osc.WTCISValue()*st_m1;

            double st_angle_error=std::arg(st_out);
            //acquire the symbol oscillation
            if(cntr>endRotation)
            {
                st_osc.AdvanceFractionOfWave(-st_angle_error*0.002/360.0);
            }

            //sample times
            if(st_osc.IfHavePassedPoint(ee))
            {

                //carrier tracking
                double ct_xt=tanh(sig2.imag())*sig2.real();
                double ct_xt_d=tanh(pt_d.real())*pt_d.imag();

                double ct_ec=ct_xt_d-ct_xt;
                if(ct_ec>M_PI)ct_ec=M_PI;
                if(ct_ec<-M_PI)ct_ec=-M_PI;
                if(ct_ec>M_PI_2)ct_ec=M_PI_2;
                if(ct_ec<-M_PI_2)ct_ec=-M_PI_2;
                if(cntr>(startProcessing*SamplesPerSymbol))
                {
                    rotator=rotator*std::exp(imag*ct_ec*0.25);//correct carrier phase
                    if(cntr>endRotation)
                    {
                        rotator_freq=rotator_freq+ct_ec*0.0001;//correct carrier frequency
                    }
                }

                //gui feedback (scatter points - skipped, no GUI)
                if(cntr >= (endRotation + 100*SamplesPerSymbol) && pointbuff_ptr<(int)pointbuff.size())
                {
                    if(pointbuff_ptr<(int)pointbuff.size())
                    {
                        ASSERTCH(pointbuff,pointbuff_ptr);
                        pointbuff[pointbuff_ptr]=pt_msk*0.75;
                        if(pointbuff_ptr<(int)pointbuff.size())pointbuff_ptr++;
                    }
                }

                //calc MSE of the points
                if(cntr>(startProcessing*SamplesPerSymbol))
                {
                    double tda=(fabs((pt_msk*0.75).real())-1.0);
                    double tdb=(fabs((pt_msk*0.75).imag())-1.0);
                    mse=msema->Update((tda*tda)+(tdb*tdb));
                }


                //soft bits
                double imagin = diffdecode.UpdateSoft(pt_msk.imag());

                int ibit=qRound((imagin)*127.0+128.0);
                if(ibit>255)ibit=255;
                if(ibit<0)ibit=0;

                RxDataBits.push_back((unsigned char)ibit);

                double real = diffdecode.UpdateSoft(pt_msk.real());

                real =- real;

                ibit=qRound((real)*127.0+128.0);

                if(ibit>255)ibit=255;
                if(ibit<0)ibit=0;


                RxDataBits.push_back((unsigned char)ibit);

                // push them out to decode
                if((int)RxDataBits.size() >= 12)
                {
                    if(soft_bits_cb)
                    {
                        soft_bits_cb(RxDataBits.data(), (int)RxDataBits.size(), soft_bits_user);
                    }
                    RxDataBits.clear();
                }

            }

            st_osc.WTnextFrame();
            st_osc_half.WTnextFrame();

            mixer2.WTnextFrame();
            mixer_center.WTnextFrame();

        }
    }

}
