// Shared function declarations.
#pragma once

#include "core/app.h"

// signal/processing.cpp
void buildWindow(SpectrumView&, int N, float initDb);
void updateFreqAxis(SpectrumView&, double fc, double fs, int N);
void patchDcBins(std::vector<float>& a, int N, int w);
void processFft(SpectrumView&, App&, double fc, double fs);
void updateRateChange(App&);

// voice/voice_ops.cpp
void retuneReceiver(Receiver&, double centerMHz, bool preserving);

// session/session.cpp
void updateFeed(App&);
void startAll(App&);
void stopAll(App&);

// gui/gui_panels.cpp
void drawDockHost(App&);
void drawControls(App&);
void drawSpectrum(App&, Receiver&, int idx, bool voiceView);
void drawWaterfall(App&, Receiver&, int idx);
void drawDecoders(App&);
void drawMessages(App&);
void drawAircraft(App&);
void drawFlightMap(App&);
void drawVoiceCalls(App&);
void drawAbout(App&);

// state/config.cpp
void cfgWriteAll(App&, struct ImGuiTextBuffer*);
void cfgReadLine(App&, const char*);
void cfgRegisterHandler(App&);

// main.cpp
bool openWavDialog(char* out, int outLen);
