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
    // Type 1=ACARS — loaded as DecodedMessage
    bool loadAcarsOrSu(const std::string& dbPath, int type,
                       class MessageLog* log, int baud);

    // Message type enum — discriminates the single messages table.
    enum Type : int { ACARS = 1, Voice = 5 };

    // Store methods — no-ops when !enabled_ or !db_.
    void storeAcars(double timeSec, int channelId, double freqMHz, const std::string& text,
                    const std::string& hex, uint32_t aesId, const std::string& icao,
                    const std::string& reg, const std::string& flight, const std::string& label,
                    bool hasPos, double lat, double lon, int alt, const std::string& decoded,
                    bool downlink, int baud);

    void storeVoice(double timeSec, double freqMHz, uint32_t aesId, const std::string& icao,
                    double durationSec, const std::string& filename);

private:
    void exec(const char* sql);
    void ensureTable();

    sqlite3* db_ = nullptr;
    std::mutex mtx_;
    std::atomic<bool> enabled_{false};
};
