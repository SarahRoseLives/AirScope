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

// Light-mode colour dimmer — accent colours that work on a dark background
// are too washed-out on a white one.  Halve the RGB channels when in light mode.
inline ImVec4 Lc(const App& app, const ImVec4& c) {
    if (!app.lightMode) return c;
    return ImVec4(c.x * 0.44f, c.y * 0.56f, c.z * 0.44f, c.w);
}

// PPM crystal-offset control: typed value plus a slider for fine adjustment.
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

void drawControls(App& app)
{
    ImGui::Begin((std::string(_L("Control")) + "###Control").c_str());

    bool running = app.active->running();

    ImGui::BeginDisabled(running);
#ifdef HAS_AIRSPY
    const char* modes[] = {"RTL-SDR", "WAV file", "Dual RTL", "Airspy"};
    int modeSel = (app.sourceMode == 1) ? 1 : (app.sourceMode == 4) ? 2
                : (app.sourceMode == 5) ? 3 : 0;
    if (ImGui::Combo(_L("Source"), &modeSel, modes, 4))
        app.sourceMode = (modeSel == 1) ? 1 : (modeSel == 2) ? 4
                     : (modeSel == 3) ? 5 : 0;
#else
    const char* modes[] = {"RTL-SDR", "WAV file", "Dual RTL"};
    int modeSel = (app.sourceMode == 1) ? 1 : (app.sourceMode == 4) ? 2 : 0;
    if (ImGui::Combo(_L("Source"), &modeSel, modes, 3))
        app.sourceMode = (modeSel == 1) ? 1 : (modeSel == 2) ? 4 : 0;
#endif
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!running)
    {
        bool canStart = (app.sourceMode == 1) ? (app.wavPath[0] != '\0') : true;
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button(_L("Start"), ImVec2(120, 0)))
            startActive(app);
        ImGui::EndDisabled();
    }
    else
    {
        if (ImGui::Button(_L("Stop"), ImVec2(120, 0)))
        {
            app.active->stop();
            app.decoders.stop();
            app.decoders.removeAll();
            if (app.dualMode)
            {
                app.sdrB.stop();
                app.decodersB.stop();
                app.decodersB.removeAll();
            }
            app.dualMode = false;
            app.status = "Idle";
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.status.c_str());

    // Version + update banner.
    ImGui::TextDisabled("AirScope v" AIRSCOPE_VERSION);
    {
        VersionCheck::State st = app.verCheck.state();
        if (st == VersionCheck::UpdateAvailable)
        {
            std::string latest = app.verCheck.latestVersion();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Update available: v%s",
                               latest.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Get update"))
            {
#if defined(_WIN32)
                std::string url = app.verCheck.productUrl();
                if (url.empty()) url = "https://sarahsforge.dev/login";
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else if (st == VersionCheck::UpToDate)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  (up to date)");
        }
        else if (st == VersionCheck::Unreleased)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "  (unreleased)");
        }
        else if (st == VersionCheck::Checking)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("  checking for updates...");
        }
    }

    ImGui::Separator();

    if (app.sourceMode == 0)
    {
        // ---- RTL-SDR ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());

        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("No RTL-SDR devices. Click Refresh.");
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox(_L("Auto gain (AGC)"), &app.autoGain))
        {
            if (running)
                app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        }
        if (!app.autoGain)
        {
            if (ImGui::SliderFloat("Gain (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f"))
            {
                if (running)
                    app.sdr.setGain((double)app.gainDb);
            }
        }
        if (ImGui::Checkbox(_L("Bias-T"), &app.biasTee))
        {
            if (running)
                app.sdr.setBiasTee(app.biasTee);
        }
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running)
                app.sdr.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running)
                app.sdr.setDcBlock(app.dcBlock);
        }
    }
    else if (app.sourceMode == 1)
    {
        // ---- WAV file ----
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##wavpath", app.wavPath, sizeof(app.wavPath));
        ImGui::SameLine();
        if (ImGui::Button(_L("Browse...")))
            openWavDialog(app.wavPath, sizeof(app.wavPath));

        if (ImGui::Checkbox(_L("Loop"), &app.wavLoop))
        {
            if (running)
                app.wav.setLoop(app.wavLoop);
        }
        if (ImGui::InputDouble("Center label (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;

        if (running)
        {
            ImGui::ProgressBar((float)app.wav.progress(), ImVec2(-1, 0));
            ImGui::Text("WAV: %d ch, %d-bit, %.1f kHz",
                        app.wav.channels(), app.wav.bits(), app.wav.sampleRate() / 1e3);
        }
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        // ---- Airspy (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.airspy.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.airspySampleRateIdx, kAirspyRateLabels, kAirspyNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        }

        ImGui::Separator();
        if (ImGui::RadioButton("Sensitive", app.airspyGainMode == 0)) app.airspyGainMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Linear", app.airspyGainMode == 1)) app.airspyGainMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Free", app.airspyGainMode == 2)) app.airspyGainMode = 2;

        if (app.airspyGainMode == 0)
        {
            if (ImGui::SliderInt("Sensitivity gain", &app.airspySenseGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(0), app.airspy.setSenseGain(app.airspySenseGain);
            }
        }
        else if (app.airspyGainMode == 1)
        {
            if (ImGui::SliderInt("Linearity gain", &app.airspyLinearGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(1), app.airspy.setLinearGain(app.airspyLinearGain);
            }
        }
        else
        {
            if (ImGui::Checkbox("LNA AGC", &app.airspyLnaAgc))
            {
                if (running) app.airspy.setLnaAgc(app.airspyLnaAgc);
            }
            ImGui::BeginDisabled(app.airspyLnaAgc);
            if (ImGui::SliderInt("LNA gain", &app.airspyLnaGain, 0, 15))
            {
                if (running) app.airspy.setLnaGain(app.airspyLnaGain);
            }
            ImGui::EndDisabled();

            if (ImGui::Checkbox("Mixer AGC", &app.airspyMixerAgc))
            {
                if (running) app.airspy.setMixerAgc(app.airspyMixerAgc);
            }
            ImGui::BeginDisabled(app.airspyMixerAgc);
            if (ImGui::SliderInt("Mixer gain", &app.airspyMixerGain, 0, 15))
            {
                if (running) app.airspy.setMixerGain(app.airspyMixerGain);
            }
            ImGui::EndDisabled();

            if (ImGui::SliderInt("VGA gain", &app.airspyVgaGain, 0, 15))
            {
                if (running) app.airspy.setVgaGain(app.airspyVgaGain);
            }
        }
        if (ImGui::Checkbox("Bias T (antenna power)", &app.airspyBias))
        {
            if (running) app.airspy.setBiasTee(app.airspyBias);
        }
        if (drawPpmAdjust("PPM", &app.ppm))
        {
            if (running) app.airspy.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.airspy.setDcBlock(app.dcBlock);
        }
    }
#endif
    if (app.sourceMode == 4)
    {
        // ---- Dual RTL: two independent RTL-SDRs ----
        ImGui::TextColored(Lc(app, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), "RTL A (Spectrum / Waterfall A)");
        ImGui::Separator();
        if (ImGui::Button("Refresh A"))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device A", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center A (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;
        ImGui::Combo("Rate A (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain A", &app.autoGain)) {}
        if (!app.autoGain)
            ImGui::SliderFloat("Gain A (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T A", &app.biasTee);
        if (drawPpmAdjust("PPM A", &app.ppm) && running)
            app.sdr.setPpm((double)app.ppm);
        ImGui::Checkbox("DC block A", &app.dcBlock);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "RTL B (Spectrum / Waterfall B)");
        ImGui::Separator();
        if (ImGui::Button("Refresh B"))
            app.devices = app.sdrB.listDevices();
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndexB, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device B", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndexB == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndexB = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center B (MHz)", &app.centerFreqMHzB, 0.1, 1.0, "%.4f"))
            app.viewB.resetView = true;
        ImGui::Combo("Rate B (MHz)", &app.sampleRateIdxB, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain B", &app.autoGainB)) {}
        if (!app.autoGainB)
            ImGui::SliderFloat("Gain B (dB)", &app.gainDbB, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T B", &app.biasTeeB);
        if (drawPpmAdjust("PPM B", &app.ppmB) && running)
            app.sdrB.setPpm((double)app.ppmB);
        ImGui::Checkbox("DC block B", &app.dcBlock); // same dcblock toggle
    }

    ImGui::Separator();
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat(_L("Averaging"), &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox(_L("Auto-scale dB"), &app.autoScale);
    ImGui::SliderFloat(_L("dB min"), &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat(_L("dB max"), &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;

    if (ImGui::Button(_L("Reset view (fit band)")))
    {
        app.viewA.resetView = true;
        app.viewB.resetView = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag=pan  scroll=zoom  dbl-click=fit");

    ImGui::BeginDisabled(app.sourceMode == 1);
    ImGui::Checkbox("Pan/scroll retunes SDR (browse band)", &app.bandBrowse);
    ImGui::EndDisabled();
    if (app.sourceMode == 1)
        ImGui::TextDisabled("  (WAV: tuning is fixed to the file)");

    // Band plan bar along bottom of spectrum
    if (ImGui::Checkbox(_L("Band Plan"), &app.showBandPlan));
    if (app.showBandPlan)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##bpr"))
            scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
        ImGui::SameLine();
        if (ImGui::SmallButton("Folder##bpf"))
        {
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", app.bandPlanDir, nullptr, nullptr, SW_SHOW);
#endif
        }
        if (app.bandPlanNames.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(no .json bandplans in bandplans/)");
        }
        else
        {
            if (app.bandPlanIdx >= (int)app.bandPlanNames.size())
                app.bandPlanIdx = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel", &app.bandPlanIdx,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
                    app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
            }
        }
    }

    if (app.dualMode)
    {
        if (ImGui::Checkbox(_L("Band Plan (B)"), &app.showBandPlanB));
        if (app.showBandPlanB && !app.bandPlanNames.empty())
        {
            if (app.bandPlanIdxB >= (int)app.bandPlanNames.size())
                app.bandPlanIdxB = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel-b", &app.bandPlanIdxB,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
                    app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Ctrl+click the spectrum to add a channel decoder there");

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Database (SQLite log)")))
    {
        ImGui::Checkbox(_L("Log messages to database"), &app.logToDb);
        if (app.writeDb.enabled())
            ImGui::TextDisabled("  Session DB is active");
        if (ImGui::SliderInt(_L("Keep DB (days)"), &app.maxDbAgeDays, 1, 90))
        {
            if (app.maxDbAgeDays < 1) app.maxDbAgeDays = 1;
            if (app.maxDbAgeDays > 90) app.maxDbAgeDays = 90;
        }
        ImGui::TextDisabled("  Archives in: .\\databases\\messages_*.db");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Display")))
    {
        if (ImGui::Checkbox(_L("Light mode"), &app.lightMode))
        {
            if (app.lightMode)
            {
                ImGui::StyleColorsLight();
                ImGuiStyle& st = ImGui::GetStyle();
                st.Colors[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
                st.Colors[ImGuiCol_TextDisabled]           = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
                st.Colors[ImGuiCol_WindowBg]               = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
                st.Colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.73f, 0.73f, 0.73f, 1.00f);
                st.Colors[ImGuiCol_TableRowBg]             = ImVec4(0.97f, 0.97f, 0.97f, 1.00f);
                st.Colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
                st.Colors[ImGuiCol_FrameBg]                = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
            }
            else
                ImGui::StyleColorsDark();
        }
        if (ImGui::SliderInt(_L("Font size"), &app.fontSize, 8, 24, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            if (app.fontSize < 8)  app.fontSize = 8;
            if (app.fontSize > 24) app.fontSize = 24;
        }
        ImGui::TextDisabled("  Restart to apply");
    }

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
                                   "Bind failed on :%d (port in use? try another)",
                                   app.outSbsPort);
        }
        ImGui::Text("Sent: %llu", (unsigned long long)app.feed.sent());
        ImGui::TextDisabled("ACARS -> JAERO JSONdump.");
        ImGui::TextDisabled("SBS: VRS receiver -> Network, 127.0.0.1, this port, BaseStation.");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
        // Pre-buffer slider (disabled above 3 Msps)
        double fs = app.active->running() ? app.active->sampleRate() : 0.0;
        if (!app.active->running())
            ImGui::BeginDisabled();
        bool overLimit = (fs > 3.0e6);
        if (overLimit)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Pre-buffer (s)", &app.iqBufferSec, 0.0f, 60.0f, "%.0f"))
        {
            app.iqRecorder.configurePrebuffer(fs, app.iqBufferSec);
        }
        if (overLimit)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(disabled > 3 Msps)");
        }
        else if (app.iqBufferSec > 0.0f && fs > 0.0)
        {
            size_t bytes = (size_t)(fs * app.iqBufferSec * 2 * sizeof(float));
            char mem[32];
            if (bytes >= 1024 * 1024 * 1024)
                std::snprintf(mem, sizeof(mem), "(~%.1f GB)", (double)bytes / (1024.0 * 1024.0 * 1024.0));
            else
                std::snprintf(mem, sizeof(mem), "(~%.0f MB)", (double)bytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mem);
        }
        if (!app.active->running())
            ImGui::EndDisabled();
        bool iqRec = app.iqRecorder.isRecording();
        if (iqRec)
        {
            if (ImGui::Button("Stop##iqrec"))
                app.iqRecorder.stop();
        }
        else
        {
            if (ImGui::Button("Start##iqrec"))
            {
                if (app.active && app.active->running())
                    app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
            }
        }
        if (app.iqRecorder.isRecording())
        {
            double sec = app.iqRecorder.elapsed();
            int m = (int)(sec / 60), s = (int)(sec) % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC %02d:%02d  —  %s",
                               m, s, app.iqRecorder.path().c_str());
        }
    }

    if (running)
    {
        if (app.sourceMode == 0)
        {
            double maxF = app.sdr.tunerMaxFreq();
            if (maxF > 0.0 && app.centerFreqMHz * 1e6 > maxF)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
                ImGui::TextWrapped("  WARNING: %.0f MHz is above this tuner's ~%.0f MHz "
                                   "ceiling. The PLL can't lock here - pick a lower "
                                   "frequency or use an R820T2/R828D dongle.",
                                   app.centerFreqMHz, maxF / 1e6);
                ImGui::PopStyleColor();
            }
        }
    }

    // ---- Web Dashboard ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Web Dashboard"))
    {
        if (ImGui::Checkbox("Enable server", &app.webServerEnabled))
        {
            if (app.webServerEnabled)
            {
                app.webServer.decodersA = &app.decoders;
                app.webServer.decodersB = &app.decodersB;
                app.webServer.dualMode = &app.dualMode;
                app.webServer.active = &app.active;
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
            char url[64];
            std::snprintf(url, sizeof(url), "http://localhost:%d", app.webServerPort);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running on port %d", app.webServerPort);
            ImGui::SameLine();
            if (ImGui::SmallButton("Open in browser"))
            {
#if defined(_WIN32)
                ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
            ImGui::SameLine();
            ImGui::TextDisabled(url);
        }
    }

    ImGui::End();
}

void drawSpectrum(App& app, SpectrumView& v, DecoderManager& mgr, const char* title,
                         bool allowBandBrowse, bool voiceView)
{
    ImGui::Begin(title);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    std::string plotId = std::string("##plot_") + title;
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
        {
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);
        }

        auto decs = mgr.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)   // green = locked
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f);  // orange = unlocked
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                mgr.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;

        // Band-browse retuning: in dual mode use explicit SDR pointers,
        // otherwise use app.active (which covers RTL/WAV/SDR++/HackRF).
        SdrSource* browseSdr;
        if (app.dualMode)
            browseSdr = voiceView ? static_cast<SdrSource*>(&app.sdrB)
                                  : static_cast<SdrSource*>(&app.sdr);
        else
            browseSdr = app.active;
        if (allowBandBrowse && app.bandBrowse && app.sourceMode != 1 &&
            browseSdr->running() && !v.resetView)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = browseSdr->centerFreq() / 1e6;
            double fsMHz = browseSdr->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - app.lastRetuneCtr) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                if (voiceView)
                {
                    // Retune SDR B preserving decoders on manager B
                    std::vector<std::pair<double, int>> keep;
                    for (auto& s : app.decodersB.status())
                        keep.push_back({s.freqMHz, s.baud});
                    app.centerFreqMHzB = viewCtr;
                    app.sdrB.setCenterFreq(viewCtr * 1e6);
                    app.decodersB.removeAll();
                    app.decodersB.configure(app.sdrB.sampleRate(), app.sdrB.centerFreq());
                    for (auto& k : keep)
                        app.decodersB.addDecoder(k.first * 1e6, k.second);
                }
                else
                {
                    retunePreserving(app, viewCtr);
                }
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

        // Drag-to-place decoder: Ctrl+mousedown starts placing, move shows a white
        // preview line through the spectrum and waterfall, release creates the decoder.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl)
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                app.placingDecoder = true;
                app.placingVoiceView = voiceView;
                app.placingFreqMHz = mp.x;
            }
        if (app.placingDecoder && app.placingVoiceView == voiceView)
            {
                app.placingFreqMHz = mp.x;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                mgr.addDecoder(mp.x * 1e6, 1200);
            }
        }
        else if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            // Ctrl released or cursor left the plot that started the placement.
            app.placingDecoder = false;
        }

        // Drag-to-place preview line redraw
        if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            ImPlotRect lim = ImPlot::GetPlotLimits();
            float xMin = (float)lim.X.Min;
            float xMax = (float)lim.X.Max;
            if (xMax > xMin)
            {
                float frac = ((float)app.placingFreqMHz - xMin) / (xMax - xMin);
                ImVec2 pp = ImPlot::GetPlotPos();
                ImVec2 ps = ImPlot::GetPlotSize();
                float px = pp.x + frac * ps.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(px, pp.y), ImVec2(px, pp.y + ps.y),
                            IM_COL32(255, 40, 40, 200), 1.5f);
            }
        }

        // --- Band plan: solid coloured bar along the bottom ---
        bool showBp = voiceView ? app.showBandPlanB : app.showBandPlan;
        const BandPlan& bp = voiceView ? app.bandPlanLoadedB : app.bandPlanLoaded;
        if (showBp && bp.valid && v.curN > 0)
        {
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            float bandTop = pp.y + ps.y - kBandH;
            float bandBot = bandTop + kBandH;
            float pxPerMHz = (float)(ps.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
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

void drawWaterfall(App& app, SpectrumView& v, const char* title)
{
    (void)app;
    ImGui::Begin(title);

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

    // Drag-to-place preview line: white vertical line through the waterfall
    // at the frequency the user is hovering, so they can centre on a signal.
    if (app.placingDecoder && app.placingVoiceView == (&v == &app.viewB) && v.curN > 0)
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

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)decs.size(),
                app.decoders.subbandCount() + (app.dualMode ? app.decodersB.subbandCount() : 0),
                app.decoders.workerCount() + (app.dualMode ? app.decodersB.workerCount() : 0));
    uint64_t drops = app.decoders.drops() + (app.dualMode ? app.decodersB.drops() : 0);
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu",
                           (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Remove all")))
    {
        app.decoders.removeAll();
        if (app.dualMode) app.decodersB.removeAll();
    }
    static std::string decsCopy;
    ImGui::SameLine();
    copyAllButton(decsCopy);

    if (ImGui::Checkbox(_L("CPU reduce"), &app.cpuReduce))
    {
        app.decoders.setCpuReduce(app.cpuReduce);
        app.decodersB.setCpuReduce(app.cpuReduce);
    }

    // Audio output device picker (used by VHF voice in later phases).
    if (app.audioDevs.empty())
        app.audioDevs = app.decoders.audioDevices();
    {
        std::vector<const char*> names;
        names.reserve(app.audioDevs.size());
        for (auto& s : app.audioDevs)
            names.push_back(s.c_str());
        if (app.audioDevice >= (int)names.size())
            app.audioDevice = 0;
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::Combo("Audio out", &app.audioDevice, names.data(), (int)names.size()))
        {
            app.decoders.setAudioDevice(app.audioDevice);
            app.decodersB.setAudioDevice(app.audioDevice);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##aud"))
        {
            app.audioDevs = app.decoders.audioDevices();
            app.decodersB.audioDevices();
        }
    }

    ImGui::Checkbox("Save decoders on restart", &app.saveDecoders);

    ImGui::Separator();

    if (ImGui::BeginTable("##decs", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Freq MHz");
        ImGui::TableSetupColumn("Baud");
        ImGui::TableSetupColumn("Eb/N0");
        ImGui::TableSetupColumn("Msgs");
        ImGui::TableHeadersRow();

        int toRemove = -1;
        bool toRemoveB = false;
        std::vector<std::string> copyRows;
        for (auto& d : decs)
        {
            int uid = d.channelId + (d.isB ? 100000 : 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char selid[24];
            std::snprintf(selid, sizeof(selid), "##sel%d", uid);
            ImVec4 c = d.locked ? Lc(app, ImVec4(0.2f, 1.0f, 0.3f, 1.0f))
                                : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, c);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x*1.3f, c.y*1.3f, c.z*1.3f, 1.0f));
            bool sel = (app.selectedDecoder == d.channelId);
            if (ImGui::Selectable(selid, sel, ImGuiSelectableFlags_None))
                app.selectedDecoder = d.channelId;
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextColored(c, "%s", d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", d.ebno);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.msgs);
            copyRows.push_back(copyFmt("%.4f\t%d\t%.1f\t%llu", d.freqMHz, d.baud, d.ebno,
                (unsigned long long)d.msgs));

            ImGui::SameLine();
            char btn[24];
            std::snprintf(btn, sizeof(btn), "X##%d", uid);
            if (ImGui::SmallButton(btn))
            {
                toRemove = d.channelId;
                toRemoveB = d.isB;
            }
        }
        handleTableCopy(copyRows);
        decsCopy = copyJoin(copyRows);
        ImGui::EndTable();
        if (toRemove >= 0)
        {
            if (toRemoveB) app.decodersB.removeDecoder(toRemove);
            else           app.decoders.removeDecoder(toRemove);
        }
    }

    ImGui::End();
}

void drawMessages(App& app)
{
    ImGui::Begin((std::string(_L("Messages")) + "###Messages").c_str());

    unsigned long long msgTotal = app.decoders.log().count();
    if (app.dualMode) msgTotal += app.decodersB.log().count();
    ImGui::Text("%llu total", msgTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.log().clear();
        if (app.dualMode) app.decodersB.log().clear();
    }
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
            for (auto& lbl : app.archiveDbLabels)
                items.push_back(lbl.c_str());
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Session", &app.archiveComboMsg, items.data(), (int)items.size()))
            {
                if (app.archiveComboMsg == 0)
                {
                    app.decoders.log().clearArchive();
                }
                else
                {
                    int idx = app.archiveComboMsg - 1;
                    if (idx < (int)app.archiveDbPaths.size())
                        app.writeDb.loadAcarsOrSu(app.archiveDbPaths[idx],
                                                  MessageStore::ACARS,
                                                  &app.decoders.log(), 0);
                }
            }
            if (app.decoders.log().hasArchive())
                ImGui::TextDisabled("  Viewing archived session");
        }
    }
    ImGui::Separator();

    auto msgs = app.decoders.log().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.log().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    // Filter on search text (case-insensitive substring across all fields).
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
            // Hide empty ACARS messages (no text and no decoded body) unless shown.
            if (!app.showEmptyMsgs && it->text.empty() && it->decoded.empty())
                continue;
            // Search filter — match any field (case-insensitive).
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
            // UTC time from epoch seconds
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
            if (!it->decoded.empty())
            {
                row += '\n';
                row += it->decoded;
            }
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

    auto acs = app.decoders.aircraftTable().snapshot();
    ImGui::Text("%zu tracked", acs.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
        app.decoders.aircraftTable().clear();
    static std::string acCopy;
    ImGui::SameLine();
    copyAllButton(acCopy);
    ImGui::SameLine();
    ImGui::Checkbox(_L("With position only"), &app.acPosOnly);
    ImGui::Separator();

    double now = (double)std::time(nullptr);
    // Newest activity first.
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

    auto acs = app.decoders.aircraftTable().snapshot();
    std::sort(acs.begin(), acs.end(),
              [](const AircraftEntry& a, const AircraftEntry& b) { return a.lastSeen > b.lastSeen; });
    const AircraftEntry* pick = nullptr;

    if (!pick) {
        for (auto& a : acs)
            if (!a.icao.empty()) { pick = &a; break; }
    }

    if (pick && !app.flightMapWv.isReady())
    {
        ImGui::Text("%s  %s  %06X",
                    pick->icao.c_str(),
                    pick->flight.empty() ? pick->reg.c_str() : pick->flight.c_str(),
                    pick->aesId);
        if (pick->hasPos)
            ImGui::SameLine(); ImGui::Text("  %.4f,%.4f  %d ft", pick->lat, pick->lon, pick->alt);
    }
    else if (!pick && !app.flightMapWv.isReady())
    {
        ImGui::TextDisabled("No aircraft with ICAO yet.");
    }

    if (!app.flightMapWv.isReady())
    {
        ImGui::TextDisabled("  Loading map...");
    }
    // Embed the map as an Edge WebView2 child window inside this panel.
    // Hide when another tab in the same dock is active.
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
// The Flight Map is an embedded Edge WebView2 browser, which only exists on
// Windows. On other platforms the panel is not registered or drawn at all.
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

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    int chan = app.selectedDecoder;
    bool valid = false;
    double freq = 0.0;
    for (auto& d : decs)
        if (d.channelId == chan)
        {
            valid = true;
            freq = d.freqMHz;
            break;
        }
    if (!valid && !decs.empty())
    {
        chan = decs.front().channelId;
        freq = decs.front().freqMHz;
    }

    if (decs.empty())
    {
        ImGui::TextDisabled("No decoders. Ctrl+click the spectrum to add one.");
        ImGui::End();
        return;
    }

    // Decoder selector (also selectable by clicking a row in Decoders panel).
    int preBaud = 0;
    bool preIsB = false;
    for (auto& d : decs)
    {
        if (d.channelId == chan)
        {
            preBaud = d.baud;
            preIsB = d.isB;
            break;
        }
    }
    char preview[128];
    std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  @%d%s",
                  chan, freq, preBaud, preIsB ? " [B]" : "");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Decoder", preview))
    {
        for (auto& d : decs)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  @%d%s",
                          d.channelId, d.freqMHz, d.baud,
                              d.isB ? " [B]" : "");
            if (ImGui::Selectable(label, d.channelId == chan))
            {
                app.selectedDecoder = d.channelId;
                chan = d.channelId;
                freq = d.freqMHz;
            }
        }
        ImGui::EndCombo();
    }

    int pairs = app.decoders.getConstellation(chan, app.constBuf, 1024);
    if (pairs == 0 && app.dualMode)
        pairs = app.decodersB.getConstellation(chan, app.constBuf, 1024);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d pts)", pairs);

    // Recompute the axis scale at most once per second so it holds steady
    // instead of jittering as the constellation data changes every frame.
    auto nowC = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(nowC - app.constLimTime).count() >= 1.0)
    {
        float m = 0.5f;
        for (float v : app.constBuf)
            m = std::max(m, std::fabs(v));
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

// ---------------------------------------------------------------------------
// Voice Calls browser
// ---------------------------------------------------------------------------

void drawVoiceCalls(App& app)
{
    ImGui::Begin((std::string(_L("Voice Calls")) + "###Voice Calls").c_str());

    auto calls = app.decoders.voiceCallLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.voiceCallLog().snapshot();
        calls.insert(calls.end(), b.begin(), b.end());
    }
    // Sort newest first
    std::sort(calls.begin(), calls.end(),
              [](const VoiceCallRecord& a, const VoiceCallRecord& b) { return a.timeSec > b.timeSec; });

    ImGui::Text("%llu calls", (unsigned long long)app.decoders.voiceCallLog().count() +
                               (app.dualMode ? app.decodersB.voiceCallLog().count() : 0));
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.voiceCallLog().clear();
        if (app.dualMode) app.decodersB.voiceCallLog().clear();
    }
    static std::string vcCopy;
    ImGui::SameLine();
    copyAllButton(vcCopy);
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan"))
    {
        app.decoders.voiceCallLog().scanDir(app.recordDir);
        if (app.dualMode) app.decodersB.voiceCallLog().scanDir(app.recordDir);
    }
    ImGui::SameLine();

    // Playback status
    if (app.audioPlayer.isPlaying())
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "  Playing... %.0fs", (double)app.audioPlayer.positionSec());
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
            // Time
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
            // Duration
            if (c.recording)
            {
                double nowSec = (double)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                double liveDur = (nowSec > c.timeSec) ? (nowSec - c.timeSec) : 0.0;
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Rec %.0fs", liveDur);
            }
            else if (c.durationSec > 0.0)
            {
                int m = (int)c.durationSec / 60;
                int s = (int)c.durationSec % 60;
                ImGui::Text("%d:%02d", m, s);
            }
            else
                ImGui::TextUnformatted("--");

            ImGui::TableNextColumn();
            // Play button — use row index for unique ImGui ID across A/B merge.
            bool sel = app.audioPlayer.isPlaying() && !c.filename.empty() &&
                       app.audioPlayer.currentPath().find(c.filename) != std::string::npos;
            char label[24];
            std::snprintf(label, sizeof(label), "%s##vcp%d", sel ? "||" : ">", rowIdx);
            if (ImGui::SmallButton(label))
            {
                if (sel)
                    app.audioPlayer.stop();
                else if (!c.filename.empty())
                {
                    std::string fullPath = std::string(app.recordDir) + "/" + c.filename;
                    app.audioPlayer.play(fullPath);
                }
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

// ---------------------------------------------------------------------------
// About dialog
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Persistent settings: serialized into airscope.ini alongside the ImGui dock
// layout via a custom settings handler.
// ---------------------------------------------------------------------------

void drawDockHost(App& app)
{
    // Default to NOT forcing a rebuild: if airscope.ini holds a saved layout,
    // the dock node already exists and we keep it. Only build the default layout
    // on first run (no node) or when explicitly forced (Reset Layout / dual /
    // a layout-version bump).
    static bool forceLayout = false;
    if (app.forceDefaultLayout) { forceLayout = true; app.forceDefaultLayout = false; }
    static bool lastDual = false;
    if (app.dualMode != lastDual) { forceLayout = true; lastDual = app.dualMode; }

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

        ImGuiID left, right, rtop, rrest, rmid, rbot, rcon, ctrl, dec;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.32f, &left, &right);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.62f, &ctrl, &dec);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.30f, &rtop, &rrest);
        ImGui::DockBuilderSplitNode(rrest, ImGuiDir_Up, 0.58f, &rmid, &rbot);
        ImGui::DockBuilderSplitNode(rbot, ImGuiDir_Right, 0.34f, &rcon, &rbot);

        ImGui::DockBuilderDockWindow((std::string(_L("Control")) + "###Control").c_str(), ctrl);
        ImGui::DockBuilderDockWindow((std::string(_L("Decoders")) + "###Decoders").c_str(), dec);

        ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
        ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
        // Always split for potential dual-mode: B windows are invisible when
        // not in dual mode, and the A windows fill the space.
        {
            ImGuiID rtopR, rmidR;
            ImGui::DockBuilderSplitNode(rtop, ImGuiDir_Right, 0.5f, &rtopR, &rtop);
            ImGui::DockBuilderSplitNode(rmid, ImGuiDir_Right, 0.5f, &rmidR, &rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum (B)")) + "###Spectrum (B)").c_str(), rtopR);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall (B)")) + "###Waterfall (B)").c_str(), rmidR);
        }
#if defined(_WIN32)
        ImGui::DockBuilderDockWindow((std::string(_L("Flight Map")) + "###Flight Map").c_str(), rmid);
#endif
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

