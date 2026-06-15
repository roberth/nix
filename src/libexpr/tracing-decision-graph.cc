#include "nix/expr/tracing-decision-graph.hh"
#include "nix/store/sqlite.hh"
#include <sqlite3.h>
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace nix {

static const char * decisionGraphSchema = R"sql(
-- Storage layer: atomic content-addressed pools.

CREATE TABLE IF NOT EXISTS Requests (
    requestHash BLOB PRIMARY KEY,
    payload     BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS Responses (
    responseHash BLOB PRIMARY KEY,
    payload      BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS Queries (
    queryHash BLOB PRIMARY KEY,
    payload   BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS Results (
    resultHash BLOB PRIMARY KEY,
    payload    BLOB NOT NULL
);

-- Storage layer: set pools.
-- members is CBOR-encoded sorted-deduplicated list of element hashes
-- (RequestSet) or of (requestHash, responseHash) pairs (FactSet).
-- setHash = SHA-256(members).

CREATE TABLE IF NOT EXISTS RequestSets (
    setHash BLOB PRIMARY KEY,
    members BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS FactSets (
    setHash BLOB PRIMARY KEY,
    members BLOB NOT NULL
);

-- Decision graph layer: two edge tables, both keyed by (queryHash, factSetHash).

CREATE TABLE IF NOT EXISTS Asks (
    queryHash      BLOB NOT NULL,
    factSetHash    BLOB NOT NULL,
    requestSetHash BLOB NOT NULL,
    PRIMARY KEY (queryHash, factSetHash, requestSetHash)
);
CREATE INDEX IF NOT EXISTS AsksByQF ON Asks(queryHash, factSetHash);

CREATE TABLE IF NOT EXISTS Terminals (
    queryHash   BLOB NOT NULL,
    factSetHash BLOB NOT NULL,
    resultHash  BLOB NOT NULL,
    PRIMARY KEY (queryHash, factSetHash, resultHash)
);
CREATE INDEX IF NOT EXISTS TerminalsByQF ON Terminals(queryHash, factSetHash);
)sql";

struct TracingDecisionGraph::State
{
    SQLite db;

    /* Storage layer */
    SQLiteStmt insertRequest, insertResponse, insertQuery, insertResult;
    SQLiteStmt selectRequest, selectResponse, selectQuery, selectResult;
    SQLiteStmt insertRequestSet, insertFactSet;
    SQLiteStmt selectRequestSet, selectFactSet;

    /* Decision graph layer */
    SQLiteStmt insertAsks, selectAsks, deleteAsks;
    SQLiteStmt insertTerminal, selectTerminal;
};

/* ─────────────────────────────────────────────────────────────────────
   Helpers
   ───────────────────────────────────────────────────────────────────── */

static std::string hashToBlob(const Hash & h)
{
    return std::string(reinterpret_cast<const char *>(h.hash), h.hashSize);
}

static Hash blobToHash(std::string_view blob)
{
    if (blob.size() != Hash(HashAlgorithm::SHA256).hashSize)
        throw Error("decision-graph: malformed hash blob (size=%d)", blob.size());
    Hash h(HashAlgorithm::SHA256);
    std::memcpy(h.hash, blob.data(), blob.size());
    return h;
}

static void bindBlob(SQLiteStmt::Use & use, std::string_view blob)
{
    use(std::string(blob), true /* binary */);
}

template<typename T>
static std::string serialiseMembers(const std::vector<T> & members);

template<>
std::string serialiseMembers<Hash>(const std::vector<Hash> & members)
{
    /* CBOR-encoded array of byte strings. */
    nlohmann::json j = nlohmann::json::array();
    for (const auto & h : members)
        j.push_back(nlohmann::json::binary(
            std::vector<uint8_t>(h.hash, h.hash + h.hashSize)));
    auto cbor = nlohmann::json::to_cbor(j);
    return std::string(cbor.begin(), cbor.end());
}

template<>
std::string serialiseMembers<TracingDecisionGraph::Fact>(
    const std::vector<TracingDecisionGraph::Fact> & members)
{
    /* CBOR-encoded array of [requestHash, responseHash] pairs. */
    nlohmann::json j = nlohmann::json::array();
    for (const auto & f : members) {
        nlohmann::json pair = nlohmann::json::array();
        pair.push_back(nlohmann::json::binary(
            std::vector<uint8_t>(f.request.hash, f.request.hash + f.request.hashSize)));
        pair.push_back(nlohmann::json::binary(
            std::vector<uint8_t>(f.response.hash, f.response.hash + f.response.hashSize)));
        j.push_back(std::move(pair));
    }
    auto cbor = nlohmann::json::to_cbor(j);
    return std::string(cbor.begin(), cbor.end());
}

static std::vector<Hash> deserialiseRequestMembers(std::string_view bytes)
{
    auto j = nlohmann::json::from_cbor(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    std::vector<Hash> out;
    out.reserve(j.size());
    for (const auto & item : j) {
        const auto & bin = item.get_binary();
        out.push_back(blobToHash(std::string_view(
            reinterpret_cast<const char *>(bin.data()), bin.size())));
    }
    return out;
}

static std::vector<TracingDecisionGraph::Fact> deserialiseFactMembers(std::string_view bytes)
{
    auto j = nlohmann::json::from_cbor(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    std::vector<TracingDecisionGraph::Fact> out;
    out.reserve(j.size());
    for (const auto & item : j) {
        const auto & req = item.at(0).get_binary();
        const auto & resp = item.at(1).get_binary();
        TracingDecisionGraph::Fact f{
            .request = blobToHash(std::string_view(
                reinterpret_cast<const char *>(req.data()), req.size())),
            .response = blobToHash(std::string_view(
                reinterpret_cast<const char *>(resp.data()), resp.size())),
        };
        out.push_back(f);
    }
    return out;
}

/* ─────────────────────────────────────────────────────────────────────
   Construction / database path
   ───────────────────────────────────────────────────────────────────── */

static std::filesystem::path defaultDbPath()
{
    if (auto override = getEnvNonEmpty("NIX_TRACING_CACHE_DIR"))
        return std::filesystem::path(*override) / "index.sqlite";
    auto cacheDir = std::filesystem::path(getCacheDir()) / "eval-tracing-decision-graph";
    return cacheDir / "index.sqlite";
}

TracingDecisionGraph::TracingDecisionGraph()
    : TracingDecisionGraph(defaultDbPath())
{
}

TracingDecisionGraph::TracingDecisionGraph(const std::filesystem::path & dbPath)
    : _state(std::make_unique<Sync<State>>())
{
    auto state(_state->lock());

    auto parent = dbPath.parent_path();
    if (!parent.empty())
        createDirs(parent);

    state->db = SQLite(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
    state->db.isCache();
    state->db.exec(decisionGraphSchema);

    state->insertRequest.create(state->db,
        "INSERT OR IGNORE INTO Requests(requestHash, payload) VALUES (?, ?)");
    state->insertResponse.create(state->db,
        "INSERT OR IGNORE INTO Responses(responseHash, payload) VALUES (?, ?)");
    state->insertQuery.create(state->db,
        "INSERT OR IGNORE INTO Queries(queryHash, payload) VALUES (?, ?)");
    state->insertResult.create(state->db,
        "INSERT OR IGNORE INTO Results(resultHash, payload) VALUES (?, ?)");

    state->selectRequest.create(state->db,
        "SELECT payload FROM Requests WHERE requestHash = ?");
    state->selectResponse.create(state->db,
        "SELECT payload FROM Responses WHERE responseHash = ?");
    state->selectQuery.create(state->db,
        "SELECT payload FROM Queries WHERE queryHash = ?");
    state->selectResult.create(state->db,
        "SELECT payload FROM Results WHERE resultHash = ?");

    state->insertRequestSet.create(state->db,
        "INSERT OR IGNORE INTO RequestSets(setHash, members) VALUES (?, ?)");
    state->insertFactSet.create(state->db,
        "INSERT OR IGNORE INTO FactSets(setHash, members) VALUES (?, ?)");
    state->selectRequestSet.create(state->db,
        "SELECT members FROM RequestSets WHERE setHash = ?");
    state->selectFactSet.create(state->db,
        "SELECT members FROM FactSets WHERE setHash = ?");

    state->insertAsks.create(state->db,
        "INSERT OR IGNORE INTO Asks(queryHash, factSetHash, requestSetHash) VALUES (?, ?, ?)");
    state->selectAsks.create(state->db,
        "SELECT requestSetHash FROM Asks WHERE queryHash = ? AND factSetHash = ?");
    state->deleteAsks.create(state->db,
        "DELETE FROM Asks WHERE queryHash = ? AND factSetHash = ? AND requestSetHash = ?");
    state->insertTerminal.create(state->db,
        "INSERT OR IGNORE INTO Terminals(queryHash, factSetHash, resultHash) VALUES (?, ?, ?)");
    state->selectTerminal.create(state->db,
        "SELECT resultHash FROM Terminals WHERE queryHash = ? AND factSetHash = ?");
}

TracingDecisionGraph::~TracingDecisionGraph() = default;

void TracingDecisionGraph::waitForWrites()
{
    /* Synchronous writes for Phase 1; nothing to wait for. */
}

/* ─────────────────────────────────────────────────────────────────────
   Storage layer: atoms
   ───────────────────────────────────────────────────────────────────── */

#define ATOM_INSERT(NAME, FIELD)                                                 \
    void TracingDecisionGraph::insert##NAME(const Hash & h, std::string_view p) \
    {                                                                            \
        auto state(_state->lock());                                              \
        auto use = state->insert##NAME.use();                                    \
        bindBlob(use, hashToBlob(h));                                            \
        bindBlob(use, p);                                                        \
        use.exec();                                                              \
    }

ATOM_INSERT(Request, requestHash)
ATOM_INSERT(Response, responseHash)
ATOM_INSERT(Query, queryHash)
ATOM_INSERT(Result, resultHash)
#undef ATOM_INSERT

#define ATOM_GET(NAME)                                                          \
    std::optional<std::string> TracingDecisionGraph::get##NAME##Payload(        \
        const Hash & h)                                                         \
    {                                                                           \
        auto state(_state->lock());                                             \
        auto query = state->select##NAME.use();                                 \
        bindBlob(query, hashToBlob(h));                                         \
        if (!query.next())                                                      \
            return std::nullopt;                                                \
        return query.getStr(0);                                                 \
    }

ATOM_GET(Request)
ATOM_GET(Response)
ATOM_GET(Query)
ATOM_GET(Result)
#undef ATOM_GET

/* ─────────────────────────────────────────────────────────────────────
   Storage layer: sets
   ───────────────────────────────────────────────────────────────────── */

template<typename T>
static std::vector<T> sortAndDedup(std::vector<T> members)
{
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeRequestSetHash(const std::vector<RequestHash> & members)
{
    auto canonical = sortAndDedup(members);
    auto bytes = serialiseMembers(canonical);
    return hashString(HashAlgorithm::SHA256, bytes);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeFactSetHash(const std::vector<Fact> & members)
{
    auto canonical = sortAndDedup(members);
    auto bytes = serialiseMembers(canonical);
    return hashString(HashAlgorithm::SHA256, bytes);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::emptySetHash()
{
    /* Empty list, CBOR-encoded, SHA-256'd. Computed once and cached. */
    static const SetHash h = []() {
        std::vector<Hash> empty;
        auto bytes = serialiseMembers(empty);
        return hashString(HashAlgorithm::SHA256, bytes);
    }();
    return h;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertRequestSet(std::vector<RequestHash> members)
{
    auto canonical = sortAndDedup(std::move(members));
    auto bytes = serialiseMembers(canonical);
    auto setHash = hashString(HashAlgorithm::SHA256, bytes);
    auto state(_state->lock());
    auto use = state->insertRequestSet.use();
    bindBlob(use, hashToBlob(setHash));
    bindBlob(use, bytes);
    use.exec();
    return setHash;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertFactSet(std::vector<Fact> members)
{
    auto canonical = sortAndDedup(std::move(members));
    auto bytes = serialiseMembers(canonical);
    auto setHash = hashString(HashAlgorithm::SHA256, bytes);
    auto state(_state->lock());
    auto use = state->insertFactSet.use();
    bindBlob(use, hashToBlob(setHash));
    bindBlob(use, bytes);
    use.exec();
    return setHash;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::extendRequestSet(const SetHash & parent, const std::vector<RequestHash> & extras)
{
    auto existing = getRequestSet(parent);
    std::vector<RequestHash> combined = existing.value_or(std::vector<RequestHash>{});
    combined.insert(combined.end(), extras.begin(), extras.end());
    return insertRequestSet(std::move(combined));
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::extendFactSet(const SetHash & parent, const std::vector<Fact> & extras)
{
    auto existing = getFactSet(parent);
    std::vector<Fact> combined = existing.value_or(std::vector<Fact>{});
    combined.insert(combined.end(), extras.begin(), extras.end());
    return insertFactSet(std::move(combined));
}

std::optional<std::vector<TracingDecisionGraph::RequestHash>>
TracingDecisionGraph::getRequestSet(const SetHash & h)
{
    if (h == emptySetHash())
        return std::vector<RequestHash>{};
    auto state(_state->lock());
    auto query = state->selectRequestSet.use();
    bindBlob(query, hashToBlob(h));
    if (!query.next())
        return std::nullopt;
    return deserialiseRequestMembers(query.getStr(0));
}

std::optional<std::vector<TracingDecisionGraph::Fact>>
TracingDecisionGraph::getFactSet(const SetHash & h)
{
    if (h == emptySetHash())
        return std::vector<Fact>{};
    auto state(_state->lock());
    auto query = state->selectFactSet.use();
    bindBlob(query, hashToBlob(h));
    if (!query.next())
        return std::nullopt;
    return deserialiseFactMembers(query.getStr(0));
}

/* ─────────────────────────────────────────────────────────────────────
   Decision graph layer
   ───────────────────────────────────────────────────────────────────── */

void TracingDecisionGraph::insertAsks(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto use = state->insertAsks.use();
    bindBlob(use, hashToBlob(q));
    bindBlob(use, hashToBlob(factSet));
    bindBlob(use, hashToBlob(requestSet));
    use.exec();
}

std::vector<TracingDecisionGraph::SetHash>
TracingDecisionGraph::getAsks(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto query = state->selectAsks.use();
    bindBlob(query, hashToBlob(q));
    bindBlob(query, hashToBlob(factSet));
    std::vector<SetHash> out;
    while (query.next())
        out.push_back(blobToHash(query.getStr(0)));
    return out;
}

void TracingDecisionGraph::removeAsks(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto use = state->deleteAsks.use();
    bindBlob(use, hashToBlob(q));
    bindBlob(use, hashToBlob(factSet));
    bindBlob(use, hashToBlob(requestSet));
    use.exec();
}

void TracingDecisionGraph::insertTerminal(
    const QueryHash & q, const SetHash & factSet, const ResultHash & result)
{
    auto state(_state->lock());
    auto use = state->insertTerminal.use();
    bindBlob(use, hashToBlob(q));
    bindBlob(use, hashToBlob(factSet));
    bindBlob(use, hashToBlob(result));
    use.exec();
}

std::optional<TracingDecisionGraph::ResultHash>
TracingDecisionGraph::getTerminal(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto query = state->selectTerminal.use();
    bindBlob(query, hashToBlob(q));
    bindBlob(query, hashToBlob(factSet));
    if (!query.next())
        return std::nullopt;
    return blobToHash(query.getStr(0));
}

} // namespace nix
