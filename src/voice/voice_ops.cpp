#include "core/app.h"
#include "core/main_funcs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

static double nowEpochSec()
{
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
}

// Retune a receiver to a new center. When `preserving` is true the current
// decoders are re-added at their same absolute frequencies (band browsing);
// otherwise they are wiped.
void retuneReceiver(Receiver& r, double centerMHz, bool preserving)
{
    std::vector<std::pair<double, int>> keep;
    double monFreq = -1.0; // frequency of the decoder that was being monitored
    if (preserving)
        for (auto& s : r.decoders.status())
        {
            keep.push_back({s.freqMHz, s.baud});
            if (s.monitored) monFreq = s.freqMHz;
        }

    r.centerMHz = centerMHz;
    r.view.resetView = true;
    if (r.src && r.src->running())
        r.src->setCenterFreq(centerMHz * 1e6);

    double fs = r.src ? r.src->sampleRate() : 0.0;
    r.decoders.removeAll();
    r.decoders.configure(fs, centerMHz * 1e6);
    if (r.view.curN > 0 && fs > 0.0)
        updateFreqAxis(r.view, centerMHz * 1e6, fs, r.view.curN);

    for (auto& k : keep)
        r.decoders.addDecoder(k.first * 1e6, k.second);

    // Re-attach the monitor to the same channel after the rebuild.
    if (monFreq >= 0.0)
        for (auto& s : r.decoders.status())
            if (s.baud == kVoiceBaud && std::fabs(s.freqMHz - monFreq) < 1e-3)
            {
                r.decoders.setVoiceMonitor(s.channelId);
                break;
            }
}

// ---------------------------------------------------------------------------
// Voice scanner: auto-scan the Voice receiver for voice calls.
//
// Watches the averaged spectrum against a slowly-adapting noise/signal
// baseline; a transient peak above the baseline is a new voice carrier. Once
// it has confirmed for `callHunterConfirm` frames a voice decoder is spawned
// on that frequency, and the audio monitor follows the strongest active call.
// Calls that disappear have their decoders removed after `callHunterLost`.
// ---------------------------------------------------------------------------
void updateCallHunter(App& app)
{
    if (!app.callHunterMode)
    {
        app.callHunterWarmup = 0;
        app.callHunterBaseline.clear();
        return;
    }

    Receiver* vr = nullptr;
    for (auto& rp : app.rx)
        if (rp->role == RxRole::Voice) { vr = rp.get(); break; }
    if (!vr || !vr->running())
        return;

    SpectrumView& v = vr->view;
    if (v.curN <= 0 || (int)v.avg.size() < v.curN || (int)v.freqMHz.size() < v.curN)
        return;

    // Reset the baseline when the FFT size or SDR centre changes.
    double curCenter = vr->centerFreq();
    if (app.callHunterBaseline.empty() ||
        (int)app.callHunterBaseline.size() != v.curN ||
        std::abs(curCenter - app.callHunterLastCenter) > 1e3)
    {
        app.callHunterBaseline.assign(v.avg.begin(), v.avg.begin() + v.curN);
        app.callHunterWarmup = 60; // ~3 s at 20 FPS — let the baseline settle
        app.callHunterLastCenter = curCenter;
    }

    // Slow EMA of the baseline. Bins under an already-spawned decoder are
    // frozen, otherwise the baseline would absorb the call and the decoder
    // would be removed prematurely.
    std::vector<uint8_t> freeze(v.curN, 0);
    for (auto& c : app.callHunterCands)
        if (c.channelId >= 0)
            for (int i = 0; i < v.curN; ++i)
                if (std::abs(v.freqMHz[i] - c.freqMHz) < 0.003)
                    freeze[i] = 1;
    for (int i = 0; i < v.curN; ++i)
        if (!freeze[i])
            app.callHunterBaseline[i] = 0.999f * app.callHunterBaseline[i] + 0.001f * v.avg[i];

    if (app.callHunterWarmup > 0)
    {
        --app.callHunterWarmup;
        return;
    }

    double bandMin = v.freqMHz.front();
    double bandMax = v.freqMHz.back();
    if (bandMax <= bandMin)
        return;
    double visMin = std::max(bandMin, v.viewXminMHz);
    double visMax = std::min(bandMax, v.viewXmaxMHz);
    if (visMax <= visMin)
        return;
    int iLo = (int)((visMin - bandMin) / (bandMax - bandMin) * v.curN);
    int iHi = (int)((visMax - bandMin) / (bandMax - bandMin) * v.curN);
    iLo = std::clamp(iLo, 0, v.curN);
    iHi = std::clamp(iHi, 0, v.curN);
    if (iHi - iLo < 4)
        return;

    float thresh = app.callHunterThreshDB;
    float valleyThresh = thresh;
    double binResHz = (bandMax - bandMin) * 1e6 / v.curN;
    int minWidthBins = std::max(1, (int)(3000.0 / binResHz));

    struct Peak { double f; float vv; };
    std::vector<Peak> peaks;
    {
        int start = -1;
        for (int i = iLo; i < iHi; ++i)
        {
            float diff = v.avg[i] - app.callHunterBaseline[i];
            bool above = (diff >= thresh);
            if (above && start < 0) start = i;
            if ((!above || i == iHi - 1) && start >= 0)
            {
                int end = (!above) ? i : iHi;
                int wBins = end - start;
                if (wBins < minWidthBins)
                {
                    start = -1; // too narrow — noise spike
                }
                else if (wBins > minWidthBins * 3)
                {
                    // Wide block (>~9 kHz): split at the deepest valley.
                    int bestSplit = -1;
                    float bestValley = 1e9f;
                    int margin = minWidthBins / 2;
                    for (int k = start + margin; k < end - margin; ++k)
                    {
                        float dk = v.avg[k] - app.callHunterBaseline[k];
                        if (dk < bestValley) { bestValley = dk; bestSplit = k; }
                    }
                    if (bestSplit >= 0 && bestValley < valleyThresh &&
                        (bestSplit - start) >= minWidthBins && (end - bestSplit) >= minWidthBins)
                    {
                        double sumF1 = 0, sumW1 = 0; float bestV1 = -999;
                        for (int k = start; k < bestSplit; ++k)
                        {
                            float dk = std::max(0.0f, v.avg[k] - app.callHunterBaseline[k]);
                            double w = std::pow(10.0, dk / 10.0);
                            sumF1 += v.freqMHz[k] * w; sumW1 += w;
                            bestV1 = std::max(bestV1, dk);
                        }
                        if (sumW1 > 0.0) peaks.push_back({sumF1 / sumW1, bestV1});
                        double sumF2 = 0, sumW2 = 0; float bestV2 = -999;
                        for (int k = bestSplit; k < end; ++k)
                        {
                            float dk = std::max(0.0f, v.avg[k] - app.callHunterBaseline[k]);
                            double w = std::pow(10.0, dk / 10.0);
                            sumF2 += v.freqMHz[k] * w; sumW2 += w;
                            bestV2 = std::max(bestV2, dk);
                        }
                        if (sumW2 > 0.0) peaks.push_back({sumF2 / sumW2, bestV2});
                    }
                    else
                    {
                        double sumF = 0, sumW = 0; float bestV = -999;
                        for (int k = start; k < end; ++k)
                        {
                            float dk = std::max(0.0f, v.avg[k] - app.callHunterBaseline[k]);
                            double w = std::pow(10.0, dk / 10.0);
                            sumF += v.freqMHz[k] * w; sumW += w;
                            bestV = std::max(bestV, dk);
                        }
                        if (sumW > 0.0) peaks.push_back({sumF / sumW, bestV});
                    }
                    start = -1;
                }
                else
                {
                    double sumF = 0, sumW = 0; float bestV = -999;
                    for (int k = start; k < end; ++k)
                    {
                        float dk = std::max(0.0f, v.avg[k] - app.callHunterBaseline[k]);
                        double w = std::pow(10.0, dk / 10.0);
                        sumF += v.freqMHz[k] * w; sumW += w;
                        bestV = std::max(bestV, dk);
                    }
                    if (sumW > 0.0) peaks.push_back({sumF / sumW, bestV});
                    start = -1;
                }
            }
        }
    }

    // Match peaks to existing candidates (±3 kHz), else create new ones.
    const double kSearchMHz = 0.003;
    for (auto& c : app.callHunterCands)
        c.matched = false;
    for (auto& p : peaks)
    {
        bool found = false;
        for (auto& c : app.callHunterCands)
        {
            if (std::abs(p.f - c.freqMHz) < kSearchMHz)
            {
                c.freqMHz = 0.7 * c.freqMHz + 0.3 * p.f;
                c.confirmCount++;
                c.lostCount = 0;
                c.matched = true;
                c.peakDB = p.vv;
                found = true;
                break;
            }
        }
        if (!found)
        {
            CallHunterCand c;
            c.freqMHz = p.f;
            c.peakDB = p.vv;
            c.confirmCount = 1;
            c.matched = true;
            app.callHunterCands.push_back(c);
        }
    }

    // Presence is decided purely from the spectrum peak (the spike in the
    // FFT): once the spike is gone the candidate is lost and its decoder is
    // removed after `callHunterLost` frames. The detector's squelch can't be
    // used here (VHF AM squelch defaults to -120 dB = always open).
    for (auto& c : app.callHunterCands)
    {
        if (!c.matched)
        {
            c.confirmCount = 0;
            c.lostCount++;
        }
    }

    const double kDecoderCover = 0.0025;
    auto hasExistingDecoder = [&](double f) {
        for (auto& s : vr->decoders.status())
            if (std::abs(s.freqMHz - f) < kDecoderCover) return true;
        return false;
    };

    for (auto& c : app.callHunterCands)
    {
        if (c.channelId < 0 && c.confirmCount >= app.callHunterConfirm)
        {
            if (hasExistingDecoder(c.freqMHz))
            {
                c.channelId = -2;
                continue;
            }
            int id = vr->decoders.addDecoder(c.freqMHz * 1e6, kVoiceBaud);
            if (id >= 0)
            {
                c.channelId = id;
                // Populate the Voice Calls tab as calls are found.
                vr->decoders.voiceCallLog().beginLive(c.freqMHz, id, nowEpochSec());
            }
        }
    }

    for (size_t j = 0; j < app.callHunterCands.size();)
    {
        auto& c = app.callHunterCands[j];
        if (c.channelId >= 0 && c.lostCount >= app.callHunterLost)
        {
            vr->decoders.voiceCallLog().closeCall(c.channelId, nowEpochSec());
            vr->decoders.removeDecoder(c.channelId);
            app.callHunterCands.erase(app.callHunterCands.begin() + j);
            continue;
        }
        if (c.channelId == -2 || (c.channelId < 0 && c.lostCount > app.callHunterLost * 3))
        {
            app.callHunterCands.erase(app.callHunterCands.begin() + j);
            continue;
        }
        ++j;
    }

    // Scanner behaviour: follow the strongest call that is currently active
    // (peak present this frame). Switch only when the monitored one isn't.
    int activeId = -1;
    double bestDb = -1e9;
    int curMon = vr->decoders.voiceMonitor();
    bool curActive = false;
    for (auto& c : app.callHunterCands)
    {
        if (c.channelId < 0 || !c.matched) continue;
        if (c.channelId == curMon) curActive = true;
        if (c.peakDB > bestDb) { bestDb = c.peakDB; activeId = c.channelId; }
    }
    if (!curActive && activeId >= 0)
        vr->decoders.setVoiceMonitor(activeId);
}
