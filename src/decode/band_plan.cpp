#include "decode/band_plan.h"
#include "decode/json_mini.h"

#include <algorithm>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

static uint32_t parseColorHex(const char* s)
{
    uint32_t v = 0;
    for (int i = 0; i < 6 && s[i]; ++i)
    {
        char c = (char)std::tolower((unsigned char)s[i]);
        int n = (c >= '0' && c <= '9') ? (c - '0')
              : (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : -1;
        if (n < 0) return 0xFF888888;
        v = (v << 4) | (uint32_t)n;
    }
    uint32_t r = (v >> 16) & 0xFF;
    uint32_t g = (v >> 8) & 0xFF;
    uint32_t b = v & 0xFF;
    return (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

BandPlan loadBandPlan(const std::string& path)
{
    BandPlan bp;
    bp.filePath = path;

    // Read whole file
    std::ifstream f(path, std::ios::binary);
    if (!f) return bp;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    json::Value root = json::parse(content.c_str());
    if (root.tag != json::Value::OBJ) return bp;

    bp.name        = root["name"].s;
    bp.designator  = root["designator"].s;
    bp.position    = root["position"].d;

    const json::Value& bands = root["bands"];
    if (bands.tag != json::Value::ARR || bands.a.empty()) return bp;

    for (auto& bv : bands.a)
    {
        if (bv.tag != json::Value::OBJ) continue;
        BandPlanEntry e;
        e.loMHz = bv["lo"].d;
        e.hiMHz = bv["hi"].d;
        e.label = bv["label"].s;
        e.color = parseColorHex(bv["color"].s.c_str());
        if (e.loMHz < e.hiMHz && !e.label.empty())
            bp.entries.push_back(e);
    }
    if (!bp.name.empty() && !bp.entries.empty())
    {
        bp.valid = true;
        std::sort(bp.entries.begin(), bp.entries.end(),
                  [](const BandPlanEntry& a, const BandPlanEntry& b) { return a.loMHz < b.loMHz; });
    }
    return bp;
}

void scanBandPlans(const char* dir, std::vector<std::string>& names,
                   std::vector<std::string>& paths)
{
    names.clear(); paths.clear();
#if defined(_WIN32)
    std::string pattern = std::string(dir) + "\\*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string path = std::string(dir) + "\\" + fd.cFileName;
        BandPlan bp = loadBandPlan(path);
        if (bp.valid) {
            std::string label = bp.designator + "  -  " + bp.name;
            names.push_back(label);
            paths.push_back(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string fn(ent->d_name);
        if (fn.size() < 6 || fn.find(".json") != fn.size() - 5) continue;
        std::string path = std::string(dir) + "/" + fn;
        BandPlan bp = loadBandPlan(path);
        if (bp.valid) {
            std::string label = bp.designator + "  -  " + bp.name;
            names.push_back(label);
            paths.push_back(path);
        }
    }
    closedir(d);
#endif
}
