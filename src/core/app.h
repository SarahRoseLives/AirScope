// AirScope shared application state — App struct and global constants.
// Per-receiver state (source, spectrum view, decoders, tuning) lives in
// Receiver (receiver/receiver.h); App holds the receiver set and global UI,
// output, storage and audio settings.
#pragma once

#include "receiver/receiver.h"
#include "sdr/iq_recorder.h"
#include "audio/audio_player.h"
#include "web/web_server.h"
#include "store/message_store.h"
#include "decode/band_plan.h"
#include "output/message_feed.h"
#include "update/version_check.h"
#include "web/flight_map_webview.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct App
{
    // Concurrent receivers (e.g. Airspy voice + SDRplay ACARS + RTL ADS-B).
    std::vector<std::unique_ptr<Receiver>> rx;

    int  newBaud = 1;
    bool placingDecoder = false;
    int  placingRx = 0;
    double placingFreqMHz = 0.0;
    int  selectedDecoder = -1;
    int  selectedRx = 0;

    // Recording
    bool recordVoice = false;
    int  recordFormat = 0; // 0=WAV, 1=OGG
    char recordDir[256] = "recordings";
    bool saveDecoders = false;

    // Country blacklist — aircraft from these 2-letter country codes will not
    // be monitored.
    std::vector<std::string> blacklistCountries;

    // IQ recorder
    IqRecorder iqRecorder;
    char iqRecPath[512] = "iq_record.wav";
    float iqBufferSec = 10.0f;

    int  audioDevice = 0;
    bool voiceMuted = false;
    float voiceSquelchDb = -120.0f; // dBFS; very low = always open
    float voiceVolume = 1.5f;
    bool cpuReduce = false;
    bool showAbout = false;
    bool webServerEnabled = false;
    int  webServerPort = 8080;
    WebServer webServer;
    AudioPlayer audioPlayer;
    std::vector<std::string> audioDevs;

    // Output
    MessageFeed feed;
    VersionCheck verCheck;
    FlightMapWebView flightMapWv;
    bool   outFile = false;
    char   outFilePath[512] = "messages.jsonl";
    bool   outUdp = false;
    char   outUdpHost[128] = "127.0.0.1";
    int    outUdpPort = 5556;
    int    outFormat = 0;
    char   outStation[64] = "";
    bool   outSbs = false;
    char   outSbsHost[128] = "127.0.0.1";
    int    outSbsPort = 30003;

    int   fftSizeIdx = 2;
    float avgAlpha = 0.6f;
    float dbMin = -80.0f;
    float dbMax = 0.0f;
    bool  dcBlock = true;
    bool  autoScale = true;

    std::string status = "Idle";

    bool   bandBrowse = true;
    std::chrono::steady_clock::time_point lastRetune;
    double lastRetuneCtr = 0.0;
    float  browseEdgePct = 24.5f;
    float  browseThrottleMs = 20.0f;
    float  browseMinMovePct = 0.10f;
    bool   acPosOnly = false;
    bool   showEmptyMsgs = false;
    std::vector<std::string> bandPlanNames;  // display names (shared)
    std::vector<std::string> bandPlanPaths;  // full file paths (shared)
    char   bandPlanDir[256] = "bandplans";

    // Persistent message store (SQLite) — per-session, opt-in.
    MessageStore writeDb;
    bool   logToDb = false;
    int    maxDbAgeDays = 30;

    // Cached list of archive DB files for session dropdowns.
    std::vector<std::string> archiveDbPaths;
    std::vector<std::string> archiveDbLabels;
    double                  archiveDbLastScan = 0.0;
    int    archiveComboMsg = 0;

    // Shared search buffer for the Messages panel.
    char searchBuf[128] = {};

    int  layoutVersion = 0;
    bool forceDefaultLayout = false;
    int  fontSize = 17;
    bool lightMode = false;
    int  languageIdx = 0;
};

// ---- shared constants ----
constexpr double kRates[] = {
    0.25e6, 0.9e6, 1.024e6, 1.2e6, 1.4e6, 1.536e6,
    1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
constexpr const char* kRateLabels[] = {
    "0.25", "0.9", "1.024", "1.2", "1.4", "1.536",
    "1.8", "1.92", "2.048", "2.4", "2.56", "2.88", "3.2"};
constexpr int kNumRates = (int)(sizeof(kRates) / sizeof(kRates[0]));

// Airspy sample rates (MHz values as doubles, and index-to-label).
constexpr double kAirspyRates[] = {2.5e6, 3.0e6, 6.0e6, 10.0e6};
constexpr const char* kAirspyRateLabels[] = {"2.5", "3.0", "6.0", "10.0"};
constexpr int kAirspyNumRates = (int)(sizeof(kAirspyRates) / sizeof(kAirspyRates[0]));

constexpr int    kFftSizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr const char* kFftLabels[] = {"1024", "2048", "4096", "8192", "16384", "32768", "65536"};
constexpr int kNumFftSizes = (int)(sizeof(kFftSizes) / sizeof(kFftSizes[0]));

// Dock layout version: bump when the built-in default layout changes.
constexpr int kLayoutVersion = 14;
