#include "nix/expr/tracing-decision-graph.hh"
#include "nix/store/sqlite.hh"
#include <sqlite3.h>
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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

    /* In-memory caches of parsed sets and payloads. Populated lazily on
       first read or write so that subsequent operations within the same
       process avoid the SQLite round-trip and the CBOR decode.
       std::optional<vector<...>> distinguishes a known-empty result from
       a known-missing one. */
    std::unordered_map<Hash, std::optional<std::vector<Hash>>> requestSetCache;
    std::unordered_map<Hash, std::optional<std::vector<TracingDecisionGraph::Fact>>> factSetCache;
    std::unordered_map<Hash, std::optional<std::string>> requestPayloadCache;
    std::unordered_map<Hash, std::optional<std::string>> responsePayloadCache;
    std::unordered_map<Hash, std::optional<std::string>> resultPayloadCache;

    /* walk() extends the cur FactSet by one Fact per step and re-hashes
       the whole canonical form to identify the child. Naively that's
       O(N²) work per walk; cache (parent, request, response) → child
       so repeated walks across shared prefixes pay it once. */
    struct FactExtKey
    {
        Hash parent;
        Hash request;
        Hash response;
        bool operator==(const FactExtKey & o) const noexcept
        {
            return parent == o.parent && request == o.request && response == o.response;
        }
    };
    struct FactExtKeyHash
    {
        size_t operator()(const FactExtKey & k) const noexcept
        {
            auto h = std::hash<Hash>{}(k.parent);
            h ^= std::hash<Hash>{}(k.request) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<Hash>{}(k.response) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<FactExtKey, Hash, FactExtKeyHash> factSetExtCache;
};

/* ─────────────────────────────────────────────────────────────────────
   Helpers. Names are dg_-prefixed because unity builds merge this
   TU with tracing-index.cc which defines same-named statics.
   ───────────────────────────────────────────────────────────────────── */

static std::string dg_hashToBlob(const Hash & h)
{
    return std::string(reinterpret_cast<const char *>(h.hash), h.hashSize);
}

static Hash dg_blobToHash(std::string_view blob)
{
    if (blob.size() != Hash(HashAlgorithm::SHA256).hashSize)
        throw Error("decision-graph: malformed hash blob (size=%d)", blob.size());
    Hash h(HashAlgorithm::SHA256);
    std::memcpy(h.hash, blob.data(), blob.size());
    return h;
}

static void dg_bindBlob(SQLiteStmt::Use & use, std::string_view blob)
{
    use(reinterpret_cast<const unsigned char *>(blob.data()), blob.size(), true /* notNull */);
}

template<typename T>
static std::string dg_serialiseMembers(const std::vector<T> & members);

template<>
std::string dg_serialiseMembers<Hash>(const std::vector<Hash> & members)
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
std::string dg_serialiseMembers<TracingDecisionGraph::Fact>(
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

static std::vector<Hash> dg_deserialiseRequestMembers(std::string_view bytes)
{
    auto j = nlohmann::json::from_cbor(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    std::vector<Hash> out;
    out.reserve(j.size());
    for (const auto & item : j) {
        const auto & bin = item.get_binary();
        out.push_back(dg_blobToHash(std::string_view(
            reinterpret_cast<const char *>(bin.data()), bin.size())));
    }
    return out;
}

static std::vector<TracingDecisionGraph::Fact> dg_deserialiseFactMembers(std::string_view bytes)
{
    auto j = nlohmann::json::from_cbor(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    std::vector<TracingDecisionGraph::Fact> out;
    out.reserve(j.size());
    for (const auto & item : j) {
        const auto & req = item.at(0).get_binary();
        const auto & resp = item.at(1).get_binary();
        TracingDecisionGraph::Fact f{
            .request = dg_blobToHash(std::string_view(
                reinterpret_cast<const char *>(req.data()), req.size())),
            .response = dg_blobToHash(std::string_view(
                reinterpret_cast<const char *>(resp.data()), resp.size())),
        };
        out.push_back(f);
    }
    return out;
}

/* ─────────────────────────────────────────────────────────────────────
   Construction / database path
   ───────────────────────────────────────────────────────────────────── */

static std::filesystem::path dg_defaultDbPath()
{
    /* Use a distinct filename so v12 and v13 can coexist in the
       same directory during the migration period. */
    if (auto override = getEnvNonEmpty("NIX_TRACING_CACHE_DIR"))
        return std::filesystem::path(*override) / "decision-graph.sqlite";
    auto cacheDir = std::filesystem::path(getCacheDir()) / "eval-tracing-decision-graph";
    return cacheDir / "index.sqlite";
}

Hash TracingDecisionGraph::computeResponseHash(const std::string & payload)
{
    return hashString(HashAlgorithm::SHA256, payload);
}

TracingDecisionGraph::TracingDecisionGraph()
    : TracingDecisionGraph(dg_defaultDbPath())
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

#define ATOM_INSERT_CACHED(NAME, CACHE)                                          \
    void TracingDecisionGraph::insert##NAME(const Hash & h, std::string_view p) \
    {                                                                            \
        auto state(_state->lock());                                              \
        auto use = state->insert##NAME.use();                                    \
        dg_bindBlob(use, dg_hashToBlob(h));                                      \
        dg_bindBlob(use, p);                                                     \
        use.exec();                                                              \
        /* Mirror INSERT OR IGNORE: only the first payload wins. */              \
        state->CACHE.try_emplace(h, std::optional{std::string(p)});              \
    }

#define ATOM_INSERT_PLAIN(NAME)                                                  \
    void TracingDecisionGraph::insert##NAME(const Hash & h, std::string_view p) \
    {                                                                            \
        auto state(_state->lock());                                              \
        auto use = state->insert##NAME.use();                                    \
        dg_bindBlob(use, dg_hashToBlob(h));                                      \
        dg_bindBlob(use, p);                                                     \
        use.exec();                                                              \
    }

ATOM_INSERT_CACHED(Request, requestPayloadCache)
ATOM_INSERT_CACHED(Response, responsePayloadCache)
ATOM_INSERT_PLAIN(Query)
ATOM_INSERT_CACHED(Result, resultPayloadCache)
#undef ATOM_INSERT_CACHED
#undef ATOM_INSERT_PLAIN

#define ATOM_GET_CACHED(NAME, CACHE)                                            \
    std::optional<std::string> TracingDecisionGraph::get##NAME##Payload(        \
        const Hash & h)                                                         \
    {                                                                           \
        auto state(_state->lock());                                             \
        if (auto it = state->CACHE.find(h); it != state->CACHE.end())           \
            return it->second;                                                  \
        auto query = state->select##NAME.use();                                 \
        dg_bindBlob(query, dg_hashToBlob(h));                                   \
        std::optional<std::string> payload;                                     \
        if (query.next())                                                       \
            payload = query.getBlob(0);                                         \
        state->CACHE.emplace(h, payload);                                       \
        return payload;                                                         \
    }

#define ATOM_GET_PLAIN(NAME)                                                    \
    std::optional<std::string> TracingDecisionGraph::get##NAME##Payload(        \
        const Hash & h)                                                         \
    {                                                                           \
        auto state(_state->lock());                                             \
        auto query = state->select##NAME.use();                                 \
        dg_bindBlob(query, dg_hashToBlob(h));                                   \
        if (!query.next())                                                      \
            return std::nullopt;                                                \
        return query.getBlob(0);                                                \
    }

ATOM_GET_CACHED(Request, requestPayloadCache)
ATOM_GET_CACHED(Response, responsePayloadCache)
ATOM_GET_PLAIN(Query)
ATOM_GET_CACHED(Result, resultPayloadCache)
#undef ATOM_GET_CACHED
#undef ATOM_GET_PLAIN

/* ─────────────────────────────────────────────────────────────────────
   Storage layer: sets
   ───────────────────────────────────────────────────────────────────── */

template<typename T>
static std::vector<T> dg_sortAndDedup(std::vector<T> members)
{
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeRequestSetHash(const std::vector<RequestHash> & members)
{
    auto canonical = dg_sortAndDedup(members);
    auto bytes = dg_serialiseMembers(canonical);
    return hashString(HashAlgorithm::SHA256, bytes);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeFactSetHash(const std::vector<Fact> & members)
{
    auto canonical = dg_sortAndDedup(members);
    auto bytes = dg_serialiseMembers(canonical);
    return hashString(HashAlgorithm::SHA256, bytes);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::emptySetHash()
{
    /* Empty list, CBOR-encoded, SHA-256'd. Computed once and cached. */
    static const SetHash h = []() {
        std::vector<Hash> empty;
        auto bytes = dg_serialiseMembers(empty);
        return hashString(HashAlgorithm::SHA256, bytes);
    }();
    return h;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertRequestSet(std::vector<RequestHash> members)
{
    auto canonical = dg_sortAndDedup(std::move(members));
    auto bytes = dg_serialiseMembers(canonical);
    auto setHash = hashString(HashAlgorithm::SHA256, bytes);
    auto state(_state->lock());
    auto use = state->insertRequestSet.use();
    dg_bindBlob(use, dg_hashToBlob(setHash));
    dg_bindBlob(use, bytes);
    use.exec();
    state->requestSetCache.try_emplace(setHash, std::optional{std::move(canonical)});
    return setHash;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertFactSet(std::vector<Fact> members)
{
    auto canonical = dg_sortAndDedup(std::move(members));
    auto bytes = dg_serialiseMembers(canonical);
    auto setHash = hashString(HashAlgorithm::SHA256, bytes);
    auto state(_state->lock());
    auto use = state->insertFactSet.use();
    dg_bindBlob(use, dg_hashToBlob(setHash));
    dg_bindBlob(use, bytes);
    use.exec();
    state->factSetCache.try_emplace(setHash, std::optional{std::move(canonical)});
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
    if (auto it = state->requestSetCache.find(h); it != state->requestSetCache.end())
        return it->second;
    auto query = state->selectRequestSet.use();
    dg_bindBlob(query, dg_hashToBlob(h));
    std::optional<std::vector<RequestHash>> parsed;
    if (query.next())
        parsed = dg_deserialiseRequestMembers(query.getBlob(0));
    state->requestSetCache.emplace(h, parsed);
    return parsed;
}

std::optional<std::vector<TracingDecisionGraph::Fact>>
TracingDecisionGraph::getFactSet(const SetHash & h)
{
    if (h == emptySetHash())
        return std::vector<Fact>{};
    auto state(_state->lock());
    if (auto it = state->factSetCache.find(h); it != state->factSetCache.end())
        return it->second;
    auto query = state->selectFactSet.use();
    dg_bindBlob(query, dg_hashToBlob(h));
    std::optional<std::vector<Fact>> parsed;
    if (query.next())
        parsed = dg_deserialiseFactMembers(query.getBlob(0));
    state->factSetCache.emplace(h, parsed);
    return parsed;
}

/* ─────────────────────────────────────────────────────────────────────
   Decision graph layer
   ───────────────────────────────────────────────────────────────────── */

void TracingDecisionGraph::insertAsks(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto use = state->insertAsks.use();
    dg_bindBlob(use, dg_hashToBlob(q));
    dg_bindBlob(use, dg_hashToBlob(factSet));
    dg_bindBlob(use, dg_hashToBlob(requestSet));
    use.exec();
}

std::vector<TracingDecisionGraph::SetHash>
TracingDecisionGraph::getAsks(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto query = state->selectAsks.use();
    dg_bindBlob(query, dg_hashToBlob(q));
    dg_bindBlob(query, dg_hashToBlob(factSet));
    std::vector<SetHash> out;
    while (query.next())
        out.push_back(dg_blobToHash(query.getBlob(0)));
    return out;
}

void TracingDecisionGraph::removeAsks(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto use = state->deleteAsks.use();
    dg_bindBlob(use, dg_hashToBlob(q));
    dg_bindBlob(use, dg_hashToBlob(factSet));
    dg_bindBlob(use, dg_hashToBlob(requestSet));
    use.exec();
}

void TracingDecisionGraph::insertTerminal(
    const QueryHash & q, const SetHash & factSet, const ResultHash & result)
{
    auto state(_state->lock());
    auto use = state->insertTerminal.use();
    dg_bindBlob(use, dg_hashToBlob(q));
    dg_bindBlob(use, dg_hashToBlob(factSet));
    dg_bindBlob(use, dg_hashToBlob(result));
    use.exec();
}

std::optional<TracingDecisionGraph::ResultHash>
TracingDecisionGraph::getTerminal(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto query = state->selectTerminal.use();
    dg_bindBlob(query, dg_hashToBlob(q));
    dg_bindBlob(query, dg_hashToBlob(factSet));
    if (!query.next())
        return std::nullopt;
    return dg_blobToHash(query.getBlob(0));
}

/* ─────────────────────────────────────────────────────────────────────
   Recording and replay
   ───────────────────────────────────────────────────────────────────── */

void TracingDecisionGraph::record(
    const QueryHash & q,
    const SetHash & factSetHash,
    const ResultHash & result)
{
    auto facts = getFactSet(factSetHash);
    if (!facts)
        throw Error("decision-graph: record(Q, factSet, result) called with FactSet hash not in storage");

    /* Walk the Facts in canonical order (already sorted by getFactSet),
       writing one singleton Asks edge per Fact and extending cur. */
    auto cur = emptySetHash();
    for (const auto & f : *facts) {
        auto requestSetHash = insertRequestSet({f.request});
        insertAsks(q, cur, requestSetHash);
        cur = extendFactSet(cur, {f});
    }
    insertTerminal(q, factSetHash, result);
}

std::optional<TracingDecisionGraph::ResultHash> TracingDecisionGraph::walk(
    const QueryHash & q,
    const std::function<ResponseHash(const RequestHash &)> & dispatch)
{
    auto cur = emptySetHash();
    /* curRequests mirrors the cur FactSet's requests for fast
       membership tests when filtering edge requests below. Build it
       once for the empty start and extend it incrementally as the
       walk advances; rebuilding every iteration was O(N²). */
    std::unordered_set<RequestHash> curRequests;
    for (;;) {
        if (auto term = getTerminal(q, cur))
            return *term;

        auto outgoing = getAsks(q, cur);
        if (outgoing.empty())
            return std::nullopt; // no path forward, no terminal

        /* Try each outgoing edge until one leads to a FactSet that
           exists in storage. With singleton-step canonical recording
           the common case has exactly one outgoing edge; the loop
           still covers the path-divergence case where multiple
           singletons coexist at a position (each leading to a
           different next FactSet depending on Response). */
        auto curFactsOpt = getFactSet(cur);
        if (!curFactsOpt)
            return std::nullopt; // shouldn't happen — cur was reachable
        const auto & curFacts = *curFactsOpt;

        bool advanced = false;
        for (const auto & requestSetHash : outgoing) {
            auto requestSetOpt = getRequestSet(requestSetHash);
            if (!requestSetOpt)
                continue;

            /* Useful dispatch: the edge's Requests minus what's
               already in cur. With singleton-step recording the
               edge is a singleton; this loop just yields one
               Request. */
            std::vector<Fact> newFacts;
            for (const auto & req : *requestSetOpt) {
                if (curRequests.count(req))
                    continue;
                auto resp = dispatch(req);
                newFacts.push_back({req, resp});
            }
            if (newFacts.empty())
                continue; // edge would add nothing; degenerate

            /* Compute the candidate next FactSet hash WITHOUT
               inserting it into the pool — we only want to follow
               edges that some recording actually placed there.

               For the common singleton-step case, consult the
               (parent, request, response) → child cache before
               re-canonicalising and re-hashing the full FactSet. */
            Hash nextCur(HashAlgorithm::SHA256);
            bool cached = false;
            std::optional<State::FactExtKey> cacheKey;
            if (newFacts.size() == 1) {
                cacheKey = State::FactExtKey{cur, newFacts[0].request, newFacts[0].response};
                auto state(_state->lock());
                if (auto it = state->factSetExtCache.find(*cacheKey);
                    it != state->factSetExtCache.end()) {
                    nextCur = it->second;
                    cached = true;
                }
            }
            if (!cached) {
                std::vector<Fact> candidate = curFacts;
                candidate.insert(candidate.end(), newFacts.begin(), newFacts.end());
                nextCur = computeFactSetHash(candidate);
                if (cacheKey) {
                    auto state(_state->lock());
                    state->factSetExtCache.emplace(*cacheKey, nextCur);
                }
            }

            if (!getFactSet(nextCur))
                continue; // next FactSet not in storage — wrong branch

            cur = nextCur;
            for (const auto & f : newFacts)
                curRequests.insert(f.request);
            advanced = true;
            break;
        }

        if (!advanced)
            return std::nullopt;
    }
}

} // namespace nix
