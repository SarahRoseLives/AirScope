#include "store/message_store.h"

#include "decode/message_log.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <string>

// ---- open / close --------------------------------------------------------

bool MessageStore::openSession(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (db_)
        return true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char fn[64];
    std::strftime(fn, sizeof(fn), "messages_%Y%m%d_%H%M%S.db", &tm);
    std::string path = dir + "/" + fn;
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK || !db_)
    {
        std::fprintf(stderr, "[store] sqlite3_open(%s) failed: %s\n",
                     path.c_str(), sqlite3_errmsg(db_));
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA cache_size=-8000");
    ensureTable();
    enabled_.store(true);
    return true;
}

void MessageStore::closeCurrent()
{
    std::lock_guard<std::mutex> lk(mtx_);
    enabled_.store(false);
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void MessageStore::setEnabled(bool on)
{
    if (on == enabled_.load())
        return;
    if (!on)
    {
        closeCurrent();
    }
    // on → caller must call openSession(dir) separately
    enabled_.store(on);
}

// ---- cleanup -------------------------------------------------------------

void MessageStore::cleanup(const std::string& dir, int maxAgeDays)
{
    if (maxAgeDays <= 0)
        return;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        std::string name = entry.path().filename().string();
        if (name.size() < 24 || name.substr(0, 9) != "messages_" ||
            name.substr(name.size() - 3) != ".db")
            continue;
        // Parse date from filename: messages_YYYYMMDD_HHMMSS.db
        int yr = std::atoi(name.substr(9, 4).c_str());
        int mo = std::atoi(name.substr(13, 2).c_str());
        int dy = std::atoi(name.substr(15, 2).c_str());
        if (yr < 2024 || mo < 1 || mo > 12 || dy < 1 || dy > 31)
            continue;
        std::tm tm{};
        tm.tm_year = yr - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = dy;
        tm.tm_isdst = -1;
        time_t ft = std::mktime(&tm);
        if (ft == (time_t)-1)
            continue;
        time_t now = std::time(nullptr);
        time_t cutoff = now - (time_t)maxAgeDays * 86400;
        if (ft < cutoff)
        {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

// ---- scan archives -------------------------------------------------------

std::vector<DbFileInfo> MessageStore::scanArchives(const std::string& dir)
{
    std::vector<DbFileInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return out;

    std::vector<DbFileInfo> unsorted;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        std::string name = entry.path().filename().string();
        if (name.size() < 20 || name.substr(0, 9) != "messages_" ||
            name.substr(name.size() - 3) != ".db")
            continue;
        DbFileInfo info;
        info.filename = name;

        // Parse timestamp from filename: messages_YYYYMMDD_HHMMSS.db
        std::string ts = name.substr(9, 8);
        std::string hm = name.substr(18, 6);
        info.displayLabel = ts.substr(0, 4) + "-" + ts.substr(4, 2) + "-" +
                            ts.substr(6, 2) + " " + hm.substr(0, 2) + ":" +
                            hm.substr(2, 2);

        // Quick row count
        std::string fullPath = dir + "/" + name;
        sqlite3* rdb = nullptr;
        if (sqlite3_open_v2(fullPath.c_str(), &rdb,
                            SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && rdb)
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(rdb, "SELECT COUNT(*) FROM messages",
                                   -1, &stmt, nullptr) == SQLITE_OK)
            {
                if (sqlite3_step(stmt) == SQLITE_ROW)
                    info.rowCount = sqlite3_column_int64(stmt, 0);
            }
            if (stmt) sqlite3_finalize(stmt);
            sqlite3_close(rdb);
        }
        unsorted.push_back(info);
    }
    // Sort newest first (by displayLabel which is YYYY-MM-DD HH:MM).
    std::sort(unsorted.begin(), unsorted.end(),
              [](const DbFileInfo& a, const DbFileInfo& b) {
                  return a.displayLabel > b.displayLabel;
              });
    return unsorted;
}

// ---- load helpers --------------------------------------------------------

static std::string colText(sqlite3_stmt* stmt, int col)
{
    const char* s = (const char*)sqlite3_column_text(stmt, col);
    return s ? s : "";
}

static std::string field(const std::string& data, int idx)
{
    size_t pos = 0;
    for (int i = 0; i < idx; ++i)
    {
        pos = data.find('|', pos);
        if (pos == std::string::npos) return {};
        ++pos;
    }
    size_t end = data.find('|', pos);
    if (end == std::string::npos)
        return data.substr(pos);
    return data.substr(pos, end - pos);
}

bool MessageStore::loadAcarsOrSu(const std::string& dbPath, int type,
                                  MessageLog* log, int baud)
{
    if (!log) return false;
    sqlite3* rdb = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &rdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return false;
    if (!rdb) return false;

    const char* sql = "SELECT time,freq_mhz,channel,text,data,aes_id,icao"
                      " FROM messages WHERE type=?1 ORDER BY time ASC";
    sqlite3_stmt* stmt = nullptr;
    std::vector<DecodedMessage> items;
    if (sqlite3_prepare_v2(rdb, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, type);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            DecodedMessage m;
            m.timeSec = sqlite3_column_double(stmt, 0);
            m.freqMHz = sqlite3_column_double(stmt, 1);
            m.channelId = sqlite3_column_int(stmt, 2);
            m.text = colText(stmt, 3);
            std::string data = colText(stmt, 4);
            m.aesId = (uint32_t)sqlite3_column_int64(stmt, 5);
            m.icao = colText(stmt, 6);

            if (type == ACARS)
            {
                // data: hex|reg|flight|label|downlink|hasPos|lat|lon|alt|decoded
                m.hex = field(data, 0);
                m.reg = field(data, 1);
                m.flight = field(data, 2);
                m.label = field(data, 3);
                m.downlink = (field(data, 4) == "1") ? 1 : 0;
                m.hasPos = (field(data, 5) == "1");
                if (m.hasPos)
                {
                    m.lat = std::atof(field(data, 6).c_str());
                    m.lon = std::atof(field(data, 7).c_str());
                    m.alt = std::atoi(field(data, 8).c_str());
                }
                m.decoded = field(data, 9);
            }
            else // SU
            {
                // data: hex|0xNN
                m.hex = field(data, 0);
                std::string st = field(data, 1);
                if (st.size() > 2 && st[0] == '0' && (st[1] == 'x' || st[1] == 'X'))
                    m.suType = (uint8_t)std::strtoul(st.c_str() + 2, nullptr, 16);
            }
            items.push_back(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(rdb);

    log->setArchive(std::move(items));
    return true;
}

bool MessageStore::loadEgc(const std::string& dbPath, EgcLog* log)
{
    if (!log) return false;
    sqlite3* rdb = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &rdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return false;
    if (!rdb) return false;

    const char* sql = "SELECT time,freq_mhz,channel,text,data"
                      " FROM messages WHERE type=?1 ORDER BY time ASC";
    sqlite3_stmt* stmt = nullptr;
    std::vector<EgcMessage> items;
    if (sqlite3_prepare_v2(rdb, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, EGC);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            EgcMessage m;
            m.freqMHz = sqlite3_column_double(stmt, 1);
            m.channelId = sqlite3_column_int(stmt, 2);
            m.text = colText(stmt, 3);
            std::string data = colText(stmt, 4);
            // data: timeUtc|priority|msgId|service|presentation
            m.timeUtc = field(data, 0);
            m.priority = field(data, 1);
            m.messageId = std::atoi(field(data, 2).c_str());
            m.service = field(data, 3);
            m.presentation = std::atoi(field(data, 4).c_str());
            items.push_back(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(rdb);

    log->setArchive(std::move(items));
    return true;
}

bool MessageStore::loadLes(const std::string& dbPath, LesLog* log)
{
    if (!log) return false;
    sqlite3* rdb = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &rdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return false;
    if (!rdb) return false;

    const char* sql = "SELECT time,freq_mhz,channel,text,data"
                      " FROM messages WHERE type=?1 ORDER BY time ASC";
    sqlite3_stmt* stmt = nullptr;
    std::vector<LesMessage> items;
    if (sqlite3_prepare_v2(rdb, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, LES);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            LesMessage m;
            m.freqMHz = sqlite3_column_double(stmt, 1);
            m.channelId = sqlite3_column_int(stmt, 2);
            m.text = colText(stmt, 3);
            std::string data = colText(stmt, 4);
            // data: timeUtc|satName|lesId|lesLabel|ch|pktNo|encrypted
            m.timeUtc = field(data, 0);
            m.satName = field(data, 1);
            m.lesId = std::atoi(field(data, 2).c_str());
            m.lesLabel = field(data, 3);
            m.channel = std::atoi(field(data, 4).c_str());
            m.pktNo = std::atoi(field(data, 5).c_str());
            m.isEncrypted = (field(data, 6) == "1");
            items.push_back(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(rdb);

    log->setArchive(std::move(items));
    return true;
}

// ---- internal helpers ----------------------------------------------------

void MessageStore::exec(const char* sql)
{
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::fprintf(stderr, "[store] sql error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
    }
}

void MessageStore::ensureTable()
{
    exec(R"(
        CREATE TABLE IF NOT EXISTS messages (
            id       INTEGER PRIMARY KEY AUTOINCREMENT,
            time     REAL    NOT NULL,
            type     INTEGER NOT NULL,
            freq_mhz REAL,
            channel  INTEGER,
            baud     INTEGER,
            text     TEXT,
            data     TEXT,
            aes_id   INTEGER,
            icao     TEXT,
            created  REAL    DEFAULT (strftime('%s','now'))
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS ix_msg_time ON messages(time)");
    exec("CREATE INDEX IF NOT EXISTS ix_msg_type ON messages(type)");
    exec("CREATE INDEX IF NOT EXISTS ix_msg_aes  ON messages(aes_id)");
    exec("CREATE INDEX IF NOT EXISTS ix_msg_icao ON messages(icao)");
}

// ---- store methods (gated by enabled_ + db_) ----------------------------

void MessageStore::storeAcars(double timeSec, int channelId, double freqMHz,
                              const std::string& text, const std::string& hex,
                              uint32_t aesId, const std::string& icao,
                              const std::string& reg, const std::string& flight,
                              const std::string& label, bool hasPos,
                              double lat, double lon, int alt,
                              const std::string& decoded, bool downlink, int baud)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    char extra[512];
    std::snprintf(extra, sizeof(extra), "%s|%s|%s|%s|%d|%d|%.5f|%.5f|%d|%s",
                  hex.c_str(), reg.c_str(), flight.c_str(), label.c_str(),
                  downlink ? 1 : 0, hasPos ? 1 : 0, lat, lon, alt, decoded.c_str());
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (time,type,freq_mhz,channel,baud,text,data,aes_id,icao)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, timeSec);
        sqlite3_bind_int(stmt, 2, ACARS);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int(stmt, 4, channelId);
        sqlite3_bind_int(stmt, 5, baud);
        sqlite3_bind_text(stmt, 6, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, extra, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 8, aesId);
        sqlite3_bind_text(stmt, 9, icao.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

void MessageStore::storeSu(double timeSec, int channelId, double freqMHz,
                           const std::string& text, const std::string& hex,
                           uint32_t aesId, uint8_t suType, int baud)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    char extra[64];
    std::snprintf(extra, sizeof(extra), "%s|0x%02X", hex.c_str(), suType);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (time,type,freq_mhz,channel,baud,text,data,aes_id)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, timeSec);
        sqlite3_bind_int(stmt, 2, SU);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int(stmt, 4, channelId);
        sqlite3_bind_int(stmt, 5, baud);
        sqlite3_bind_text(stmt, 6, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, extra, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 8, aesId);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

void MessageStore::storeEgc(double timeSec, int channelId, double freqMHz,
                            const std::string& text, const std::string& timeUtc,
                            const std::string& priority, int msgId,
                            const std::string& service, int presentation)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    char extra[384];
    std::snprintf(extra, sizeof(extra), "%s|%s|%d|%s|%d",
                  timeUtc.c_str(), priority.c_str(), msgId, service.c_str(), presentation);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (time,type,freq_mhz,channel,text,data)"
        " VALUES (?1,?2,?3,?4,?5,?6)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, timeSec);
        sqlite3_bind_int(stmt, 2, EGC);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int(stmt, 4, channelId);
        sqlite3_bind_text(stmt, 5, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, extra, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

void MessageStore::storeLes(double timeSec, int channelId, double freqMHz,
                            const std::string& text, const std::string& timeUtc,
                            const std::string& satName, int lesId,
                            const std::string& lesLabel, int ch, int pktNo,
                            bool encrypted)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    char extra[384];
    std::snprintf(extra, sizeof(extra), "%s|%s|%d|%s|%d|%d|%d",
                  timeUtc.c_str(), satName.c_str(), lesId, lesLabel.c_str(),
                  ch, pktNo, encrypted ? 1 : 0);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (time,type,freq_mhz,channel,text,data)"
        " VALUES (?1,?2,?3,?4,?5,?6)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, timeSec);
        sqlite3_bind_int(stmt, 2, LES);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int(stmt, 4, channelId);
        sqlite3_bind_text(stmt, 5, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, extra, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

void MessageStore::storeVoice(double timeSec, double freqMHz, uint32_t aesId,
                              const std::string& icao, double durationSec,
                              const std::string& filename)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (time,type,freq_mhz,aes_id,icao,data)"
        " VALUES (?1,?2,?3,?4,?5,?6)";
    char extra[128];
    std::snprintf(extra, sizeof(extra), "%.1f|%s", durationSec, filename.c_str());
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, timeSec);
        sqlite3_bind_int(stmt, 2, Voice);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int64(stmt, 4, aesId);
        sqlite3_bind_text(stmt, 5, icao.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, extra, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

void MessageStore::storeMes(uint32_t mesId, const std::string& action,
                            const std::string& sat, int les, int channel,
                            double freqMHz, double nowSec)
{
    if (!enabled_.load()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO messages (time,type,freq_mhz,channel,aes_id,data)"
        " VALUES (?1,?2,?3,?4,?5,?6)";
    char extra[256];
    std::snprintf(extra, sizeof(extra), "MES|%u|%s|%s|%d",
                  mesId, action.c_str(), sat.c_str(), les);
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double(stmt, 1, nowSec);
        sqlite3_bind_int(stmt, 2, MES);
        sqlite3_bind_double(stmt, 3, freqMHz);
        sqlite3_bind_int(stmt, 4, channel);
        sqlite3_bind_int64(stmt, 5, mesId);
        sqlite3_bind_text(stmt, 6, extra, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}
