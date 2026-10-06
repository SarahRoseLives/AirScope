#include "core/app.h"
#include "core/main_funcs.h"

// Retune a receiver to a new center. When `preserving` is true the current
// decoders are re-added at their same absolute frequencies (band browsing);
// otherwise they are wiped.
void retuneReceiver(Receiver& r, double centerMHz, bool preserving)
{
    std::vector<std::pair<double, int>> keep;
    if (preserving)
        for (auto& s : r.decoders.status())
            keep.push_back({s.freqMHz, s.baud});

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
}
