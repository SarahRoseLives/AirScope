// SQLite-backed message archive — per-session databases, opt-in via checkbox.
// Load methods open a read-only connection to any archive file.
// All methods are safe to call from any thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

struct DbFileInfo
{
    std::string filename;    // e.g. "messages_20250628_120500.db"
    std::string displayLabel; // e.g. "2025-06-28 12:05"
    int64_t rowCount = 0;
};

class MessageStore
{
public:
    ~MessageStore() { closeCurrent(); }

    bool openSession(const std::string& dir);
    void closeCurrent();
    void setEnabled(bool on);
    bool enabled() const { return enabled_.load(); }

    // Delete archive files older than maxAgeDays.
    void cleanup(const std::string& dir, int maxAgeDays);

    // List available archive files in a directory.
    std::vector<DbFileInfo> scanArchives(const std::string& dir);

    // Load messages from a SQLite file into the given vectors.
    // Returns true on success.
    // Type 1=ACARS, 2=SU — loaded as DecodedMessage
    bool loadAcarsOrSu(const std::string& dbPath, int type,
                       class MessageLog* log, int baud);
    // Type 3=EGC
    bool loadEgc(const std::string& dbPath, class EgcLog* log);
    // Type 4=LES
    bool loadLes(const std::string& dbPath, class LesLog* log);

    // Message type enum — discriminates the single messages table.
    enum Type : int { ACARS = 1, SU = 2, EGC = 3, LES = 4, Voice = 5, MES = 6 };

    // Store methods — no-ops when !enabled_ or !db_.
    void storeAcars(double timeSec, int channelId, double freqMHz, const std::string& text,
                    const std::string& hex, uint32_t aesId, const std::string& icao,
                    const std::string& reg, const std::string& flight, const std::string& label,
                    bool hasPos, double lat, double lon, int alt, const std::string& decoded,
                    bool downlink, int baud);

    void storeSu(double timeSec, int channelId, double freqMHz, const std::string& text,
                 const std::string& hex, uint32_t aesId, uint8_t suType, int baud);

    void storeEgc(double timeSec, int channelId, double freqMHz, const std::string& text,
                  const std::string& timeUtc, const std::string& priority, int msgId,
                  const std::string& service, int presentation);

    void storeLes(double timeSec, int channelId, double freqMHz, const std::string& text,
                  const std::string& timeUtc, const std::string& satName, int lesId,
                  const std::string& lesLabel, int ch, int pktNo, bool encrypted);

    void storeVoice(double timeSec, double freqMHz, uint32_t aesId, const std::string& icao,
                    double durationSec, const std::string& filename);

    void storeMes(uint32_t mesId, const std::string& action, const std::string& sat,
                  int les, int channel, double freqMHz, double nowSec);

private:
    void exec(const char* sql);
    void ensureTable();

    sqlite3* db_ = nullptr;
    std::mutex mtx_;
    std::atomic<bool> enabled_{false};
};
