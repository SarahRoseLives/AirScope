// User-editable band plan file format.
// Each .bandplan file describes the frequency allocations for one satellite.
// The UI auto-discovers files placed in the bandplans/ folder next to the EXE.
//
// Format:
//   # Comments and blank lines are ignored.
//   header: Name, Designator, OrbitalPosition
//   lowMHz  highMHz  label  colorRRGGBB
//
// Example:
//   header: I4-F3 AOR-W, 4F3, -98.0
//   1537.650 1537.750 STD-C EGC            ff3333
//   1542.900 1543.050 Aero Voice (C-ch)    3388ff
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct BandPlanEntry
{
    double loMHz;
    double hiMHz;
    std::string label;
    uint32_t color; // RGBA8 (A=0xFF)
};

struct BandPlan
{
    std::string name;
    std::string designator;
    double position = 0.0;      // orbital degrees (negative = west)
    std::string filePath;
    std::vector<BandPlanEntry> entries;
    bool valid = false;
};

// Parse a .bandplan file.  Returns a BandPlan with valid==false on error.
BandPlan loadBandPlan(const std::string& path);

// Scan a directory for *.bandplan files and return their display names.
// Each entry is "designator  —  Name" for use in a combo box.
// The companion vector 'paths' receives the full file path for each entry.
void scanBandPlans(const char* dir, std::vector<std::string>& names,
                   std::vector<std::string>& paths);
