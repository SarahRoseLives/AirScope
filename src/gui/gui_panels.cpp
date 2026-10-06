#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include "core/app.h"
#include "core/main_funcs.h"
#include "decode/icao_country.h"
#include "decode/band_plan.h"
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"
#include "gui/copyable.h"
#ifdef HAS_AIRSPY
#include "sdr/airspy_source.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Light-mode colour dimmer.
inline ImVec4 Lc(const App& app, const ImVec4& c) {
    if (!app.lightMode) return c;
    return ImVec4(c.x * 0.44f, c.y * 0.56f, c.z * 0.44f, c.w);
}

static bool drawPpmAdjust(const char* label, float* ppm)
{
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    bool changed = ImGui::InputFloat("##ppm", ppm, 0.1f, 1.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##ppmsl", ppm, -50.0f, 50.0f, "");
    if (*ppm < -200.0f) *ppm = -200.0f;
    if (*ppm > 200.0f) *ppm = 200.0f;
    ImGui::PopID();
    return changed;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

static void refreshDevices(Receiver& r)
{
    if (!r.src)
        r.src = makeSdrSource(r.mode);
    r.devices = r.src ? r.src->listDevices() : std::vector<SdrDeviceInfo>{};
}

static void drawDeviceCombo(Receiver& r)
{
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh##dev"))
        refreshDevices(r);
    ImGui::SameLine();
    ImGui::Text("(%d)", (int)r.devices.size());
    if (r.devices.empty())
        return;
    if (r.deviceIndex >= (int)r.devices.size())
        r.deviceIndex = 0;
    std::string preview = r.devices[r.deviceIndex].name;
    if (ImGui::BeginCombo("Device", preview.c_str()))
    {
        for (int i = 0; i < (int)r.devices.size(); ++i)
        {
            bool sel = (r.deviceIndex == i);
            std::string label = std::to_string(i) + ": " + r.devices[i].name +
                                " [" + r.devices[i].serial + "]";
            if (ImGui::Selectable(label.c_str(), sel))
                r.deviceIndex = i;
        }
        ImGui::EndCombo();
    }
}

// One receiver's configuration panel.
static void drawReceiverControls(App& app, Receiver& r, int idx)
{
    ImGui::PushID(idx);
    bool running = r.running();

    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "Receiver %d  -  %s", idx + 1, rxModeName(r.mode));
    ImGui::Separator();
    ImGui::TextUnformatted(hdr);

    // Source type.
    int modeSel = r.mode;
    const char* modes[4];
    int nm = 0;
    modes[nm++] = "RTL-SDR";
    modes[nm++] = "WAV file";
#ifdef HAS_AIRSPY
    modes[nm++] = "Airspy";
#endif
#ifdef HAS_SDRPLAY
    modes[nm++] = "SDRplay";
#endif
    ImGui::BeginDisabled(running);
    if (ImGui::Combo("Source", &modeSel, modes, nm))
    {
        // Map combo index back to a mode constant.
        int map[] = {kRxRtl, kRxWav,
#ifdef HAS_AIRSPY
                     kRxAirspy,
#endif
#ifdef HAS_SDRPLAY
                     kRxSdrplay,
#endif
        };
        r.mode = map[std::clamp(modeSel, 0, nm - 1)];
        r.src = makeSdrSource(r.mode);
        r.devices.clear();
        refreshDevices(r);
    }
    ImGui::EndDisabled();

    ImGui::Checkbox("Show spectrum/waterfall", &r.showSpectrum);

    if (r.mode == kRxWav)
    {
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##wavpath", r.wavPath, sizeof(r.wavPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
            openWavDialog(r.wavPath, sizeof(r.wavPath));
        ImGui::Checkbox("Loop", &r.wavLoop);
        ImGui::InputDouble("Center label (MHz)", &r.centerMHz, 0.1, 1.0, "%.4f");
    }
    else
    {
        drawDeviceCombo(r);

        if (ImGui::InputDouble("Center (MHz)", &r.centerMHz, 0.1, 1.0, "%.4f"))
        {
            r.view.resetView = true;
            if (running) r.src->setCenterFreq(r.centerMHz * 1e6);
        }

        if (r.mode == kRxRtl)
        {
            if (ImGui::Combo("Sample rate (MHz)", &r.rateIdx, kRateLabels, kNumRates))
            {
                r.view.resetView = true;
                if (running) r.src->setSampleRate(kRates[r.rateIdx]);
            }
            if (ImGui::Checkbox("Auto gain (AGC)", &r.autoGain))
                if (running) r.src->setGain(r.autoGain ? -1.0 : (double)r.gainDb);
            if (!r.autoGain)
                if (ImGui::SliderFloat("Gain (dB)", &r.gainDb, 0.0f, 50.0f, "%.1f"))
                    if (running) r.src->setGain((double)r.gainDb);
            if (ImGui::Checkbox("Bias-T", &r.biasTee))
                if (running) r.src->setBiasTee(r.biasTee);
        }
        else if (r.mode == kRxAirspy)
        {
#ifdef HAS_AIRSPY
            if (ImGui::Combo("Sample rate (MHz)", &r.apRateIdx, kAirspyRateLabels, kAirspyNumRates))
            {
                r.view.resetView = true;
                if (running) r.src->setSampleRate(kAirspyRates[r.apRateIdx]);
            }
            ImGui::Separator();
            if (ImGui::RadioButton("Sensitive", r.apGainMode == 0)) r.apGainMode = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Linear", r.apGainMode == 1)) r.apGainMode = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("Free", r.apGainMode == 2)) r.apGainMode = 2;

            auto* a = dynamic_cast<AirspySource*>(r.src.get());
            if (r.apGainMode == 0)
            {
                if (ImGui::SliderInt("Sensitivity gain", &r.apSense, 0, 21) && a)
                { a->setGainMode(0); a->setSenseGain(r.apSense); }
            }
            else if (r.apGainMode == 1)
            {
                if (ImGui::SliderInt("Linearity gain", &r.apLinear, 0, 21) && a)
                { a->setGainMode(1); a->setLinearGain(r.apLinear); }
            }
            else
            {
                if (ImGui::Checkbox("LNA AGC", &r.apLnaAgc) && a) a->setLnaAgc(r.apLnaAgc);
                ImGui::BeginDisabled(r.apLnaAgc);
                if (ImGui::SliderInt("LNA gain", &r.apLna, 0, 15) && a) a->setLnaGain(r.apLna);
                ImGui::EndDisabled();
                if (ImGui::Checkbox("Mixer AGC", &r.apMixerAgc) && a) a->setMixerAgc(r.apMixerAgc);
                ImGui::BeginDisabled(r.apMixerAgc);
                if (ImGui::SliderInt("Mixer gain", &r.apMixer, 0, 15) && a) a->setMixerGain(r.apMixer);
                ImGui::EndDisabled();
                if (ImGui::SliderInt("VGA gain", &r.apVga, 0, 15) && a) a->setVgaGain(r.apVga);
            }
            if (ImGui::Checkbox("Bias T", &r.apBias) && a) a->setBiasTee(r.apBias);
#else
            ImGui::TextDisabled("Airspy support not built.");
#endif
        }
        else if (r.mode == kRxSdrplay)
        {
            if (ImGui::Combo("Sample rate (MHz)", &r.spRateIdx, kSdrplayRateLabels, kSdrplayNumRates))
            {
                r.view.resetView = true;
                if (running) r.src->setSampleRate(kSdrplayRates[r.spRateIdx]);
            }
            if (ImGui::Checkbox("AGC", &r.spAgc))
                if (running) r.src->setGain(r.spAgc ? -1.0 : (double)r.spGRdB);
            ImGui::BeginDisabled(r.spAgc);
            if (ImGui::SliderInt("Gain reduction (dB)", &r.spGRdB, 20, 59))
                if (running) r.src->setGain((double)r.spGRdB);
            ImGui::EndDisabled();
            if (ImGui::Checkbox("Bias-T", &r.spBias))
                if (running) r.src->setBiasTee(r.spBias);
        }

        if (drawPpmAdjust("PPM", &r.ppm))
            if (running) r.src->setPpm((double)r.ppm);
    }

    if (!r.status.empty())
        ImGui::TextDisabled("%s", r.status.c_str());
    ImGui::PopID();
}

void drawControls(App& app)
{
    ImGui::Begin((std::string(_L("Control")) + "###Control").c_str());

    bool anyRunning = false;
    for (auto& rp : app.rx)
        if (rp && rp->running()) anyRunning = true;

    if (!anyRunning)
    {
        if (ImGui::Button(_L("Start"), ImVec2(120, 0)))
            startAll(app);
    }
    else
    {
        if (ImGui::Button(_L("Stop"), ImVec2(120, 0)))
            stopAll(app);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.status.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  AirScope v" AIRSCOPE_VERSION);

    {
        VersionCheck::State st = app.verCheck.state();
        if (st == VersionCheck::UpdateAvailable)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Update available: v%s",
                               app.verCheck.latestVersion().c_str());
        }
    }

    ImGui::Separator();

    // Per-receiver configuration.
    for (size_t i = 0; i < app.rx.size(); ++i)
        if (app.rx[i])
            drawReceiverControls(app, *app.rx[i], (int)i);

    ImGui::Separator();
    // ---- global display settings ----
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat(_L("Averaging"), &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox(_L("Auto-scale dB"), &app.autoScale);
    ImGui::SliderFloat(_L("dB min"), &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat(_L("dB max"), &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;
    if (ImGui::Button(_L("Reset view (fit band)")))
        for (auto& rp : app.rx) rp->view.resetView = true;
    ImGui::SameLine();
    ImGui::TextDisabled("drag=pan  scroll=zoom  dbl-click=fit");
    ImGui::Checkbox("Pan/scroll retunes SDR (browse band)", &app.bandBrowse);
    ImGui::Checkbox(_L("DC block"), &app.dcBlock);

    // Band plan selection (shared list; per-receiver toggle below).
    if (ImGui::Checkbox(_L("Band Plan"), &app.rx.front()->showBandPlan)) {}
    if (app.rx.front()->showBandPlan)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##bpr"))
            scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
        if (!app.bandPlanNames.empty())
        {
            if (ImGui::Combo("##bplan-sel", &app.rx.front()->bandPlanIdx,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                auto& r = *app.rx.front();
                if (r.bandPlanIdx >= 0 && r.bandPlanIdx < (int)app.bandPlanPaths.size())
                    r.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[r.bandPlanIdx]);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Ctrl+click the spectrum to add a channel decoder there");

    // ---- Database ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Database (SQLite log)")))
    {
        ImGui::Checkbox(_L("Log messages to database"), &app.logToDb);
        if (app.writeDb.enabled())
            ImGui::TextDisabled("  Session DB is active");
        ImGui::SliderInt(_L("Keep DB (days)"), &app.maxDbAgeDays, 1, 90);
        ImGui::TextDisabled("  Archives in: .\\databases\\messages_*.db");
    }

    // ---- Display ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Display")))
    {
        if (ImGui::Checkbox(_L("Light mode"), &app.lightMode))
        {
            if (app.lightMode) ImGui::StyleColorsLight();
            else               ImGui::StyleColorsDark();
        }
        if (ImGui::SliderInt(_L("Font size"), &app.fontSize, 8, 24, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            if (app.fontSize < 8)  app.fontSize = 8;
            if (app.fontSize > 24) app.fontSize = 24;
        }
        ImGui::TextDisabled("  Restart to apply");
    }

    // ---- Output ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Output (message feed)")))
    {
        const char* fmts[] = {"JSON (JAERO/Acarshub)", "JAERO text", "JSON (AirScope)"};
        ImGui::Combo("Format", &app.outFormat, fmts, 3);
        ImGui::Checkbox("Write to file", &app.outFile);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("File", app.outFilePath, sizeof(app.outFilePath));
        ImGui::Checkbox("Send over UDP", &app.outUdp);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("UDP host", app.outUdpHost, sizeof(app.outUdpHost));
        ImGui::InputInt("UDP port", &app.outUdpPort);
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("Station", app.outStation, sizeof(app.outStation));
        ImGui::Separator();
        ImGui::Checkbox("SBS/BaseStation server (positions)", &app.outSbs);
        ImGui::InputInt("SBS port", &app.outSbsPort);
        if (app.outSbs)
        {
            if (app.feed.sbsListening())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                   "Listening on TCP :%d  -  %d client(s), %llu sent",
                                   app.outSbsPort, app.feed.sbsClients(),
                                   (unsigned long long)app.feed.sbsSent());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                   "Bind failed on :%d (port in use?)", app.outSbsPort);
        }
        ImGui::Text("Sent: %llu", (unsigned long long)app.feed.sent());
        ImGui::TextDisabled("ACARS -> JAERO JSONdump.");
    }

    // ---- IQ Recorder ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
        bool iqRec = app.iqRecorder.isRecording();
        if (iqRec)
        {
            if (ImGui::Button("Stop##iqrec"))
                app.iqRecorder.stop();
        }
        else if (ImGui::Button("Start##iqrec"))
        {
            if (app.rx.front()->running())
                app.iqRecorder.start(app.iqRecPath, app.rx.front()->sampleRate());
        }
        if (app.iqRecorder.isRecording())
        {
            double sec = app.iqRecorder.elapsed();
            int m = (int)(sec / 60), s = (int)(sec) % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC %02d:%02d  —  %s",
                               m, s, app.iqRecorder.path().c_str());
        }
        ImGui::TextDisabled("Records the first receiver's raw IQ.");
    }

    // ---- Web Dashboard ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Web Dashboard"))
    {
        if (ImGui::Checkbox("Enable server", &app.webServerEnabled))
        {
            if (app.webServerEnabled)
            {
                app.webServer.receivers.clear();
                for (auto& rp : app.rx)
                    app.webServer.receivers.push_back(rp.get());
                app.webServer.start(app.webServerPort);
            }
            else
                app.webServer.stop();
        }
        if (!app.webServer.running())
        {
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputInt("Port", &app.webServerPort))
            {
                if (app.webServerPort < 1) app.webServerPort = 1;
                if (app.webServerPort > 65535) app.webServerPort = 65535;
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running on port %d",
                               app.webServerPort);
        }
    }

    ImGui::End();
}

void drawSpectrum(App& app, Receiver& r, int idx, bool voiceView)
{
    (void)voiceView;
    SpectrumView& v = r.view;
    std::string title = std::string(_L("Spectrum")) + "###Spectrum" + std::to_string(idx);
    ImGui::Begin(title.c_str());
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    std::string plotId = std::string("##plot_") + std::to_string(idx);
    if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1, -1), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("MHz", "dB", 0, 0);

        bool bandValid = (v.curN > 0 && v.freqMHz.front() < v.freqMHz.back());
        if (bandValid)
        {
            double bandSpan = v.freqMHz.back() - v.freqMHz.front();
            ImPlot::SetupAxisZoomConstraints(ImAxis_X1, bandSpan * 1e-4, bandSpan);
        }
        if (v.resetView && bandValid)
            ImPlot::SetupAxisLimits(ImAxis_X1, v.freqMHz.front(), v.freqMHz.back(), ImGuiCond_Always);
        if (app.autoScale || v.resetView)
            ImPlot::SetupAxisLimits(ImAxis_Y1, app.dbMin, app.dbMax, ImGuiCond_Always);

        if (v.curN > 0)
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);

        auto decs = r.decoders.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                r.decoders.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;

        // Band-browse retuning.
        if (app.bandBrowse && r.mode != kRxWav && r.src && r.src->running() &&
            !v.resetView)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = r.src->centerFreq() / 1e6;
            double fsMHz = r.src->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - app.lastRetuneCtr) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs = std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                retuneReceiver(r, viewCtr, true);
                app.lastRetune = now;
                app.lastRetuneCtr = viewCtr;
            }
        }

        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        v.specLeftInset = pp.x - origin.x;
        v.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        if (bandValid)
            v.resetView = false;

        // Drag-to-place decoder: Ctrl+mousedown starts placing, release creates it.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl)
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                app.placingDecoder = true;
                app.placingRx = idx;
                app.placingFreqMHz = mp.x;
            }
            if (app.placingDecoder && app.placingRx == idx)
                app.placingFreqMHz = mp.x;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                r.decoders.addDecoder(mp.x * 1e6, app.newBaud > 0 ? app.newBaud : 1200);
            }
        }
        else if (app.placingDecoder && app.placingRx == idx)
        {
            app.placingDecoder = false;
        }

        if (app.placingDecoder && app.placingRx == idx)
        {
            ImPlotRect lim2 = ImPlot::GetPlotLimits();
            float xMin = (float)lim2.X.Min;
            float xMax = (float)lim2.X.Max;
            if (xMax > xMin)
            {
                float frac = ((float)app.placingFreqMHz - xMin) / (xMax - xMin);
                ImVec2 pp2 = ImPlot::GetPlotPos();
                ImVec2 ps2 = ImPlot::GetPlotSize();
                float px = pp2.x + frac * ps2.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(px, pp2.y), ImVec2(px, pp2.y + ps2.y),
                            IM_COL32(255, 40, 40, 200), 1.5f);
            }
        }

        // Band plan bar.
        const BandPlan& bp = r.bandPlanLoaded;
        if (r.showBandPlan && bp.valid && v.curN > 0)
        {
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp2 = ImPlot::GetPlotPos(), ps2 = ImPlot::GetPlotSize();
            float bandTop = pp2.y + ps2.y - kBandH;
            float bandBot = bandTop + kBandH;
            float pxPerMHz = (float)(ps2.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp2.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp2.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
                dl->AddRectFilled(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot), e.color);
                float segW = hiPx - loPx;
                if (segW > 50 && !e.label.empty())
                {
                    float lw = ImGui::CalcTextSize(e.label.c_str()).x;
                    if (lw < segW - 4)
                    {
                        float cx = loPx + (segW - lw) * 0.5f;
                        float cy = bandTop + (kBandH - ImGui::GetTextLineHeight()) * 0.5f;
                        dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 230), e.label.c_str());
                    }
                }
            }
        }

        ImPlot::EndPlot();
        v.fftSkip = false;
    }
    ImGui::End();
}

void drawWaterfall(App& app, Receiver& r, int idx)
{
    SpectrumView& v = r.view;
    std::string title = std::string(_L("Waterfall")) + "###Waterfall" + std::to_string(idx);
    ImGui::Begin(title.c_str());

    float uMin = 0.0f, uMax = 1.0f;
    float xLo = 0.0f, xHi = 1.0f;
    if (v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        if (bandSpan > 0.0 && viewSpan > 0.0)
        {
            double visLo = std::max(bandMin, v.viewXminMHz);
            double visHi = std::min(bandMax, v.viewXmaxMHz);
            if (visHi > visLo)
            {
                uMin = (float)((visLo - bandMin) / bandSpan);
                uMax = (float)((visHi - bandMin) / bandSpan);
                xLo = (float)((visLo - v.viewXminMHz) / viewSpan);
                xHi = (float)((visHi - v.viewXminMHz) / viewSpan);
            }
            else
            {
                xLo = xHi = 0.0f;
            }
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left = std::max(0.0f, v.specLeftInset);
    float right = std::max(0.0f, v.specRightInset);
    float w = avail.x - left - right;
    if (w < 1.0f)
    {
        w = avail.x;
        left = 0.0f;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);

    ImVec2 wfP0 = ImGui::GetCursorScreenPos();
    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
    v.fftSkip = false;

    if (app.placingDecoder && app.placingRx == idx && v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        double visLo = std::max(bandMin, v.viewXminMHz);
        double visHi = std::min(bandMax, v.viewXmaxMHz);
        if (bandSpan > 0 && viewSpan > 0 &&
            app.placingFreqMHz >= visLo && app.placingFreqMHz <= visHi)
        {
            float u = (float)((app.placingFreqMHz - visLo) / (visHi - visLo));
            float pixFrac = xLo + u * (xHi - xLo);
            float px = wfP0.x + pixFrac * w;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(px, wfP0.y), ImVec2(px, wfP0.y + avail.y),
                        IM_COL32(255, 40, 40, 200), 1.5f);
        }
    }
    ImGui::End();
}

void drawDecoders(App& app)
{
    ImGui::Begin((std::string(_L("Decoders")) + "###Decoders").c_str());

    struct Row { Receiver* r; DecoderManager::Status s; };
    std::vector<Row> rows;
    int subbands = 0, threads = 0;
    uint64_t drops = 0;
    for (auto& rp : app.rx)
    {
        if (!rp) continue;
        for (auto& s : rp->decoders.status())
            rows.push_back({rp.get(), s});
        subbands += rp->decoders.subbandCount();
        threads += rp->decoders.workerCount();
        drops += rp->decoders.drops();
    }
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)rows.size(), subbands, threads);
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu", (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Remove all")))
        for (auto& rp : app.rx) rp->decoders.removeAll();
    static std::string decsCopy;
    ImGui::SameLine();
    copyAllButton(decsCopy);

    if (ImGui::Checkbox(_L("CPU reduce"), &app.cpuReduce))
        for (auto& rp : app.rx) rp->decoders.setCpuReduce(app.cpuReduce);

    if (app.audioDevs.empty())
        app.audioDevs = app.rx.front()->decoders.audioDevices();
    {
        std::vector<const char*> names;
        for (auto& s : app.audioDevs) names.push_back(s.c_str());
        if (app.audioDevice >= (int)names.size()) app.audioDevice = 0;
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::Combo("Audio out", &app.audioDevice, names.data(), (int)names.size()))
            for (auto& rp : app.rx) rp->decoders.setAudioDevice(app.audioDevice);
    }
    ImGui::Checkbox("Save decoders on restart", &app.saveDecoders);

    ImGui::Separator();

    if (ImGui::BeginTable("##decs", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Rx", ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Freq MHz");
        ImGui::TableSetupColumn("Baud");
        ImGui::TableSetupColumn("Msgs");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        Receiver* toRemoveRx = nullptr;
        int toRemove = -1;
        std::vector<std::string> copyRows;
        for (auto& row : rows)
        {
            auto& d = row.s;
            std::string uid = std::to_string((uintptr_t)row.r) + "_" + std::to_string(d.channelId);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", (int)(std::find_if(app.rx.begin(), app.rx.end(),
                    [&](const std::unique_ptr<Receiver>& p){ return p.get() == row.r; }) -
                    app.rx.begin()) + 1);
            ImGui::TableNextColumn();
            ImVec4 c = d.locked ? Lc(app, ImVec4(0.2f, 1.0f, 0.3f, 1.0f))
                                : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, c);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x*1.3f, c.y*1.3f, c.z*1.3f, 1.0f));
            char selid[32];
            std::snprintf(selid, sizeof(selid), "##sel_%s", uid.c_str());
            if (ImGui::Selectable(selid, app.selectedDecoder == d.channelId))
            {
                app.selectedDecoder = d.channelId;
                app.selectedRx = (int)(std::find_if(app.rx.begin(), app.rx.end(),
                    [&](const std::unique_ptr<Receiver>& p){ return p.get() == row.r; }) - app.rx.begin());
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextColored(c, "%s", d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.msgs);
            ImGui::TableNextColumn();
            char btn[32];
            std::snprintf(btn, sizeof(btn), "X##%s", uid.c_str());
            if (ImGui::SmallButton(btn))
            {
                toRemoveRx = row.r;
                toRemove = d.channelId;
            }
            copyRows.push_back(copyFmt("%.4f\t%d\t%llu", d.freqMHz, d.baud,
                (unsigned long long)d.msgs));
        }
        handleTableCopy(copyRows);
        decsCopy = copyJoin(copyRows);
        ImGui::EndTable();
        if (toRemoveRx && toRemove >= 0)
            toRemoveRx->decoders.removeDecoder(toRemove);
    }

    ImGui::End();
}

void drawMessages(App& app)
{
    ImGui::Begin((std::string(_L("Messages")) + "###Messages").c_str());

    unsigned long long msgTotal = 0;
    for (auto& rp : app.rx) msgTotal += rp->decoders.log().count();
    ImGui::Text("%llu total", msgTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        for (auto& rp : app.rx) rp->decoders.log().clear();
    static std::string msgCopy;
    ImGui::SameLine();
    copyAllButton(msgCopy);
    ImGui::SameLine();
    ImGui::Checkbox(_L("Show empty"), &app.showEmptyMsgs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchmsg", "Search...", app.searchBuf, sizeof(app.searchBuf));

    // Session archive dropdown — reloads ACARS from a past DB file.
    {
        double now = (double)std::time(nullptr);
        if (app.archiveDbLastScan == 0.0 || now - app.archiveDbLastScan > 3.0)
        {
            app.archiveDbPaths.clear();
            app.archiveDbLabels.clear();
            auto files = app.writeDb.scanArchives("databases");
            for (auto& f : files)
            {
                app.archiveDbPaths.push_back(f.filename);
                app.archiveDbLabels.push_back(f.displayLabel + "  (" +
                                              std::to_string(f.rowCount) + " msgs)");
            }
            app.archiveDbLastScan = now;
        }
        if (!app.archiveDbLabels.empty())
        {
            std::vector<const char*> items;
            items.push_back("Live");
            for (auto& lbl : app.archiveDbLabels) items.push_back(lbl.c_str());
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Session", &app.archiveComboMsg, items.data(), (int)items.size()))
            {
                auto& log = app.rx.front()->decoders.log();
                if (app.archiveComboMsg == 0)
                    log.clearArchive();
                else
                {
                    int idx = app.archiveComboMsg - 1;
                    if (idx < (int)app.archiveDbPaths.size())
                        app.writeDb.loadAcarsOrSu(app.archiveDbPaths[idx], MessageStore::ACARS, &log, 0);
                }
            }
            if (app.rx.front()->decoders.log().hasArchive())
                ImGui::TextDisabled("  Viewing archived session");
        }
    }
    ImGui::Separator();

    std::vector<DecodedMessage> msgs;
    for (auto& rp : app.rx)
    {
        auto b = rp->decoders.log().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }
    if (ImGui::BeginTable("##msgs", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Lbl", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Text");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        std::vector<std::string> copyRows;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        {
            if (!app.showEmptyMsgs && it->text.empty() && it->decoded.empty())
                continue;
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->hex + "|" + it->reg + "|"
                                + it->label + "|" + it->icao + "|" + it->decoded;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(rowIdx++);
            ImGui::TableNextColumn();
            {
                time_t t = (time_t)it->timeSec;
                std::tm tm{};
#if defined(_WIN32)
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                ImGui::TextDisabled("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->downlink ? "DL" : "UL");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->reg.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%06X", it->aesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", it->text.c_str());
            if (it->hasPos)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
                                   "POS %.4f, %.4f  %d ft", it->lat, it->lon, it->alt);
            if (!it->decoded.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Lc(app, ImVec4(0.6f, 1.0f, 0.6f, 1.0f)));
                ImGui::TextWrapped("%s", it->decoded.c_str());
                ImGui::PopStyleColor();
            }
            char utc[16];
            {
                time_t t = (time_t)it->timeSec;
                std::tm tm{};
#if defined(_WIN32)
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                std::snprintf(utc, sizeof(utc), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            std::string row = copyFmt("%s\t%.3f\t%s\t%s\t%06X\t%s\t%s",
                                      utc, it->freqMHz, it->downlink ? "DL" : "UL",
                                      it->reg.c_str(), it->aesId, it->label.c_str(),
                                      it->text.c_str());
            if (!it->decoded.empty()) { row += '\n'; row += it->decoded; }
            copyRows.push_back(std::move(row));
            ImGui::PopID();
        }
        handleTableCopy(copyRows);
        msgCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawAircraft(App& app)
{
    ImGui::Begin((std::string(_L("Aircraft")) + "###Aircraft").c_str());

    std::vector<AircraftEntry> acs;
    for (auto& rp : app.rx)
    {
        auto b = rp->decoders.aircraftTable().snapshot();
        acs.insert(acs.end(), b.begin(), b.end());
    }
    ImGui::Text("%zu tracked", acs.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        for (auto& rp : app.rx) rp->decoders.aircraftTable().clear();
    static std::string acCopy;
    ImGui::SameLine();
    copyAllButton(acCopy);
    ImGui::SameLine();
    ImGui::Checkbox(_L("With position only"), &app.acPosOnly);
    ImGui::Separator();

    double now = (double)std::time(nullptr);
    std::sort(acs.begin(), acs.end(),
              [](const AircraftEntry& a, const AircraftEntry& b) { return a.lastSeen > b.lastSeen; });

    if (ImGui::BeginTable("##aircraft", 10,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("AES", ImGuiTableColumnFlags_WidthFixed, 58);
        ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("Ctry", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Flight", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("Alt", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<std::string> copyRows;
        for (const auto& a : acs)
        {
            if (app.acPosOnly && !a.hasPos)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%06X", a.aesId);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.icao.c_str());
            ImGui::TableNextColumn();
            const char* cc = nullptr;
            if (!a.icao.empty())
            {
                uint32_t ihex = (uint32_t)std::strtoul(a.icao.c_str(), nullptr, 16);
                cc = icaoCountry(ihex);
                if (cc) ImGui::TextUnformatted(cc);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.reg.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.flight.c_str());
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%.4f", a.lat);
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%.4f", a.lon);
            ImGui::TableNextColumn();
            if (a.hasPos) ImGui::Text("%d", a.alt);
            ImGui::TableNextColumn();
            ImGui::Text("%ds", (int)(now - a.lastSeen));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)a.msgs);
            copyRows.push_back(copyFmt("%06X\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%ds\t%llu",
                a.aesId, a.icao.c_str(), cc ? cc : "",
                a.reg.c_str(), a.flight.c_str(),
                a.hasPos ? copyFmt("%.4f", a.lat).c_str() : "",
                a.hasPos ? copyFmt("%.4f", a.lon).c_str() : "",
                a.hasPos ? copyFmt("%d", a.alt).c_str() : "",
                (int)(now - a.lastSeen), (unsigned long long)a.msgs));
        }
        handleTableCopy(copyRows);
        acCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

#if defined(_WIN32)
void drawFlightMap(App& app)
{
    ImGui::Begin((std::string(_L("Flight Map")) + "###Flight Map").c_str());

    const AircraftEntry* pick = nullptr;
    for (auto& rp : app.rx)
    {
        for (auto& a : rp->decoders.aircraftTable().snapshot())
            if (!a.icao.empty()) { static AircraftEntry keep; keep = a; pick = &keep; break; }
        if (pick) break;
    }

    if (pick && !app.flightMapWv.isReady())
    {
        ImGui::Text("%s  %s  %06X", pick->icao.c_str(),
                    pick->flight.empty() ? pick->reg.c_str() : pick->flight.c_str(), pick->aesId);
        if (pick->hasPos)
            ImGui::SameLine(); ImGui::Text("  %.4f,%.4f  %d ft", pick->lat, pick->lon, pick->alt);
    }
    else if (!pick && !app.flightMapWv.isReady())
    {
        ImGui::TextDisabled("No aircraft with ICAO yet.");
    }
    if (!app.flightMapWv.isReady())
        ImGui::TextDisabled("  Loading map...");

    ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = std::max(1, (int)avail.x);
    int h = std::max(1, (int)avail.y);
    bool tabActive = true;
    if (ImGuiDockNode* node = ImGui::GetCurrentWindow()->DockNode)
        tabActive = (node->VisibleWindow == ImGui::GetCurrentWindow());
    ImGui::InvisibleButton("##map", ImVec2((float)w, (float)h));
    bool inMainViewport = (ImGui::GetWindowViewport() == ImGui::GetMainViewport());
    if (!inMainViewport)
    {
        app.flightMapWv.setBounds(0, 0, 0, 0, false);
        ImGui::SetCursorScreenPos(pos);
        ImGui::TextWrapped("%s", _L("Dock the Flight Map back in the main window to show the embedded map."));
    }
    else
        app.flightMapWv.setBounds((int)pos.x, (int)pos.y, w, h, tabActive);

    static std::string lastIcao;
    if (pick && pick->icao != lastIcao)
    {
        lastIcao = pick->icao;
        app.flightMapWv.setIcao(pick->icao);
    }

    ImGui::End();
}
#else
void drawFlightMap(App&) {}
#endif // _WIN32

ImPlotPoint constGetter(int idx, void* data)
{
    const float* p = static_cast<const float*>(data);
    return ImPlotPoint(p[idx * 2], p[idx * 2 + 1]);
}

void drawConstellation(App& app)
{
    ImGui::Begin((std::string(_L("Constellation")) + "###Constellation").c_str());

    struct Row { Receiver* r; DecoderManager::Status s; };
    std::vector<Row> rows;
    for (auto& rp : app.rx)
    {
        if (!rp) continue;
        for (auto& s : rp->decoders.status())
            rows.push_back({rp.get(), s});
    }

    int chan = app.selectedDecoder;
    Receiver* sel = nullptr;
    double freq = 0.0;
    int preBaud = 0;
    for (auto& row : rows)
        if (row.s.channelId == chan && (sel == nullptr ||
            row.r == (app.selectedRx < (int)app.rx.size() ? app.rx[app.selectedRx].get() : nullptr)))
        { sel = row.r; freq = row.s.freqMHz; preBaud = row.s.baud; break; }
    if (!sel && !rows.empty()) { sel = rows.front().r; chan = rows.front().s.channelId;
        freq = rows.front().s.freqMHz; preBaud = rows.front().s.baud; }

    if (rows.empty())
    {
        ImGui::TextDisabled("No decoders. Ctrl+click the spectrum to add one.");
        ImGui::End();
        return;
    }

    char preview[128];
    std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  @%d", chan, freq, preBaud);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Decoder", preview))
    {
        for (auto& row : rows)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  @%d",
                          row.s.channelId, row.s.freqMHz, row.s.baud);
            if (ImGui::Selectable(label, row.s.channelId == chan && row.r == sel))
            {
                app.selectedDecoder = row.s.channelId;
                chan = row.s.channelId;
                sel = row.r;
            }
        }
        ImGui::EndCombo();
    }

    int pairs = 0;
    if (sel)
        pairs = sel->decoders.getConstellation(chan, app.constBuf, 1024);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d pts)", pairs);

    auto nowC = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(nowC - app.constLimTime).count() >= 1.0)
    {
        float m = 0.5f;
        for (float v : app.constBuf) m = std::max(m, std::fabs(v));
        app.constLim = m * 1.15;
        app.constLimTime = nowC;
    }
    double lim = app.constLim;

    if (ImPlot::BeginPlot("##const", ImVec2(-1, -1),
                          ImPlotFlags_Equal | ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -lim, lim, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -lim, lim, ImGuiCond_Always);
        if (pairs > 0)
            ImPlot::PlotScatterG("IQ", constGetter, app.constBuf.data(), pairs);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

void drawVoiceCalls(App& app)
{
    ImGui::Begin((std::string(_L("Voice Calls")) + "###Voice Calls").c_str());

    std::vector<VoiceCallRecord> calls;
    uint64_t total = 0;
    for (auto& rp : app.rx)
    {
        auto b = rp->decoders.voiceCallLog().snapshot();
        calls.insert(calls.end(), b.begin(), b.end());
        total += rp->decoders.voiceCallLog().count();
    }
    std::sort(calls.begin(), calls.end(),
              [](const VoiceCallRecord& a, const VoiceCallRecord& b) { return a.timeSec > b.timeSec; });

    ImGui::Text("%llu calls", (unsigned long long)total);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        for (auto& rp : app.rx) rp->decoders.voiceCallLog().clear();
    static std::string vcCopy;
    ImGui::SameLine();
    copyAllButton(vcCopy);
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan"))
        for (auto& rp : app.rx) rp->decoders.voiceCallLog().scanDir(app.recordDir);
    ImGui::SameLine();

    if (app.audioPlayer.isPlaying())
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "  Playing... %.0fs",
                           (double)app.audioPlayer.positionSec());
    else
        ImGui::TextDisabled("  Idle");

    if (ImGui::BeginTable("##vclist", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 68);
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 72);
        ImGui::TableSetupColumn(">");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        std::vector<std::string> copyRows;
        for (auto& c : calls)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            time_t t = (time_t)c.timeSec;
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            ImGui::Text("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", c.freqMHz);
            ImGui::TableNextColumn();
            if (!c.icao.empty())
                ImGui::TextColored(Lc(app, ImVec4(1.0f, 0.85f, 0.3f, 1.0f)), "%s", c.icao.c_str());
            else if (c.aesId)
                ImGui::TextDisabled("%06X", c.aesId);
            else
                ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (c.recording)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Rec");
            else if (c.durationSec > 0.0)
            {
                int m = (int)c.durationSec / 60;
                int s = (int)c.durationSec % 60;
                ImGui::Text("%d:%02d", m, s);
            }
            else
                ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            bool sel = app.audioPlayer.isPlaying() && !c.filename.empty() &&
                       app.audioPlayer.currentPath().find(c.filename) != std::string::npos;
            char label[24];
            std::snprintf(label, sizeof(label), "%s##vcp%d", sel ? "||" : ">", rowIdx);
            if (ImGui::SmallButton(label))
            {
                if (sel)
                    app.audioPlayer.stop();
                else if (!c.filename.empty())
                    app.audioPlayer.play(std::string(app.recordDir) + "/" + c.filename);
            }
            copyRows.push_back(copyFmt("%02d:%02d:%02d\t%.4f\t%s\t%s",
                tm.tm_hour, tm.tm_min, tm.tm_sec, c.freqMHz,
                c.icao.empty() ? (c.aesId ? copyFmt("%06X", c.aesId).c_str() : "--") : c.icao.c_str(),
                c.filename.c_str()));
            rowIdx++;
        }
        handleTableCopy(copyRows);
        vcCopy = copyJoin(copyRows);
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawAbout(App& app)
{
    if (!app.showAbout)
        return;

    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin((std::string(_L("About AirScope")) + "###About AirScope").c_str(), &app.showAbout,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextWrapped("AirScope v" AIRSCOPE_VERSION);
        ImGui::Separator();
        ImGui::TextWrapped("AirScope was created by Sarah Rose.");
        ImGui::Spacing();
        ImGui::TextWrapped("Built with components from:");
        ImGui::TextDisabled("  goadsb (Sarah Rose)");
        ImGui::TextDisabled("  acarsdec (Thierry Leconte)");
        ImGui::TextDisabled("  libacars (Tomasz Lemiech)");
        ImGui::TextDisabled("  Dear ImGui / ImPlot");
        ImGui::Spacing();
        ImGui::TextWrapped("Thanks to Mike AA8IA for donating an Airspy R2 and Airspy Mini for development.");
    }
    ImGui::End();
}

void drawDockHost(App& app)
{
    static bool forceLayout = false;
    if (app.forceDefaultLayout) { forceLayout = true; app.forceDefaultLayout = false; }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##AirScopeHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("AirScopeDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

    if (forceLayout || ImGui::DockBuilderGetNode(dockId) == nullptr)
    {
        forceLayout = false;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);

        ImGuiID left, right, rtop, rmid, rbot, rcon, ctrl, dec;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.32f, &left, &right);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.68f, &ctrl, &dec);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, &rtop, &rbot);
        ImGui::DockBuilderSplitNode(rtop, ImGuiDir_Up, 0.55f, &rtop, &rmid);
        ImGui::DockBuilderSplitNode(rbot, ImGuiDir_Right, 0.34f, &rcon, &rbot);

        ImGui::DockBuilderDockWindow((std::string(_L("Control")) + "###Control").c_str(), ctrl);
        ImGui::DockBuilderDockWindow((std::string(_L("Decoders")) + "###Decoders").c_str(), dec);

        int disp = 0;
        for (size_t i = 0; i < app.rx.size(); ++i)
        {
            if (!app.rx[i]->showSpectrum) continue;
            std::string st = std::string(_L("Spectrum")) + "###Spectrum" + std::to_string(i);
            std::string wt = std::string(_L("Waterfall")) + "###Waterfall" + std::to_string(i);
            ImGui::DockBuilderDockWindow(st.c_str(), rtop);
            ImGui::DockBuilderDockWindow(wt.c_str(), rmid);
            ++disp;
        }
        (void)disp;
        ImGui::DockBuilderDockWindow((std::string(_L("Messages")) + "###Messages").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Aircraft")) + "###Aircraft").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Voice Calls")) + "###Voice Calls").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("Constellation")) + "###Constellation").c_str(), rcon);
        ImGui::DockBuilderFinish(dockId);
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu(_L("View")))
        {
            if (ImGui::MenuItem(_L("Reset Layout")))
                forceLayout = true;
            ImGui::Separator();
            ImGui::MenuItem(_L("Drag a tab out to float a pane on the desktop"), nullptr, false, false);
            ImGui::MenuItem(_L("Right-click a table row (or Ctrl+C) to copy"), nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_L("Help")))
        {
            if (ImGui::BeginMenu(_L("Languages")))
            {
                for (int i = 0; i < (int)Lang::KOUNT; ++i)
                {
                    Lang l = (Lang)i;
                    if (ImGui::MenuItem(i18nName(l), nullptr, app.languageIdx == i))
                    {
                        app.languageIdx = i;
                        i18nSet(l);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(_L("About")))
                app.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}
