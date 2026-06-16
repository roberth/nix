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
--
-- Response payloads are *not* persisted. Walk dispatch recomputes the
-- live response from the current environment and compares hashes
-- only; the recorded payload bytes are never read back. Keeping just
-- the hash (which appears in FactSet members and is therefore
-- implicit in Asks/Terminals reachability) suffices for correctness.

CREATE TABLE IF NOT EXISTS Requests (
    requestHash BLOB PRIMARY KEY,
    payload     BLOB NOT NULL
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
-- members is the raw concatenation of sorted-deduplicated element
-- hashes (RequestSet, 32 bytes per member). setHash = SHA-256(members).
--
-- FactSets are *not* persisted. Recording walks emit an intermediate
-- FactSet per step, growing 1..N. Storing every intermediate cost
-- O(N²) bytes per query (94% of the DB on a 10-attr sample). The
-- decision-graph layer doesn't need FactSet members on disk: the Asks
-- and Terminals tables are keyed by (queryHash, factSetHash), so a
-- recording reaching some intermediate position is detectable via
-- "any Asks/Terminal row at (Q, factSetHash)?". walk() maintains the
-- current FactSet members in-process.

CREATE TABLE IF NOT EXISTS RequestSets (
    setHash BLOB PRIMARY KEY,
    members BLOB NOT NULL
);

-- Decision graph layer: two edge tables, both keyed by (queryHash, factSetHash).

-- No separate (queryHash, factSetHash) index is needed: the primary
-- key prefix already covers WHERE-by-(queryHash, factSetHash) lookups.
-- WITHOUT ROWID stores rows directly in the PK B-tree instead of in a
-- separate heap with a duplicate PK index — a ~50% reduction in
-- on-disk size for these all-blob, no-other-payload tables.
CREATE TABLE IF NOT EXISTS Asks (
    queryHash      BLOB NOT NULL,
    factSetHash    BLOB NOT NULL,
    requestSetHash BLOB NOT NULL,
    PRIMARY KEY (queryHash, factSetHash, requestSetHash)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Terminals (
    queryHash   BLOB NOT NULL,
    factSetHash BLOB NOT NULL,
    resultHash  BLOB NOT NULL,
    PRIMARY KEY (queryHash, factSetHash, resultHash)
) WITHOUT ROWID;

-- Clean up indexes from earlier schema versions, if present.
DROP INDEX IF EXISTS AsksByQF;
DROP INDEX IF EXISTS TerminalsByQF;
)sql";

struct TracingDecisionGraph::State
{
    SQLite db;

    /* Storage layer */
    SQLiteStmt insertRequest, insertQuery, insertResult;
    SQLiteStmt selectRequest, selectQuery, selectResult;
    SQLiteStmt insertRequestSet;
    SQLiteStmt selectRequestSet;
    SQLiteStmt countAsks, countTerminals;

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

/* Set-pool serialisation. The pool blobs are internal; they don't need
   to be human-readable or self-describing. SHA-256 hashes are fixed
   size, so a raw concatenation of canonical members is both the
   storage form and the hash input. This avoids per-call CBOR
   encode/decode through nlohmann::json, which dominated walk()
   profiles. Format version is implicit in the schema: changing it
   invalidates the on-disk cache. */
template<typename T>
static std::string dg_serialiseMembers(const std::vector<T> & members);

template<>
std::string dg_serialiseMembers<Hash>(const std::vector<Hash> & members)
{
    const size_t hs = members.empty() ? Hash(HashAlgorithm::SHA256).hashSize : members[0].hashSize;
    std::string out;
    out.reserve(members.size() * hs);
    for (const auto & h : members)
        out.append(reinterpret_cast<const char *>(h.hash), h.hashSize);
    return out;
}

template<>
std::string dg_serialiseMembers<TracingDecisionGraph::Fact>(
    const std::vector<TracingDecisionGraph::Fact> & members)
{
    const size_t hs = Hash(HashAlgorithm::SHA256).hashSize;
    std::string out;
    out.reserve(members.size() * hs * 2);
    for (const auto & f : members) {
        out.append(reinterpret_cast<const char *>(f.request.hash), f.request.hashSize);
        out.append(reinterpret_cast<const char *>(f.response.hash), f.response.hashSize);
    }
    return out;
}

static std::vector<Hash> dg_deserialiseRequestMembers(std::string_view bytes)
{
    const size_t hs = Hash(HashAlgorithm::SHA256).hashSize;
    if (bytes.size() % hs != 0)
        throw Error("decision-graph: malformed RequestSet blob (size=%d)", bytes.size());
    std::vector<Hash> out;
    out.reserve(bytes.size() / hs);
    for (size_t i = 0; i < bytes.size(); i += hs)
        out.push_back(dg_blobToHash(bytes.substr(i, hs)));
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
    state->insertQuery.create(state->db,
        "INSERT OR IGNORE INTO Queries(queryHash, payload) VALUES (?, ?)");
    state->insertResult.create(state->db,
        "INSERT OR IGNORE INTO Results(resultHash, payload) VALUES (?, ?)");

    state->selectRequest.create(state->db,
        "SELECT payload FROM Requests WHERE requestHash = ?");
    state->selectQuery.create(state->db,
        "SELECT payload FROM Queries WHERE queryHash = ?");
    state->selectResult.create(state->db,
        "SELECT payload FROM Results WHERE resultHash = ?");

    /* Drop obsolete tables from earlier schema versions. */
    state->db.exec("DROP TABLE IF EXISTS Responses;");
    state->db.exec("DROP TABLE IF EXISTS FactSets;");

    state->insertRequestSet.create(state->db,
        "INSERT OR IGNORE INTO RequestSets(setHash, members) VALUES (?, ?)");
    state->selectRequestSet.create(state->db,
        "SELECT members FROM RequestSets WHERE setHash = ?");

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
    state->countAsks.create(state->db,
        "SELECT 1 FROM Asks WHERE queryHash = ? AND factSetHash = ? LIMIT 1");
    state->countTerminals.create(state->db,
        "SELECT 1 FROM Terminals WHERE queryHash = ? AND factSetHash = ? LIMIT 1");
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
    /* FactSets are not persisted; only the hash is meaningful as a key
       into Asks/Terminals. The members are kept in-process so the
       caller (record / walk) can still inspect them within one
       invocation. */
    auto canonical = dg_sortAndDedup(std::move(members));
    auto bytes = dg_serialiseMembers(canonical);
    auto setHash = hashString(HashAlgorithm::SHA256, bytes);
    auto state(_state->lock());
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
    /* FactSets aren't persisted; if not in the in-process cache it's
       unknown to us. Walks reconstruct curFacts incrementally and
       don't need this path. */
    return std::nullopt;
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
        throw Error("decision-graph: record(Q, factSet, result) called with FactSet hash not in the in-process cache");

    /* Walk the Facts in canonical order (already sorted by getFactSet),
       writing one singleton Asks edge per Fact and extending cur.
       The intermediate FactSets are kept only in the in-process cache,
       not persisted. */
    auto cur = emptySetHash();
    std::vector<Fact> curFacts;
    curFacts.reserve(facts->size());
    for (const auto & f : *facts) {
        auto requestSetHash = insertRequestSet({f.request});
        insertAsks(q, cur, requestSetHash);
        curFacts.push_back(f);
        cur = insertFactSet(curFacts); // in-memory only
    }
    insertTerminal(q, factSetHash, result);
}

bool TracingDecisionGraph::hasAnyEdge(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    {
        auto check = state->countAsks.use();
        dg_bindBlob(check, dg_hashToBlob(q));
        dg_bindBlob(check, dg_hashToBlob(factSet));
        if (check.next())
            return true;
    }
    auto check = state->countTerminals.use();
    dg_bindBlob(check, dg_hashToBlob(q));
    dg_bindBlob(check, dg_hashToBlob(factSet));
    return check.next();
}

std::optional<TracingDecisionGraph::ResultHash> TracingDecisionGraph::walk(
    const QueryHash & q,
    const std::function<ResponseHash(const RequestHash &)> & dispatch)
{
    auto cur = emptySetHash();
    /* Maintain curFacts and curRequests in this walk's locals.
       FactSets aren't persisted, so we can't fetch them mid-walk;
       reconstruct as we go. curRequests speeds up the "is this
       request already in cur?" filter on each edge. */
    std::vector<Fact> curFacts;
    std::unordered_set<RequestHash> curRequests;
    for (;;) {
        if (auto term = getTerminal(q, cur))
            return *term;

        auto outgoing = getAsks(q, cur);
        if (outgoing.empty())
            return std::nullopt; // no path forward, no terminal

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

            /* Compute the candidate next FactSet hash. For the common
               singleton-step case, consult the (parent, request,
               response) → child cache before recomputing. */
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

            /* Validate that some recording for THIS query reaches
               (Q, nextCur). With FactSets gone, this is the
               on-the-Q-graph membership check. */
            if (!hasAnyEdge(q, nextCur))
                continue; // wrong branch

            cur = nextCur;
            for (const auto & f : newFacts) {
                curFacts.push_back(f);
                curRequests.insert(f.request);
            }
            advanced = true;
            break;
        }

        if (!advanced)
            return std::nullopt;
    }
}

} // namespace nix
