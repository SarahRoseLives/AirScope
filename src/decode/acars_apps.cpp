#include "decode/acars_apps.h"
#include "decode/libacars_lock.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/adsc.h>
#include <libacars/list.h>
#include <libacars/vstring.h>
}

namespace {

// libacars initializes its configuration lazily and uses some lazily-built
// static dictionaries during parsing, so serialize all decode calls. ACARS
// messages are infrequent, so a single mutex is plenty.
std::mutex g_decodeMtx;

// Basic ADS-C report groups carrying lat/lon/alt (downlink tags 7,9,10,18,19,20).
bool isBasicReportTag(uint8_t t)
{
    return t == 7 || t == 9 || t == 10 || t == 18 || t == 19 || t == 20;
}

// Parse one coordinate starting at `i` (which must point at N/S/E/W).
// Accepts the common ACARS decimal-minute encodings, e.g. ddmm.m (5 digits),
// dddmm.m (6 digits) and the seconds variants. Advances `i` past the token.
bool parseCoord(const std::string& s, size_t& i, double& val)
{
    char c = (char)std::toupper((unsigned char)s[i]);
    bool isLat = (c == 'N' || c == 'S');
    if (!isLat && c != 'E' && c != 'W')
        return false;
    int degDigits = isLat ? 2 : 3;
    // Coordinate is ddmm.m / dddmm.m (deg + min + tenths). Cap the digit run
    // at this length: in concatenated reports the following numeric fields
    // (altitude/speed) run straight on with no separator.
    int maxDigits = degDigits + 3;
    size_t j = i + 1;
    int nd = 0;
    while (nd < maxDigits && j < s.size() && std::isdigit((unsigned char)s[j]))
    {
        ++j;
        ++nd;
    }
    if (nd < degDigits + 2)
        return false;

    std::string d = s.substr(i + 1, (size_t)nd);
    double deg = std::atof(d.substr(0, (size_t)degDigits).c_str());
    double min = std::atof(d.substr((size_t)degDigits, 2).c_str());
    std::string rest = d.substr((size_t)degDigits + 2);
    if (rest.size() == 1)
        min += std::atof(("0." + rest).c_str());
    else if (rest.size() == 2)
        min += std::atof(rest.c_str()) / 60.0; // seconds
    if (deg > 90.0 && isLat)
        return false;
    val = deg + min / 60.0;
    if (c == 'S' || c == 'W')
        val = -val;
    i = j - 1;
    return true;
}

} // namespace

extern "C" void airscope_libacars_lock(void) { g_decodeMtx.lock(); }
extern "C" void airscope_libacars_unlock(void) { g_decodeMtx.unlock(); }

bool parseAcarsPosition(const std::string& text, double& lat, double& lon)
{
    bool gotLat = false, gotLon = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = (char)std::toupper((unsigned char)text[i]);
        // Ignore N/S/E/W that are part of a word (e.g. the "S" in "POS").
        if (i > 0 && std::isalpha((unsigned char)text[i - 1]))
            continue;
        if ((c == 'N' || c == 'S') && !gotLat)
        {
            double v;
            size_t k = i;
            if (parseCoord(text, k, v) && v >= -90.0 && v <= 90.0)
            {
                lat = v;
                gotLat = true;
                i = k;
            }
        }
        else if ((c == 'E' || c == 'W') && !gotLon)
        {
            double v;
            size_t k = i;
            if (parseCoord(text, k, v) && v >= -180.0 && v <= 180.0)
            {
                lon = v;
                gotLon = true;
                i = k;
            }
        }
        if (gotLat && gotLon)
            break;
    }
    if (!gotLat || !gotLon)
        return false;
    if (lat == 0.0 && lon == 0.0)
        return false;
    return true;
}

AcarsAppResult decodeAcarsApps(const std::string& label, const std::string& text,
                               bool downlink)
{
    AcarsAppResult r;
    if (label.empty() || text.empty())
        return r;

    std::lock_guard<std::mutex> lk(g_decodeMtx);

    la_msg_dir dir = downlink ? LA_MSG_DIR_AIR2GND : LA_MSG_DIR_GND2AIR;

    // Some labels (e.g. H1) prefix the application data with a sublabel/MFI;
    // skip past it so the right decoder sees clean payload.
    int offset = la_acars_extract_sublabel_and_mfi(label.c_str(), dir, text.c_str(),
                                                   (int)text.size(), nullptr, nullptr);
    if (offset < 0 || offset > (int)text.size())
        offset = 0;

    la_proto_node* node = la_acars_decode_apps(label.c_str(), text.c_str() + offset, dir);
    if (!node)
        return r;

    r.decoded = true;

    la_vstring* vs = la_proto_tree_format_text(nullptr, node);
    if (vs)
    {
        if (vs->str)
            r.text = vs->str;
        la_vstring_destroy(vs, true);
    }

    // Walk the ADS-C tags for position (basic report), flight id, airframe ICAO.
    // Only downlink (air->ground) messages carry these: the SAME tag numbers
    // (7/9/10/18/19/20) mean position reports on downlink but contract-request
    // structures on uplink, so casting an uplink tag to a basic report yields
    // garbage lat/lon/alt. Guard on direction and sanity-check the values.
    la_proto_node* adscNode = la_proto_tree_find_adsc(node);
    if (downlink && adscNode && adscNode->data)
    {
        la_adsc_msg_t* msg = static_cast<la_adsc_msg_t*>(adscNode->data);
        if (!msg->err)
        {
            for (la_list* l = msg->tag_list; l != nullptr; l = l->next)
            {
                la_adsc_tag_t* tag = static_cast<la_adsc_tag_t*>(l->data);
                if (!tag || !tag->data)
                    continue;
                if (!r.hasPos && isBasicReportTag(tag->tag))
                {
                    auto* br = static_cast<la_adsc_basic_report_t*>(tag->data);
                    if (br->lat >= -90.0 && br->lat <= 90.0 && br->lon >= -180.0 &&
                        br->lon <= 180.0 && br->alt > -1500 && br->alt < 70000)
                    {
                        r.hasPos = true;
                        r.lat = br->lat;
                        r.lon = br->lon;
                        r.alt = br->alt;
                    }
                }
                else if (tag->tag == 12) // flight id group
                {
                    auto* fid = static_cast<la_adsc_flight_id_t*>(tag->data);
                    r.flightId.assign(fid->id, strnlen(fid->id, sizeof(fid->id)));
                    // trim trailing spaces
                    while (!r.flightId.empty() && r.flightId.back() == ' ')
                        r.flightId.pop_back();
                }
                else if (tag->tag == 17) // airframe id group (ICAO 24-bit)
                {
                    auto* af = static_cast<la_adsc_airframe_id_t*>(tag->data);
                    char hx[8];
                    std::snprintf(hx, sizeof(hx), "%02X%02X%02X", af->icao_hex[0],
                                  af->icao_hex[1], af->icao_hex[2]);
                    r.icaoHex = hx;
                }
            }
        }
    }

    la_proto_tree_destroy(node);
    return r;
}
