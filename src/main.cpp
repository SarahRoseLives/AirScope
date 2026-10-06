// AirScope - VHF airband ACARS / voice + 1090 ADSB receiver

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "core/app.h"
#include "core/main_funcs.h"
#include "i18n/i18n.h"
#include "version.h"
#include "gui/waterfall.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

bool openWavDialog(char* out, int outLen)
{
    char file[1024] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "WAV / IQ files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
    {
        std::strncpy(out, file, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
#else
// Native file picker via the desktop's dialog helper (zenity / kdialog / qarma).
bool openWavDialog(char* out, int outLen)
{
    const char* cmds[] = {
        "zenity --file-selection "
            "--file-filter='WAV / IQ files | *.wav *.WAV' "
            "--file-filter='All files | *' 2>/dev/null",
        "qarma --file-selection "
            "--file-filter='WAV / IQ files | *.wav *.WAV' "
            "--file-filter='All files | *' 2>/dev/null",
        "kdialog --getopenfilename . "
            "'WAV / IQ files (*.wav *.WAV)|All files (*)' 2>/dev/null",
    };
    for (const char* cmd : cmds)
    {
        FILE* p = popen(cmd, "r");
        if (!p)
            continue;
        char buf[1024] = "";
        char* got = std::fgets(buf, sizeof(buf), p);
        int rc = pclose(p);
        if (rc == 127 || !got)
            continue;
        size_t n = std::strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = 0;
        if (n == 0)
            return false;
        std::strncpy(out, buf, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main(int, char**)
{
#if defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "AirScope", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    App app;
    // Three concurrent receivers by default: RTL, Airspy, SDRplay.
    app.rx.push_back(std::make_unique<Receiver>());
    app.rx.push_back(std::make_unique<Receiver>());
    app.rx.push_back(std::make_unique<Receiver>());
    app.rx[0]->mode = kRxRtl;
    app.rx[0]->centerMHz = 131.550;
    app.rx[1]->mode = kRxAirspy;
    app.rx[1]->centerMHz = 130.000;
    app.rx[2]->mode = kRxSdrplay;
    app.rx[2]->centerMHz = 131.725;

    cfgRegisterHandler(app);
    io.IniFilename = "airscope.ini";
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

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

    i18nInit();
    i18nSet((Lang)app.languageIdx);

    if (app.fontSize < 8)  app.fontSize = 8;
    if (app.fontSize > 24) app.fontSize = 24;
    {
        const char* fontPaths[] = {
            "third_party/imgui/misc/fonts/Roboto-Medium.ttf",
            "../third_party/imgui/misc/fonts/Roboto-Medium.ttf",
        };
        bool loaded = false;
        for (auto& fp : fontPaths)
        {
            FILE* f = std::fopen(fp, "rb");
            if (f) { std::fclose(f); loaded = (io.Fonts->AddFontFromFileTTF(fp, (float)app.fontSize) != nullptr); break; }
        }
        if (!loaded)
            io.Fonts->AddFontDefault();
    }
    ImGui::GetStyle().ScaleAllSizes((float)app.fontSize / 13.0f);
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 0.0f;
        st.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    if (app.layoutVersion != kLayoutVersion)
    {
        app.forceDefaultLayout = true;
        app.layoutVersion = kLayoutVersion;
    }

    for (auto& rp : app.rx)
        buildWindow(rp->view, kFftSizes[app.fftSizeIdx], app.dbMin);

    app.audioDevs = app.rx.front()->decoders.audioDevices();
    for (size_t i = 0; i < app.rx.size(); ++i)
    {
        auto& rp = app.rx[i];
        if (!rp->src)
            rp->src = makeSdrSource(rp->mode);
        rp->decoders.setMessageStore(&app.writeDb);
        rp->decoders.setAudioDevice(app.audioDevice);
        rp->decoders.setAudioEnabled(i == 0); // one audio device only
        rp->decoders.voiceCallLog().scanDir(app.recordDir);
        rp->devices = rp->src ? rp->src->listDevices() : std::vector<SdrDeviceInfo>{};
    }

    app.writeDb.cleanup("databases", app.maxDbAgeDays);
    app.verCheck.start("airscope", AIRSCOPE_VERSION);
    scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
    for (auto& rp : app.rx)
        if (rp->bandPlanIdx >= 0 && rp->bandPlanIdx < (int)app.bandPlanPaths.size())
            rp->bandPlanLoaded = loadBandPlan(app.bandPlanPaths[rp->bandPlanIdx]);

    if (app.webServerEnabled)
    {
        app.webServer.receivers.clear();
        for (auto& rp : app.rx)
            app.webServer.receivers.push_back(rp.get());
        app.webServer.start(app.webServerPort);
    }
#if defined(_WIN32)
    app.flightMapWv.init(glfwGetWin32Window(window));
#endif

    const ImVec4 clear_color = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawDockHost(app);

        for (auto& rp : app.rx)
        {
            if (rp->running())
                processFft(rp->view, app, rp->src->centerFreq(), rp->src->sampleRate());
        }
        updateRateChange(app);

        if (app.logToDb != app.writeDb.enabled())
        {
            if (app.logToDb)
                app.writeDb.openSession("databases");
            else
                app.writeDb.setEnabled(false);
        }

        // Refresh saved decoder lists for persistent restart.
        if (app.saveDecoders)
        {
            for (auto& rp : app.rx)
            {
                if (!rp->running()) continue;
                rp->savedDecoders.clear();
                for (auto& st : rp->decoders.status())
                    rp->savedDecoders.push_back({st.freqMHz, st.baud});
            }
        }

        updateFeed(app);

        drawControls(app);
        for (size_t i = 0; i < app.rx.size(); ++i)
        {
            auto& rp = app.rx[i];
            if (!rp->showSpectrum) continue;
            drawSpectrum(app, *rp, (int)i, i == 1);
            drawWaterfall(app, *rp, (int)i);
        }
        drawDecoders(app);
        drawMessages(app);
        drawAircraft(app);
        drawVoiceCalls(app);
        drawFlightMap(app);
        drawConstellation(app);
        drawAbout(app);

        // Auto-mute live audio during playback, restore after.
        static bool wasPlaying = false;
        bool isPlaying = app.audioPlayer.isPlaying();
        if (isPlaying && !wasPlaying)
        {
            for (auto& rp : app.rx) rp->decoders.setVoiceMute(true);
        }
        else if (!isPlaying && wasPlaying)
        {
            for (auto& rp : app.rx) rp->decoders.setVoiceMute(app.voiceMuted);
        }
        wasPlaying = isPlaying;

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
    }

    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    stopAll(app);
    app.webServer.stop();
    app.writeDb.closeCurrent();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
