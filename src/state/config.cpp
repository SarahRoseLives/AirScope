#include "imgui.h"
#include "imgui_internal.h"
#include "core/app.h"
#include "core/main_funcs.h"
#include "version.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void cfgWriteAll(App& app, ImGuiTextBuffer* buf)
{
    buf->append("[AirScope][State]\n");
#define WI(f) buf->appendf(#f "=%d\n", (int)app.f)
#define WF(f) buf->appendf(#f "=%g\n", (double)app.f)
#define WD(f) buf->appendf(#f "=%.10g\n", (double)app.f)
#define WS(f) buf->appendf(#f "=%s\n", app.f)
    WI(sourceMode); WI(deviceIndex); WI(sampleRateIdx); WI(newBaud); WI(fftSizeIdx);
    WI(audioDevice); WI(voiceMuted); WI(cpuReduce);
    WI(logToDb); WI(maxDbAgeDays);
    WI(webServerEnabled); WI(webServerPort);
    WD(centerFreqMHz);
    WF(iqBufferSec);
    WI(autoGain); WF(gainDb); WI(biasTee); WF(ppm); WI(dcBlock);
    WI(autoScale); WI(bandBrowse); WF(avgAlpha); WF(dbMin); WF(dbMax);
    WF(browseEdgePct); WF(browseThrottleMs); WF(browseMinMovePct);
    WS(wavPath); WI(wavLoop);
#ifdef HAS_AIRSPY
    WI(airspySampleRateIdx); WI(airspyGainMode); WI(airspySenseGain); WI(airspyLinearGain);
    WI(airspyLnaGain); WI(airspyMixerGain); WI(airspyVgaGain);
    WI(airspyLnaAgc); WI(airspyMixerAgc); WI(airspyBias);
#endif
    WI(deviceIndexB); WD(centerFreqMHzB); WI(sampleRateIdxB);
    WI(autoGainB); WF(gainDbB); WI(biasTeeB); WF(ppmB);
    WS(recordDir);
    WI(recordFormat);
    WI(saveDecoders);
    WI(acPosOnly);
    WI(showEmptyMsgs);
    WI(showBandPlan); WI(bandPlanIdx); WI(showBandPlanB); WI(bandPlanIdxB); WS(bandPlanDir);
    WI(outFile); WS(outFilePath); WI(outUdp); WS(outUdpHost); WI(outUdpPort);
    WI(outFormat); WS(outStation); WI(outSbs); WI(outSbsPort);
    WI(layoutVersion);
    WI(fontSize);
    WI(languageIdx);
    WI(lightMode);
    for (auto& cc : app.blacklistCountries)
        buf->appendf("blacklistCC=%s\n", cc.c_str());
    if (app.saveDecoders)
    {
        for (auto& sd : app.savedDecoders)
            buf->appendf("savedDecoder=%.3f,%d\n", sd.first, sd.second);
        for (auto& sd : app.savedDecodersB)
            buf->appendf("savedDecoderB=%.3f,%d\n", sd.first, sd.second);
    }
    buf->append("\n");
#undef WI
#undef WF
#undef WD
#undef WS
}

void cfgReadLine(App& app, const char* line)
{
    const char* eq = std::strchr(line, '=');
    if (!eq)
        return;
    char key[48];
    int klen = (int)(eq - line);
    if (klen <= 0 || klen >= (int)sizeof(key))
        return;
    std::memcpy(key, line, klen);
    key[klen] = 0;
    const char* val = eq + 1;
#define RI(f) if (!std::strcmp(key, #f)) { app.f = std::atoi(val); return; }
#define RB(f) if (!std::strcmp(key, #f)) { app.f = (std::atoi(val) != 0); return; }
#define RF(f) if (!std::strcmp(key, #f)) { app.f = (float)std::atof(val); return; }
#define RD(f) if (!std::strcmp(key, #f)) { app.f = std::atof(val); return; }
#define RS(f) if (!std::strcmp(key, #f)) { std::strncpy(app.f, val, sizeof(app.f) - 1); app.f[sizeof(app.f) - 1] = 0; return; }
    RI(sourceMode); RI(deviceIndex); RI(sampleRateIdx); RI(newBaud); RI(fftSizeIdx);
    RI(audioDevice); RB(voiceMuted); RB(cpuReduce);
    RB(logToDb); RI(maxDbAgeDays);
    RB(webServerEnabled); RI(webServerPort);
    RD(centerFreqMHz);
    RF(iqBufferSec);
    RB(autoGain); RF(gainDb); RB(biasTee); RF(ppm); RB(dcBlock);
    RB(autoScale); RB(bandBrowse); RF(avgAlpha); RF(dbMin); RF(dbMax);
    RF(browseEdgePct); RF(browseThrottleMs); RF(browseMinMovePct);
    RS(wavPath); RB(wavLoop);
#ifdef HAS_AIRSPY
    RI(airspySampleRateIdx); RI(airspyGainMode); RI(airspySenseGain); RI(airspyLinearGain);
    RI(airspyLnaGain); RI(airspyMixerGain); RI(airspyVgaGain);
    RB(airspyLnaAgc); RB(airspyMixerAgc); RB(airspyBias);
#endif
    RI(deviceIndexB); RD(centerFreqMHzB); RI(sampleRateIdxB);
    RB(autoGainB); RF(gainDbB); RB(biasTeeB); RF(ppmB);
    RS(recordDir);
    RI(recordFormat); RB(saveDecoders);
    // savedDecoder lines: freqMHz,baud (e.g. "131.550,1200")
    if (!std::strcmp(key, "savedDecoder"))
    {
        double f = 0.0; int b = 0;
        if (std::sscanf(val, "%lf,%d", &f, &b) == 2 && f > 0.0 && b > 0)
            app.savedDecoders.push_back({f, b});
        return;
    }
    if (!std::strcmp(key, "savedDecoderB"))
    {
        double f = 0.0; int b = 0;
        if (std::sscanf(val, "%lf,%d", &f, &b) == 2 && f > 0.0 && b > 0)
            app.savedDecodersB.push_back({f, b});
        return;
    }
    RB(acPosOnly);
    RB(showEmptyMsgs);
    RB(showBandPlan); RI(bandPlanIdx); RB(showBandPlanB); RI(bandPlanIdxB); RS(bandPlanDir);
    RB(outFile); RS(outFilePath); RB(outUdp); RS(outUdpHost); RI(outUdpPort);
    RI(outFormat); RS(outStation); RB(outSbs); RI(outSbsPort);
    RI(layoutVersion);
    RI(fontSize);
    RI(languageIdx);
    RB(lightMode);
    if (!std::strcmp(key, "blacklistCC") && val[0] && val[1] && !val[2])
    {
        std::string cc(val, 2);
        auto& bl = app.blacklistCountries;
        if (std::find(bl.begin(), bl.end(), cc) == bl.end())
            bl.push_back(cc);
        return;
    }
#undef RI
#undef RB
#undef RF
#undef RD
#undef RS
}

void cfgRegisterHandler(App& app)
{
    ImGuiSettingsHandler h;
    h.TypeName = "AirScope";
    h.TypeHash = ImHashStr("AirScope");
    h.UserData = &app;
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
        return std::strcmp(name, "State") == 0 ? (void*)1 : nullptr;
    };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void* entry,
                      const char* line) {
        if (!entry)
            return;
        cfgReadLine(*static_cast<App*>(handler->UserData), line);
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        cfgWriteAll(*static_cast<App*>(handler->UserData), buf);
    };
    ImGui::AddSettingsHandler(&h);
}
