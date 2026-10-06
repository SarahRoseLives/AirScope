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
    for (size_t i = 0; i < app.rx.size(); ++i)
    {
        Receiver& r = *app.rx[i];
        buf->appendf("rx%d.mode=%d\n", (int)i, r.mode);
        buf->appendf("rx%d.device=%d\n", (int)i, r.deviceIndex);
        buf->appendf("rx%d.center=%.10g\n", (int)i, r.centerMHz);
        buf->appendf("rx%d.rate=%d\n", (int)i, r.rateIdx);
        buf->appendf("rx%d.autogain=%d\n", (int)i, r.autoGain ? 1 : 0);
        buf->appendf("rx%d.gain=%g\n", (int)i, (double)r.gainDb);
        buf->appendf("rx%d.bias=%d\n", (int)i, r.biasTee ? 1 : 0);
        buf->appendf("rx%d.ppm=%g\n", (int)i, (double)r.ppm);
        buf->appendf("rx%d.aprate=%d\n", (int)i, r.apRateIdx);
        buf->appendf("rx%d.aprate=%d\n", (int)i, r.apRateIdx);
        buf->appendf("rx%d.apgainmode=%d\n", (int)i, r.apGainMode);
        buf->appendf("rx%d.apsense=%d\n", (int)i, r.apSense);
        buf->appendf("rx%d.aplinear=%d\n", (int)i, r.apLinear);
        buf->appendf("rx%d.aplna=%d\n", (int)i, r.apLna);
        buf->appendf("rx%d.apmixer=%d\n", (int)i, r.apMixer);
        buf->appendf("rx%d.apvga=%d\n", (int)i, r.apVga);
        buf->appendf("rx%d.aplnaagc=%d\n", (int)i, r.apLnaAgc ? 1 : 0);
        buf->appendf("rx%d.apmixeragc=%d\n", (int)i, r.apMixerAgc ? 1 : 0);
        buf->appendf("rx%d.apbias=%d\n", (int)i, r.apBias ? 1 : 0);
        buf->appendf("rx%d.sprate=%d\n", (int)i, r.spRateIdx);
        buf->appendf("rx%d.spagc=%d\n", (int)i, r.spAgc ? 1 : 0);
        buf->appendf("rx%d.spgr=%d\n", (int)i, r.spGRdB);
        buf->appendf("rx%d.spbias=%d\n", (int)i, r.spBias ? 1 : 0);
        buf->appendf("rx%d.wavpath=%s\n", (int)i, r.wavPath);
        buf->appendf("rx%d.wavloop=%d\n", (int)i, r.wavLoop ? 1 : 0);
        buf->appendf("rx%d.showbp=%d\n", (int)i, r.showBandPlan ? 1 : 0);
        buf->appendf("rx%d.bpidx=%d\n", (int)i, r.bandPlanIdx);
        if (app.saveDecoders)
            for (auto& sd : r.savedDecoders)
                buf->appendf("rx%d.saved=%.3f,%d\n", (int)i, sd.first, sd.second);
    }

    WI(newBaud); WI(fftSizeIdx);
    WI(audioDevice); WI(voiceMuted); WI(cpuReduce);
    WI(logToDb); WI(maxDbAgeDays);
    WI(webServerEnabled); WI(webServerPort);
    WF(iqBufferSec);
    WI(autoScale); WI(bandBrowse); WF(avgAlpha); WF(dbMin); WF(dbMax);
    WF(browseEdgePct); WF(browseThrottleMs); WF(browseMinMovePct);
    WS(recordDir);
    WI(recordFormat);
    WI(saveDecoders);
    WI(acPosOnly);
    WI(showEmptyMsgs);
    WS(bandPlanDir);
    WI(outFile); WS(outFilePath); WI(outUdp); WS(outUdpHost); WI(outUdpPort);
    WI(outFormat); WS(outStation); WI(outSbs); WI(outSbsPort);
    WI(layoutVersion);
    WI(fontSize);
    WI(languageIdx);
    WI(lightMode);
    for (auto& cc : app.blacklistCountries)
        buf->appendf("blacklistCC=%s\n", cc.c_str());
    buf->append("\n");
#undef WI
#undef WF
#undef WD
#undef WS
}

static bool parseRxKey(const char* key, int& idx, const char*& field)
{
    if (key[0] != 'r' || key[1] != 'x')
        return false;
    char* end = nullptr;
    long i = std::strtol(key + 2, &end, 10);
    if (!end || *end != '.')
        return false;
    idx = (int)i;
    field = end + 1;
    return true;
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

    int ri;
    const char* field;
    if (parseRxKey(key, ri, field) && ri >= 0 && ri < (int)app.rx.size())
    {
        Receiver& r = *app.rx[ri];
        if (!std::strcmp(field, "mode")) r.mode = std::atoi(val);
        else if (!std::strcmp(field, "device")) r.deviceIndex = std::atoi(val);
        else if (!std::strcmp(field, "center")) r.centerMHz = std::atof(val);
        else if (!std::strcmp(field, "rate")) r.rateIdx = std::atoi(val);
        else if (!std::strcmp(field, "autogain")) r.autoGain = std::atoi(val) != 0;
        else if (!std::strcmp(field, "gain")) r.gainDb = (float)std::atof(val);
        else if (!std::strcmp(field, "bias")) r.biasTee = std::atoi(val) != 0;
        else if (!std::strcmp(field, "ppm")) r.ppm = (float)std::atof(val);
        else if (!std::strcmp(field, "aprate")) r.apRateIdx = std::atoi(val);
        else if (!std::strcmp(field, "apgainmode")) r.apGainMode = std::atoi(val);
        else if (!std::strcmp(field, "apsense")) r.apSense = std::atoi(val);
        else if (!std::strcmp(field, "aplinear")) r.apLinear = std::atoi(val);
        else if (!std::strcmp(field, "aplna")) r.apLna = std::atoi(val);
        else if (!std::strcmp(field, "apmixer")) r.apMixer = std::atoi(val);
        else if (!std::strcmp(field, "apvga")) r.apVga = std::atoi(val);
        else if (!std::strcmp(field, "aplnaagc")) r.apLnaAgc = std::atoi(val) != 0;
        else if (!std::strcmp(field, "apmixeragc")) r.apMixerAgc = std::atoi(val) != 0;
        else if (!std::strcmp(field, "apbias")) r.apBias = std::atoi(val) != 0;
        else if (!std::strcmp(field, "sprate")) r.spRateIdx = std::atoi(val);
        else if (!std::strcmp(field, "spagc")) r.spAgc = std::atoi(val) != 0;
        else if (!std::strcmp(field, "spgr")) r.spGRdB = std::atoi(val);
        else if (!std::strcmp(field, "spbias")) r.spBias = std::atoi(val) != 0;
        else if (!std::strcmp(field, "wavpath"))
        {
            std::strncpy(r.wavPath, val, sizeof(r.wavPath) - 1);
            r.wavPath[sizeof(r.wavPath) - 1] = 0;
        }
        else if (!std::strcmp(field, "wavloop")) r.wavLoop = std::atoi(val) != 0;
        else if (!std::strcmp(field, "showbp")) r.showBandPlan = std::atoi(val) != 0;
        else if (!std::strcmp(field, "bpidx")) r.bandPlanIdx = std::atoi(val);
        else if (!std::strcmp(field, "saved"))
        {
            double f = 0.0; int b = 0;
            if (std::sscanf(val, "%lf,%d", &f, &b) == 2 && f > 0.0 && b > 0)
                r.savedDecoders.push_back({f, b});
        }
        return;
    }

#define RI(f) if (!std::strcmp(key, #f)) { app.f = std::atoi(val); return; }
#define RB(f) if (!std::strcmp(key, #f)) { app.f = (std::atoi(val) != 0); return; }
#define RF(f) if (!std::strcmp(key, #f)) { app.f = (float)std::atof(val); return; }
#define RD(f) if (!std::strcmp(key, #f)) { app.f = std::atof(val); return; }
#define RS(f) if (!std::strcmp(key, #f)) { std::strncpy(app.f, val, sizeof(app.f) - 1); app.f[sizeof(app.f) - 1] = 0; return; }
    RI(newBaud); RI(fftSizeIdx);
    RI(audioDevice); RB(voiceMuted); RB(cpuReduce);
    RB(logToDb); RI(maxDbAgeDays);
    RB(webServerEnabled); RI(webServerPort);
    RF(iqBufferSec);
    RB(autoScale); RB(bandBrowse); RF(avgAlpha); RF(dbMin); RF(dbMax);
    RF(browseEdgePct); RF(browseThrottleMs); RF(browseMinMovePct);
    RS(recordDir);
    RI(recordFormat); RB(saveDecoders);
    RB(acPosOnly);
    RB(showEmptyMsgs);
    RS(bandPlanDir);
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
