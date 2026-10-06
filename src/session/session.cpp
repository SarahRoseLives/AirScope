#include "core/app.h"
#include "core/main_funcs.h"

#include "sdr/wav_file_source.h"
#include "decode/acars/acars_freqs.h"
#ifdef HAS_AIRSPY
#include "sdr/airspy_source.h"
#endif

#include <cmath>
#include <string>

void updateFeed(App& app)
{
    app.feed.setFormat(app.outFormat);
    app.feed.setStationId(app.outStation);
    app.feed.setFileEnabled(app.outFile, app.outFilePath);
    app.feed.setUdpEnabled(app.outUdp, app.outUdpHost, app.outUdpPort);
    app.feed.setSbsEnabled(app.outSbs, app.outSbsPort);
    app.feed.pollSbs();

    for (auto& rp : app.rx)
    {
        if (!rp)
            continue;
        auto& log = rp->decoders.log();
        uint64_t at = log.count();
        if (at > rp->lastFeedCount)
        {
            auto snap = log.snapshot();
            uint64_t newN = at - rp->lastFeedCount;
            if (newN > snap.size()) newN = snap.size();
            for (size_t i = snap.size() - (size_t)newN; i < snap.size(); ++i)
                app.feed.feedAcars(snap[i]);
            rp->lastFeedCount = at;
        }
    }
}

static bool startReceiver(App& app, Receiver& r, bool feedIqRecorder, std::string& err)
{
    r.view.ring.clear();
    r.view.waterfall.clear();
    r.view.resetView = true;

    if (!r.src)
        r.src = makeSdrSource(r.mode);
    if (!r.src)
    {
        err = std::string("Source not available: ") + rxModeName(r.mode);
        return false;
    }

    IqRing* ring = &r.view.ring;
    DecoderManager* mgr = &r.decoders;
    AdsbManager* adsb = r.adsb.get();
    IqRecorder* iqr = feedIqRecorder ? &app.iqRecorder : nullptr;
    auto cb = [ring, mgr, adsb, iqr](const float* iq, int n) {
        ring->push(iq, (size_t)n);
        mgr->feed(iq, n);
        if (adsb)
            adsb->feed(iq, n);
        if (iqr)
        {
            iqr->prebuffer(iq, n);
            if (iqr->isRecording())
                iqr->write(iq, n);
        }
    };

    bool ok = false;
    if (r.mode == kRxRtl)
    {
        r.src->setSampleRate(kRates[r.rateIdx]);
        r.src->setCenterFreq(r.centerMHz * 1e6);
        r.src->setGain(r.autoGain ? -1.0 : (double)r.gainDb);
        r.src->setBiasTee(r.biasTee);
        r.src->setPpm((double)r.ppm);
        r.src->setDcBlock(app.dcBlock);
        ok = r.src->start(r.deviceIndex, cb, err);
    }
    else if (r.mode == kRxWav)
    {
        auto* w = dynamic_cast<WavFileSource*>(r.src.get());
        if (!w)
        {
            err = "WAV source error";
            return false;
        }
        w->setPath(r.wavPath);
        w->setLoop(r.wavLoop);
        w->setCenterFreq(r.centerMHz * 1e6);
        ok = r.src->start(0, cb, err);
    }
    else if (r.mode == kRxAirspy)
    {
#ifdef HAS_AIRSPY
        auto* a = dynamic_cast<AirspySource*>(r.src.get());
        if (a)
        {
            a->setSampleRate(kAirspyRates[r.apRateIdx]);
            a->setCenterFreq(r.centerMHz * 1e6);
            a->setGainMode(r.apGainMode);
            a->setSenseGain(r.apSense);
            a->setLinearGain(r.apLinear);
            a->setLnaGain(r.apLna);
            a->setMixerGain(r.apMixer);
            a->setVgaGain(r.apVga);
            a->setLnaAgc(r.apLnaAgc);
            a->setMixerAgc(r.apMixerAgc);
            a->setBiasTee(r.apBias);
            a->setPpm((double)r.ppm);
            a->setDcBlock(app.dcBlock);
        }
        ok = r.src->start(r.deviceIndex, cb, err);
#else
        err = "Airspy support not built";
        return false;
#endif
    }
    else if (r.mode == kRxSdrplay)
    {
        r.src->setSampleRate(kSdrplayRates[r.spRateIdx]);
        r.src->setCenterFreq(r.centerMHz * 1e6);
        r.src->setGain(r.spAgc ? -1.0 : (double)r.spGRdB);
        r.src->setBiasTee(r.spBias);
        r.src->setPpm((double)r.ppm);
        r.src->setDcBlock(app.dcBlock);
        ok = r.src->start(r.deviceIndex, cb, err);
    }

    if (ok)
    {
        r.decoders.removeAll();
        r.decoders.configure(r.src->sampleRate(), r.src->centerFreq());
        r.decoders.start();
        if (r.adsb)
        {
            r.adsb->configure(r.src->sampleRate(), r.src->centerFreq());
            r.adsb->start();
        }
        if (feedIqRecorder)
            app.iqRecorder.configurePrebuffer(r.src->sampleRate(), app.iqBufferSec);

        if (app.saveDecoders)
            for (auto& sd : r.savedDecoders)
                r.decoders.addDecoder(sd.first * 1e6, sd.second);

        // Pre-populate the ACARS receiver with the most common channels that
        // fit inside the SDR bandwidth (only when the user hasn't added any).
        if (r.role == RxRole::Acars && r.decoders.decoderCount() == 0)
        {
            double fs = r.src->sampleRate();
            double ctr = r.src->centerFreq();
            double half = fs * 0.5 - 150.0e3;
            for (int i = 0; i < kNumAcarsCommon; ++i)
            {
                double hz = kAcarsCommonFreqsMHz[i] * 1e6;
                if (std::fabs(hz - ctr) <= half)
                    r.decoders.addDecoder(hz, kAcarsBaud);
            }
        }

        r.lastFeedCount = r.decoders.log().count();
    }
    return ok;
}

void startAll(App& app)
{
    std::string lastErr;
    bool anyOk = false;
    for (auto& rp : app.rx)
    {
        if (!rp)
            continue;
        std::string err;
        bool feedIq = (rp == app.rx.front());
        bool ok = startReceiver(app, *rp, feedIq, err);
        rp->status = ok ? "Running" : ("Error: " + err);
        if (ok) anyOk = true;
        else lastErr = err;
    }
    app.status = anyOk ? (lastErr.empty() ? "Running" : "Running (some sources failed)")
                       : ("Error: " + lastErr);
}

void stopAll(App& app)
{
    for (auto& rp : app.rx)
    {
        if (!rp)
            continue;
        if (rp->src)
            rp->src->stop();
        if (rp->adsb)
            rp->adsb->stop();
        rp->decoders.stop();
        rp->decoders.removeAll();
        rp->status = "Idle";
    }
    app.iqRecorder.stop();
    app.status = "Idle";
}
