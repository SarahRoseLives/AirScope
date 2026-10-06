#include "core/app.h"
#include "core/main_funcs.h"

#include <chrono>
#include <string>
#include <thread>

void updateFeed(App& app)
{
    app.feed.setFormat(app.outFormat);
    app.feed.setStationId(app.outStation);
    app.feed.setFileEnabled(app.outFile, app.outFilePath);
    app.feed.setUdpEnabled(app.outUdp, app.outUdpHost, app.outUdpPort);
    app.feed.setSbsEnabled(app.outSbs, app.outSbsPort);
    app.feed.pollSbs();

    auto& alog = app.decoders.log();
    uint64_t at = alog.count();
    if (at > app.lastAcarsFed)
    {
        auto snap = alog.snapshot();
        uint64_t newN = at - app.lastAcarsFed;
        if (newN > snap.size()) newN = snap.size();
        for (size_t i = snap.size() - (size_t)newN; i < snap.size(); ++i)
            app.feed.feedAcars(snap[i]);
        app.lastAcarsFed = at;
    }
}

void startActive(App& app)
{
    app.viewA.ring.clear();
    app.viewA.waterfall.clear();
    app.viewA.resetView = true;

    IqRing* ring = &app.viewA.ring;
    DecoderManager* mgr = &app.decoders;
    IqRecorder* iqr = &app.iqRecorder;
    auto cb = [ring, mgr, iqr](const float* iq, int n) {
        ring->push(iq, (size_t)n);
        mgr->feed(iq, n);
        iqr->prebuffer(iq, n);
        if (iqr->isRecording())
            iqr->write(iq, n);
    };
    std::string err;
    bool ok = false;

    if (app.sourceMode == 0)
    {
        app.active = &app.sdr;
        app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        app.sdr.setBiasTee(app.biasTee);
        app.sdr.setPpm((double)app.ppm);
        app.sdr.setDcBlock(app.dcBlock);
        ok = app.sdr.start(app.deviceIndex, cb, err);
    }
    else if (app.sourceMode == 4)
    {
        // Dual RTL: RTL A uses same config as mode 0
        app.active = &app.sdr;
        app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        app.sdr.setBiasTee(app.biasTee);
        app.sdr.setPpm((double)app.ppm);
        app.sdr.setDcBlock(app.dcBlock);
        ok = app.sdr.start(app.deviceIndex, cb, err);
    }
    else if (app.sourceMode == 1)
    {
        app.active = &app.wav;
        app.wav.setPath(app.wavPath);
        app.wav.setLoop(app.wavLoop);
        app.wav.setCenterFreq(app.centerFreqMHz * 1e6);
        ok = app.wav.start(0, cb, err);
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        app.active = &app.airspy;
        app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        app.airspy.setGainMode(app.airspyGainMode);
        app.airspy.setSenseGain(app.airspySenseGain);
        app.airspy.setLinearGain(app.airspyLinearGain);
        app.airspy.setLnaGain(app.airspyLnaGain);
        app.airspy.setMixerGain(app.airspyMixerGain);
        app.airspy.setVgaGain(app.airspyVgaGain);
        app.airspy.setLnaAgc(app.airspyLnaAgc);
        app.airspy.setMixerAgc(app.airspyMixerAgc);
        app.airspy.setBiasTee(app.airspyBias);
        app.airspy.setPpm((double)app.ppm);
        app.airspy.setDcBlock(app.dcBlock);
        ok = app.airspy.start(app.deviceIndex, cb, err);
    }
#endif

    if (ok)
    {
        // Dual RTL mode: start second RTL with independent tuning
        bool startedB = false;
        if (app.sourceMode == 4)
        {
            app.viewB.ring.clear();
            app.viewB.waterfall.clear();
            app.viewB.resetView = true;
            IqRing* ringB = &app.viewB.ring;
            DecoderManager* mgrB = &app.decodersB;
            IqRecorder* iqr = &app.iqRecorder;
            auto cbB = [ringB, mgrB, iqr](const float* iq, int n) {
                ringB->push(iq, (size_t)n);
                mgrB->feed(iq, n);
                iqr->prebuffer(iq, n);
                if (iqr->isRecording())
                    iqr->write(iq, n);
            };
            app.sdrB.setSampleRate(kRates[app.sampleRateIdxB]);
            app.sdrB.setCenterFreq(app.centerFreqMHzB * 1e6);
            app.sdrB.setGain(app.autoGainB ? -1.0 : (double)app.gainDbB);
            app.sdrB.setBiasTee(app.biasTeeB);
            app.sdrB.setPpm((double)app.ppmB);
            app.sdrB.setDcBlock(app.dcBlock);
            std::string errB;
            startedB = app.sdrB.start(app.deviceIndexB, cbB, errB);
            if (startedB)
            {
                app.decodersB.removeAll();
                app.decodersB.configure(app.sdrB.sampleRate(), app.sdrB.centerFreq());
                app.decodersB.setMaxWorkers(2);
                app.decodersB.start();
            }
            else
                app.status = "Dual RTL B error: " + errB;
        }
        app.dualMode = startedB;

        app.decoders.removeAll();
        app.decoders.configure(app.active->sampleRate(), app.active->centerFreq());
        app.decoders.setAudioEnabled(true);
        if (app.dualMode)
            app.decoders.setMaxWorkers(4); // cap primary workers in dual mode (B gets 2)
        app.decoders.start();
        app.lastConfiguredFs = app.active->sampleRate();
        app.iqRecorder.configurePrebuffer(app.active->sampleRate(), app.iqBufferSec);

        // Restore saved decoders (from airscope.ini)
        if (app.saveDecoders && !app.savedDecoders.empty())
        {
            for (auto& sd : app.savedDecoders)
                app.decoders.addDecoder(sd.first * 1e6, sd.second);
        }
        if (app.dualMode && app.saveDecoders && !app.savedDecodersB.empty())
        {
            for (auto& sd : app.savedDecodersB)
                app.decodersB.addDecoder(sd.first * 1e6, sd.second);
        }
        if (!app.saveDecoders)
        {
            app.savedDecoders.clear();
            app.savedDecodersB.clear();
        }
    }

    // If the IQ recorder was active, restart it with the new sample rate
    // so the WAV header matches the actual capture rate.
    bool wasIqRec = app.iqRecorder.isRecording();
    if (wasIqRec)
    {
        app.iqRecorder.stop();
        if (ok)
            app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
    }

    if (ok)
        app.status = app.dualMode ? "Running (dual SDR)" : "Running";
    else
        app.status = "Error: " + err;
}
