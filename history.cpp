#include "history.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace cw {
namespace {

struct sqlite3;
struct sqlite3_stmt;
using Destructor = void (*)(void*);
using RowCallback = int (*)(void*, int, char**, char**);

constexpr int SqlOk = 0;
constexpr int SqlRow = 100;
constexpr int SqlDone = 101;
constexpr int SqlNull = 5;
constexpr int SqlReadWrite = 0x02;
constexpr int SqlCreate = 0x04;
constexpr int DefaultRetentionDays = 7;
constexpr int MaxRetentionDays = 3650;

struct Sqlite {
    HMODULE library = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    int (*open)(const char*, sqlite3**, int, const char*){};
    int (*close)(sqlite3*){};
    int (*busyTimeout)(sqlite3*, int){};
    int (*exec)(sqlite3*, const char*, RowCallback, void*, char**){};
    int (*prepare)(sqlite3*, const char*, int, sqlite3_stmt**, const char**){};
    int (*finalize)(sqlite3_stmt*){};
    int (*step)(sqlite3_stmt*){};
    int (*reset)(sqlite3_stmt*){};
    int (*bindInteger)(sqlite3_stmt*, int, std::int64_t){};
    int (*bindNumber)(sqlite3_stmt*, int, double){};
    int (*bindNull)(sqlite3_stmt*, int){};
    int (*bindText)(sqlite3_stmt*, int, const void*, int, Destructor){};
    int (*columnType)(sqlite3_stmt*, int){};
    std::int64_t (*columnInteger)(sqlite3_stmt*, int){};
    double (*columnNumber)(sqlite3_stmt*, int){};
    const void* (*columnText)(sqlite3_stmt*, int){};
    int (*columnTextBytes)(sqlite3_stmt*, int){};
    int (*changes)(sqlite3*){};

    template<class Function>
    bool resolve(Function& destination, const char* name) {
        const FARPROC procedure = GetProcAddress(library, name);
        static_assert(sizeof(destination) == sizeof(procedure));
        std::memcpy(&destination, &procedure, sizeof(destination));
        return destination != nullptr;
    }

    Sqlite() {
        const bool loaded = library && resolve(open, "sqlite3_open_v2") &&
            resolve(close, "sqlite3_close") && resolve(busyTimeout, "sqlite3_busy_timeout") &&
            resolve(exec, "sqlite3_exec") && resolve(prepare, "sqlite3_prepare_v2") &&
            resolve(finalize, "sqlite3_finalize") && resolve(step, "sqlite3_step") &&
            resolve(reset, "sqlite3_reset") && resolve(bindInteger, "sqlite3_bind_int64") &&
            resolve(bindNumber, "sqlite3_bind_double") && resolve(bindNull, "sqlite3_bind_null") &&
            resolve(bindText, "sqlite3_bind_text16") && resolve(columnType, "sqlite3_column_type") &&
            resolve(columnInteger, "sqlite3_column_int64") && resolve(columnNumber, "sqlite3_column_double") &&
            resolve(columnText, "sqlite3_column_text16") && resolve(columnTextBytes, "sqlite3_column_bytes16") &&
            resolve(changes, "sqlite3_changes");
        if (!loaded && library) {
            FreeLibrary(library);
            library = nullptr;
        }
    }

    ~Sqlite() {
        if (library) FreeLibrary(library);
    }
};

const Sqlite& sqlite() {
    static const Sqlite api;
    return api;
}

struct Database {
    const Sqlite& api;
    sqlite3* handle{};

    ~Database() {
        if (handle) api.close(handle);
    }

    bool exec(const char* sql) const {
        return api.exec(handle, sql, nullptr, nullptr, nullptr) == SqlOk;
    }
};

class Statement {
public:
    Statement(const Database& db, const char* sql) : api_(db.api) {
        api_.prepare(db.handle, sql, -1, &handle_, nullptr);
    }

    ~Statement() {
        if (handle_) api_.finalize(handle_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    explicit operator bool() const { return handle_ != nullptr; }
    int step() const { return api_.step(handle_); }
    void reset() const { api_.reset(handle_); }
    std::int64_t integer(int column) const { return api_.columnInteger(handle_, column); }
    double number(int column) const {
        return isNull(column) ? Missing : api_.columnNumber(handle_, column);
    }
    bool isNull(int column) const { return api_.columnType(handle_, column) == SqlNull; }

    bool bindInteger(int index, std::int64_t value) const {
        return api_.bindInteger(handle_, index, value) == SqlOk;
    }

    bool bindNumber(int index, double value) const {
        return (std::isfinite(value) ? api_.bindNumber(handle_, index, value)
                                     : api_.bindNull(handle_, index)) == SqlOk;
    }

    bool text(int index, const std::wstring& value) const { return api_.bindText(handle_, index, value.data(), static_cast<int>(value.size()*sizeof(wchar_t)), reinterpret_cast<Destructor>(-1))==SqlOk; }
    std::wstring text(int column) const { auto* p=static_cast<const wchar_t*>(api_.columnText(handle_,column)); return p?std::wstring(p):L""; }
    bool bindName(int index, const ProcessEntry& entry) const {
        const auto length = std::find(std::begin(entry.name), std::end(entry.name), L'\0') - entry.name;
        const auto transient = reinterpret_cast<Destructor>(-1);
        return api_.bindText(handle_, index, entry.name, static_cast<int>(length * sizeof(wchar_t)), transient) == SqlOk;
    }

    void readName(int column, ProcessEntry& entry) const {
        const auto* text = static_cast<const wchar_t*>(api_.columnText(handle_, column));
        if (!text) return;
        const auto length = static_cast<std::size_t>(api_.columnTextBytes(handle_, column)) / sizeof(wchar_t);
        std::copy_n(text, std::min(length, std::size(entry.name) - 1), entry.name);
    }

private:
    const Sqlite& api_;
    sqlite3_stmt* handle_{};
};

constexpr std::array<const char*, MetricCount> MetricColumns = {
    "cpu_percent", "memory_percent", "memory_used", "memory_total", "disk_used", "disk_total",
    "disk_read", "disk_write", "gpu_percent", "gpu_memory_used", "gpu_memory_total",
    "network_down", "network_up", "cpu_temp", "cpu_watts", "gpu_temp", "gpu_watts"
};

std::string sampleColumns(std::string_view qualifier = {}) {
    std::string columns(qualifier);
    columns += "timestamp_ms";
    for (const char* column : MetricColumns) {
        columns += ',';
        columns += qualifier;
        columns += column;
    }
    return columns;
}

bool createSchema(const Database& db) {
    std::string schema = "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS samples(timestamp_ms INTEGER PRIMARY KEY";
    for (const char* column : MetricColumns) {
        schema += ',';
        schema += column;
        schema += " REAL";
    }
    schema += R"sql();
        CREATE TABLE IF NOT EXISTS rankings(
            timestamp_ms INTEGER NOT NULL REFERENCES samples(timestamp_ms) ON DELETE CASCADE,
            sort_kind INTEGER NOT NULL, place INTEGER NOT NULL,
            pid INTEGER NOT NULL, name TEXT NOT NULL, cpu REAL, memory INTEGER NOT NULL,
            PRIMARY KEY(timestamp_ms, sort_kind, place)
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS traffic_cursor(session INTEGER, path TEXT, public INTEGER, lan INTEGER, other INTEGER, updated INTEGER, PRIMARY KEY(session,path));
        CREATE TABLE IF NOT EXISTS traffic_bytes(time INTEGER, path TEXT, public INTEGER, lan INTEGER, other INTEGER);
        CREATE INDEX IF NOT EXISTS traffic_time ON traffic_bytes(time);
        CREATE TABLE IF NOT EXISTS settings(
            id INTEGER PRIMARY KEY CHECK(id = 1),
            retention_days INTEGER NOT NULL CHECK(retention_days BETWEEN 1 AND 3650)
        );
        INSERT OR IGNORE INTO settings(id, retention_days) VALUES(1, 7);
        COMMIT;
    )sql";
    return db.exec(schema.c_str());
}

Snapshot readSnapshot(const Statement& statement) {
    Snapshot snapshot;
    snapshot.timestampMs = statement.integer(0);
    for (std::size_t i = 0; i < MetricCount; ++i) {
        snapshot.values[i] = statement.number(static_cast<int>(i + 1));
    }
    return snapshot;
}

bool saveSample(const Statement& statement, const Snapshot& snapshot) {
    bool bound = statement.bindInteger(1, snapshot.timestampMs);
    for (std::size_t i = 0; bound && i < MetricCount; ++i) {
        bound = statement.bindNumber(static_cast<int>(i + 2), snapshot.values[i]);
    }
    const bool saved = bound && statement.step() == SqlDone;
    statement.reset();
    return saved;
}

bool saveRanking(const Statement& statement, std::int64_t timestampMs, int sortKind,
                 const std::array<ProcessEntry, TopCount>& ranking) {
    for (std::size_t i = 0; i < ranking.size(); ++i) {
        const auto& entry = ranking[i];
        if (!entry.pid) continue;
        const auto memory = std::min<std::uint64_t>(entry.memory, std::numeric_limits<std::int64_t>::max());
        const bool saved = statement.bindInteger(1, timestampMs) && statement.bindInteger(2, sortKind) &&
            statement.bindInteger(3, static_cast<std::int64_t>(i)) && statement.bindInteger(4, entry.pid) &&
            statement.bindName(5, entry) && statement.bindNumber(6, entry.cpu) &&
            statement.bindInteger(7, static_cast<std::int64_t>(memory)) && statement.step() == SqlDone;
        statement.reset();
        if (!saved) return false;
    }
    return true;
}

} // namespace

struct History::Impl {
    Database db{sqlite()};
    std::optional<Statement> insertSample;
    std::optional<Statement> insertRank;
    bool initialized{};

    Impl(const std::filesystem::path& path, bool writer) {
        if (!db.api.library || path.empty()) return;
        const auto utf8 = path.u8string();
        const int flags = SqlReadWrite | (writer ? SqlCreate : 0);
        if (db.api.open(reinterpret_cast<const char*>(utf8.c_str()), &db.handle, flags, nullptr) != SqlOk) return;
        if (db.api.busyTimeout(db.handle, writer ? 1000 : 150) != SqlOk) return;
        if (!db.exec("PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON;")) return;
        if (writer) {
            if (!db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") || !createSchema(db)) return;
            std::string insert = "INSERT OR REPLACE INTO samples(" + sampleColumns() + ") VALUES(?";
            for (std::size_t i = 0; i < MetricCount; ++i) insert += ",?";
            insert += ')';
            insertSample.emplace(db, insert.c_str());
            insertRank.emplace(db, "INSERT INTO rankings VALUES(?,?,?,?,?,?,?)");
            if (!*insertSample || !*insertRank) return;
        }
        const Statement setting(db, "SELECT retention_days FROM settings WHERE id=1");
        initialized = setting && setting.step() == SqlRow;
    }
};

History::History(const std::filesystem::path& path, bool writer)
    : impl_(std::make_unique<Impl>(path, writer)) {}
History::~History() = default;
History::History(History&&) noexcept = default;
History& History::operator=(History&&) noexcept = default;

bool History::ready() const { return impl_ && impl_->initialized; }

bool History::save(const Snapshot& snapshot) {
    if (!ready() || !impl_->insertSample || snapshot.timestampMs <= 0) return false;
    const auto& db = impl_->db;
    if (!db.exec("BEGIN IMMEDIATE")) return false;
    if (saveSample(*impl_->insertSample, snapshot) &&
        saveRanking(*impl_->insertRank, snapshot.timestampMs, 0, snapshot.byCpu) &&
        saveRanking(*impl_->insertRank, snapshot.timestampMs, 1, snapshot.byMemory) &&
        db.exec("COMMIT")) {
        return true;
    }
    db.exec("ROLLBACK");
    return false;
}

int History::retentionDays() const {
    if (!ready()) return DefaultRetentionDays;
    const Statement setting(impl_->db, "SELECT retention_days FROM settings WHERE id=1");
    if (!setting || setting.step() != SqlRow) return DefaultRetentionDays;
    const auto days = setting.integer(0);
    return days >= 1 && days <= MaxRetentionDays ? static_cast<int>(days) : DefaultRetentionDays;
}

bool History::setRetentionDays(int days) {
    if (!ready() || days < 1 || days > MaxRetentionDays) return false;
    const Statement setting(impl_->db, "UPDATE settings SET retention_days=?1 WHERE id=1");
    return setting && setting.bindInteger(1, days) && setting.step() == SqlDone &&
        impl_->db.api.changes(impl_->db.handle) == 1;
}

bool History::prune(std::int64_t cutoffMs, bool& more) {
    if (!ready() || !impl_->insertSample) return false;
    const Statement prune(impl_->db, R"sql(
        DELETE FROM samples WHERE timestamp_ms IN (
            SELECT timestamp_ms FROM samples WHERE timestamp_ms < ?1 ORDER BY timestamp_ms LIMIT 4096
        )
    )sql");
    if (!prune || !prune.bindInteger(1, cutoffMs) || prune.step() != SqlDone) return false;
    more = impl_->db.api.changes(impl_->db.handle) == 4096;
    Statement trafficPrune(impl_->db,"DELETE FROM traffic_bytes WHERE time<?");
    if(!trafficPrune.bindInteger(1,cutoffMs) || trafficPrune.step()!=SqlDone) return false;
    return true;
}

std::vector<Snapshot> History::series(std::int64_t fromMs, std::int64_t toMs, std::size_t maxPoints) const {
    if (!ready() || fromMs < 0 || toMs < fromMs || !maxPoints) return {};
    const auto pointLimit = static_cast<std::int64_t>(std::min<std::size_t>(maxPoints, 10000));
    const auto bucketSpan = (toMs - fromMs) / pointLimit;
    static const std::string query = R"sql(
        WITH RECURSIVE buckets(start_ms,end_ms) AS (
            SELECT ?1, ?1 + min(?3, ?2 - ?1)
            UNION ALL
            SELECT end_ms + 1, end_ms + 1 + min(?3, ?2 - end_ms - 1)
            FROM buckets WHERE end_ms < ?2
        ) SELECT )sql" + sampleColumns("s.") + R"sql(
        FROM buckets AS b JOIN samples AS s ON s.timestamp_ms = (
            SELECT max(timestamp_ms) FROM samples WHERE timestamp_ms BETWEEN b.start_ms AND b.end_ms
        ) ORDER BY s.timestamp_ms
    )sql";
    const Statement read(impl_->db, query.c_str());
    if (!read || !read.bindInteger(1, fromMs) || !read.bindInteger(2, toMs) || !read.bindInteger(3, bucketSpan)) return {};
    std::vector<Snapshot> snapshots;
    int status;
    while ((status = read.step()) == SqlRow) snapshots.push_back(readSnapshot(read));
    if (status != SqlDone) return {};
    return snapshots;
}

std::optional<Snapshot> History::at(std::int64_t timestampMs) const {
    if (!ready()) return std::nullopt;
    static const std::string query = "SELECT " + sampleColumns("s.") + R"sql(
        ,r.sort_kind,r.place,r.pid,r.name,r.cpu,r.memory
        FROM samples AS s LEFT JOIN rankings AS r USING(timestamp_ms)
        WHERE s.timestamp_ms = (SELECT max(timestamp_ms) FROM samples WHERE timestamp_ms <= ?1)
    )sql";
    const Statement read(impl_->db, query.c_str());
    if (!read || !read.bindInteger(1, timestampMs) || read.step() != SqlRow) return std::nullopt;
    Snapshot snapshot = readSnapshot(read);
    constexpr int firstRankColumn = static_cast<int>(MetricCount + 1);
    int status;
    do {
        if (read.isNull(firstRankColumn)) continue;
        const auto kind = read.integer(firstRankColumn);
        const auto place = read.integer(firstRankColumn + 1);
        const auto pid = read.integer(firstRankColumn + 2);
        if (kind < 0 || kind > 1 || place < 0 || place >= static_cast<std::int64_t>(TopCount) ||
            pid <= 0 || pid > std::numeric_limits<std::uint32_t>::max()) continue;
        auto& ranking = kind == 0 ? snapshot.byCpu : snapshot.byMemory;
        auto& entry = ranking[static_cast<std::size_t>(place)];
        entry.pid = static_cast<std::uint32_t>(pid);
        read.readName(firstRankColumn + 3, entry);
        entry.cpu = read.number(firstRankColumn + 4);
        entry.memory = static_cast<std::uint64_t>(std::max<std::int64_t>(0, read.integer(firstRankColumn + 5)));
    } while ((status = read.step()) == SqlRow);
    return status == SqlDone ? std::optional<Snapshot>(snapshot) : std::nullopt;
}

} // namespace cw

namespace cw {
bool History::saveTraffic(const TrafficPage& page, std::int64_t now) {
    if(!ready() || page.status || !page.started) return false;
    auto& db=impl_->db;
    if(!db.exec("BEGIN IMMEDIATE")) return false;
    bool ok=true;
    Statement previous(db,"SELECT public,lan,other FROM traffic_cursor WHERE session=? AND path=?");
    Statement cursor(db,"INSERT OR REPLACE INTO traffic_cursor VALUES(?,?,?,?,?,?)");
    Statement insert(db,"INSERT INTO traffic_bytes VALUES(?,?,?,?,?)");
    for(unsigned i=0;ok && i<std::min<unsigned>(page.count,page.rows.size());++i) {
        const auto& r=page.rows[i]; std::wstring path(r.path);
        previous.bindInteger(1,page.started); previous.text(2,path);
        std::int64_t a=0,b=0,c=0;
        if(previous.step()==SqlRow){a=previous.integer(0);b=previous.integer(1);c=previous.integer(2);}
        previous.reset();
        auto da=static_cast<std::int64_t>(r.internet)-a, dbb=static_cast<std::int64_t>(r.lan)-b, dc=static_cast<std::int64_t>(r.other)-c;
        if(da<0 || dbb<0 || dc<0){ok=false;break;}
        if(!da && !dbb && !dc) continue;
        ok=insert.bindInteger(1,now) && insert.text(2,path) && insert.bindInteger(3,da) && insert.bindInteger(4,dbb) && insert.bindInteger(5,dc) && insert.step()==SqlDone;
        insert.reset();
        ok=ok && cursor.bindInteger(1,page.started) && cursor.text(2,path) && cursor.bindInteger(3,r.internet) && cursor.bindInteger(4,r.lan) && cursor.bindInteger(5,r.other) && cursor.bindInteger(6,now) && cursor.step()==SqlDone;
        cursor.reset();
    }
    // Keep active session cursors even when older traffic expires, avoiding recounts.
    Statement prune(db,"DELETE FROM traffic_bytes WHERE time<?");
    prune.bindInteger(1,now-static_cast<std::int64_t>(retentionDays())*86400000);
    ok=ok && prune.step()==SqlDone;
    Statement pruneCursor(db,"DELETE FROM traffic_cursor WHERE updated<? AND session<>?");
    pruneCursor.bindInteger(1,now-static_cast<std::int64_t>(retentionDays())*86400000); pruneCursor.bindInteger(2,page.started);
    ok=ok && pruneCursor.step()==SqlDone;
    if(ok && db.exec("COMMIT")) return true;
    db.exec("ROLLBACK"); return false;
}
std::vector<TrafficTotal> History::trafficTotals(std::int64_t from) const {
    std::vector<TrafficTotal> rows; if(!ready()) return rows;
    Statement q(impl_->db,"SELECT path,SUM(public),SUM(lan),SUM(other) FROM traffic_bytes WHERE time>=? GROUP BY path ORDER BY SUM(public) DESC,path");
    q.bindInteger(1,from);
    while(q.step()==SqlRow) rows.push_back({q.text(0),static_cast<std::uint64_t>(q.integer(1)),static_cast<std::uint64_t>(q.integer(2)),static_cast<std::uint64_t>(q.integer(3)),0});
    return rows;
}
}
