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
--
-- RequestSets are stored as a hash-prefix trie of content-addressed
-- nodes — one row per node. Each node is either a leaf (when its
-- members fit under the split threshold) carrying up to
-- TRIE_SPLIT_THRESHOLD Request hashes, or an internal node carrying
-- a sparse list of (bucket-index, child-node-hash) pairs. The bucket
-- index is the top TRIE_RADIX_BITS bits of each Request hash at the
-- current depth. SHA-256 outputs are uniform, so buckets balance in
-- expectation without content-defined chunking.
--
-- Structural sharing: two RequestSets that share elements share the
-- subtrees those elements live in. setHash is the root node's hash;
-- emptySetHash() is the canonical empty (no node stored).
--
-- FactSets are *not* persisted. Recording walks emit an intermediate
-- FactSet per step, growing 1..N. Storing every intermediate cost
-- O(N²) bytes per query. The decision-graph layer doesn't need
-- FactSet members on disk: the Asks and Terminals tables are keyed
-- by (queryHash, factSetHash), so a recording reaching some
-- intermediate position is detectable via "any Asks/Terminal row at
-- (Q, factSetHash)?". walk() maintains the current FactSet members
-- in-process.

CREATE TABLE IF NOT EXISTS RequestSetNodes (
    nodeHash BLOB PRIMARY KEY,
    payload  BLOB NOT NULL
) WITHOUT ROWID;

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
    SQLiteStmt insertRequestSetNode;
    SQLiteStmt selectRequestSetNode;
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
    /* RequestSet trie *node* cache. Different RequestSets that share
       subtrees (via content addressing) hit the same node hashes;
       caching per-node lets second-and-later getRequestSet calls reuse
       the SQLite reads from the first. */
    std::unordered_map<Hash, std::optional<std::string>> requestSetNodePayloadCache;
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

/* Per-element hashes for set hashing.
   H_element(req) and H_element(fact) are SHA-256 of the element's
   canonical bytes — re-hashing gives domain separation so a Hash
   that happens to appear both as a RequestHash and as a fact's
   request component doesn't XOR to the same set-element value.
   With set hash defined as XOR over per-element hashes, set
   extension is O(1) instead of O(N) (no re-sort, no rehash of the
   full set), at the cost of a weaker hash: an attacker who can
   choose set members can construct collisions algebraically. For
   an internal eval cache this is acceptable — the worst case is a
   wrong cache hit which is detected on next use. */
static Hash dg_requestElementHash(const Hash & req)
{
    return hashString(HashAlgorithm::SHA256,
        std::string_view(reinterpret_cast<const char *>(req.hash), req.hashSize));
}

static Hash dg_factElementHash(const Hash & request, const Hash & response)
{
    std::string buf;
    buf.reserve(request.hashSize + response.hashSize);
    buf.append(reinterpret_cast<const char *>(request.hash), request.hashSize);
    buf.append(reinterpret_cast<const char *>(response.hash), response.hashSize);
    return hashString(HashAlgorithm::SHA256, buf);
}

static Hash dg_xorHash(const Hash & a, const Hash & b)
{
    Hash out = a;
    for (size_t i = 0; i < out.hashSize; ++i)
        out.hash[i] ^= b.hash[i];
    return out;
}

/* ──────────────────────────────────────────────────────────────────────
   RequestSet trie: a hash-prefix trie over Request hashes.

   Leaf payload:     [0x00] || hash_1 || hash_2 || ... || hash_n
                     (n ≤ TRIE_SPLIT_THRESHOLD; n hashes sorted lex.)
   Internal payload: [0x01] || (bucket_index_byte || child_node_hash)+
                     (bucket indices sorted ascending; ≤ TRIE_RADIX entries.)

   Bucket index at trie depth d for a hash h = (TRIE_RADIX_BITS bits of h
   starting at bit d * TRIE_RADIX_BITS, MSB first).

   The split threshold is *probabilistic* per the user's design intent:
   no hard upper bound on internal nodes, just "split when leaf would
   exceed threshold." Uniform-random SHA-256 keys keep buckets balanced
   in expectation, so depth ≈ log_TRIE_RADIX(N).
   ────────────────────────────────────────────────────────────────────── */

constexpr int TRIE_RADIX_BITS = 4;
constexpr int TRIE_RADIX = 1 << TRIE_RADIX_BITS; // 16
constexpr size_t TRIE_SPLIT_THRESHOLD = 16;

static uint8_t dg_bucketAt(const Hash & h, int depth)
{
    const int bitOffset = depth * TRIE_RADIX_BITS;
    const int byteIdx = bitOffset / 8;
    const int bitInByte = bitOffset % 8;
    // Read TRIE_RADIX_BITS bits MSB-first starting at byteIdx:bitInByte
    uint16_t word = (uint16_t)h.hash[byteIdx] << 8;
    if (byteIdx + 1 < (int)h.hashSize)
        word |= (uint16_t)h.hash[byteIdx + 1];
    return (uint8_t)((word >> (16 - TRIE_RADIX_BITS - bitInByte)) & ((1 << TRIE_RADIX_BITS) - 1));
}

static std::string dg_trieLeafPayload(const std::vector<Hash> & members)
{
    const size_t hs = Hash(HashAlgorithm::SHA256).hashSize;
    std::string out;
    out.reserve(1 + members.size() * hs);
    out.push_back(0x00);
    for (const auto & h : members)
        out.append(reinterpret_cast<const char *>(h.hash), h.hashSize);
    return out;
}

static std::string dg_trieInternalPayload(const std::vector<std::pair<uint8_t, Hash>> & children)
{
    const size_t hs = Hash(HashAlgorithm::SHA256).hashSize;
    std::string out;
    out.reserve(1 + children.size() * (1 + hs));
    out.push_back(0x01);
    for (const auto & [bucket, child] : children) {
        out.push_back(static_cast<char>(bucket));
        out.append(reinterpret_cast<const char *>(child.hash), child.hashSize);
    }
    return out;
}

struct DgTrieNode
{
    bool isLeaf;
    std::vector<Hash> members;                          // populated when isLeaf
    std::vector<std::pair<uint8_t, Hash>> children;     // populated otherwise
};

static DgTrieNode dg_parseTrieNode(std::string_view payload)
{
    if (payload.empty())
        throw Error("decision-graph: malformed RequestSet node (empty)");
    const size_t hs = Hash(HashAlgorithm::SHA256).hashSize;
    DgTrieNode out;
    out.isLeaf = (payload[0] == 0x00);
    if (out.isLeaf) {
        if ((payload.size() - 1) % hs != 0)
            throw Error("decision-graph: malformed RequestSet leaf (size=%d)", payload.size());
        for (size_t i = 1; i + hs <= payload.size(); i += hs)
            out.members.push_back(dg_blobToHash(payload.substr(i, hs)));
    } else {
        const size_t entrySize = 1 + hs;
        if ((payload.size() - 1) % entrySize != 0)
            throw Error("decision-graph: malformed RequestSet internal node (size=%d)", payload.size());
        for (size_t i = 1; i + entrySize <= payload.size(); i += entrySize) {
            uint8_t bucket = static_cast<uint8_t>(payload[i]);
            out.children.emplace_back(bucket, dg_blobToHash(payload.substr(i + 1, hs)));
        }
    }
    return out;
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

    state->insertRequestSetNode.create(state->db,
        "INSERT OR IGNORE INTO RequestSetNodes(nodeHash, payload) VALUES (?, ?)");
    state->selectRequestSetNode.create(state->db,
        "SELECT payload FROM RequestSetNodes WHERE nodeHash = ?");

    /* Drop the previous flat-blob RequestSets table from earlier
       schema versions if present (incompatible payload format). */
    state->db.exec("DROP TABLE IF EXISTS RequestSets;");

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

/* Pure recursive trie root hash — no DB access. Used by
   computeRequestSetHash (caller has just the members in hand) and by
   insertRequestSet's writer (which also persists each node). */
static Hash dg_trieRootHash(std::vector<Hash> sortedMembers, int depth)
{
    if (sortedMembers.size() <= TRIE_SPLIT_THRESHOLD) {
        auto payload = dg_trieLeafPayload(sortedMembers);
        return hashString(HashAlgorithm::SHA256, payload);
    }
    /* Bucket by the depth'th 4-bit slice. Members come in sorted; a
       stable bucket-sort preserves intra-bucket sortedness. */
    std::vector<std::vector<Hash>> buckets(TRIE_RADIX);
    for (auto & h : sortedMembers)
        buckets[dg_bucketAt(h, depth)].push_back(std::move(h));
    std::vector<std::pair<uint8_t, Hash>> children;
    for (uint8_t i = 0; i < TRIE_RADIX; ++i) {
        if (buckets[i].empty())
            continue;
        children.emplace_back(i, dg_trieRootHash(std::move(buckets[i]), depth + 1));
    }
    auto payload = dg_trieInternalPayload(children);
    return hashString(HashAlgorithm::SHA256, payload);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeRequestSetHash(const std::vector<RequestHash> & members)
{
    if (members.empty())
        return emptySetHash();
    auto canonical = dg_sortAndDedup(members);
    if (canonical.empty())
        return emptySetHash();
    return dg_trieRootHash(std::move(canonical), 0);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeFactSetHash(const std::vector<Fact> & members)
{
    auto canonical = dg_sortAndDedup(members);
    SetHash out = emptySetHash();
    for (const auto & f : canonical)
        out = dg_xorHash(out, dg_factElementHash(f.request, f.response));
    return out;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::emptySetHash()
{
    /* All-zero hash: XOR identity, so H(∅ ∪ {e}) = H_element(e). */
    static const SetHash h = []() {
        SetHash z(HashAlgorithm::SHA256);
        std::memset(z.hash, 0, z.hashSize);
        return z;
    }();
    return h;
}

std::vector<TracingDecisionGraph::RequestHash>
TracingDecisionGraph::usefulDispatch(
    const std::vector<RequestHash> & edgeRequestSet,
    const std::unordered_set<RequestHash> & curRequests)
{
    std::vector<RequestHash> out;
    out.reserve(edgeRequestSet.size());
    for (const auto & req : edgeRequestSet)
        if (!curRequests.count(req))
            out.push_back(req);
    return out;
}

/* Recursively build the trie and INSERT each visited node, returning
   the root node's hash. Same shape as dg_trieRootHash but with the
   side effect of persisting nodes (idempotent via INSERT OR IGNORE
   on (nodeHash); identical subtrees dedupe automatically). */
Hash TracingDecisionGraph::insertTrieRecursive(std::vector<Hash> sortedMembers, int depth)
{
    auto persist = [&](const Hash & nodeHash, const std::string & payload) {
        auto state(_state->lock());
        /* If we've already cached or persisted this node, skip the
           SQLite write. INSERT OR IGNORE makes the write itself
           idempotent, but the cache check saves the trip. */
        auto [it, inserted] = state->requestSetNodePayloadCache.try_emplace(
            nodeHash, std::optional{payload});
        if (!inserted)
            return;
        auto use = state->insertRequestSetNode.use();
        dg_bindBlob(use, dg_hashToBlob(nodeHash));
        dg_bindBlob(use, payload);
        use.exec();
    };
    if (sortedMembers.size() <= TRIE_SPLIT_THRESHOLD) {
        auto payload = dg_trieLeafPayload(sortedMembers);
        auto nodeHash = hashString(HashAlgorithm::SHA256, payload);
        persist(nodeHash, payload);
        return nodeHash;
    }
    std::vector<std::vector<Hash>> buckets(TRIE_RADIX);
    for (auto & h : sortedMembers)
        buckets[dg_bucketAt(h, depth)].push_back(std::move(h));
    std::vector<std::pair<uint8_t, Hash>> children;
    for (uint8_t i = 0; i < TRIE_RADIX; ++i) {
        if (buckets[i].empty())
            continue;
        children.emplace_back(i, insertTrieRecursive(std::move(buckets[i]), depth + 1));
    }
    auto payload = dg_trieInternalPayload(children);
    auto nodeHash = hashString(HashAlgorithm::SHA256, payload);
    persist(nodeHash, payload);
    return nodeHash;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertRequestSet(std::vector<RequestHash> members)
{
    if (members.empty())
        return emptySetHash();
    auto canonical = dg_sortAndDedup(std::move(members));
    if (canonical.empty())
        return emptySetHash();
    /* Snapshot for the in-process member cache so getRequestSet can
       short-circuit the trie traversal within the same process. */
    auto cachedMembers = canonical;
    auto rootHash = insertTrieRecursive(std::move(canonical), 0);
    {
        auto state(_state->lock());
        state->requestSetCache.try_emplace(rootHash, std::optional{std::move(cachedMembers)});
    }
    return rootHash;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertFactSet(std::vector<Fact> members)
{
    /* FactSets are not persisted; only the hash is meaningful as a key
       into Asks/Terminals. The members are kept in-process so the
       caller (record / walk) can still inspect them within one
       invocation. */
    auto canonical = dg_sortAndDedup(std::move(members));
    SetHash setHash = emptySetHash();
    for (const auto & f : canonical)
        setHash = dg_xorHash(setHash, dg_factElementHash(f.request, f.response));
    auto state(_state->lock());
    state->factSetCache.try_emplace(setHash, std::optional{std::move(canonical)});
    return setHash;
}

void TracingDecisionGraph::primeFactSetCache(
    const SetHash & hash, const std::vector<Fact> & members)
{
    auto state(_state->lock());
    /* Always store the latest snapshot — the caller's growing v13FactSet
       supersedes any prior shorter version recorded under the same
       hash. Distinct factSet hashes never collide so this only
       overwrites when the caller has re-primed at the same hash. */
    state->factSetCache.insert_or_assign(hash, std::optional{members});
}

Hash TracingDecisionGraph::xorFactIntoHash(
    const Hash & h, const Hash & request, const Hash & response)
{
    return dg_xorHash(h, dg_factElementHash(request, response));
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

/* Read one trie node payload by its hash. The per-node cache below
   makes shared subtrees (the dominant case) free after the first
   visit, regardless of which RequestSet root they were reached
   through. */
std::optional<std::string> TracingDecisionGraph::getRequestSetNodePayload(const Hash & nodeHash)
{
    {
        auto state(_state->lock());
        if (auto it = state->requestSetNodePayloadCache.find(nodeHash);
            it != state->requestSetNodePayloadCache.end())
            return it->second;
    }
    std::optional<std::string> payload;
    {
        auto state(_state->lock());
        auto query = state->selectRequestSetNode.use();
        dg_bindBlob(query, dg_hashToBlob(nodeHash));
        if (query.next())
            payload = query.getBlob(0);
    }
    {
        auto state(_state->lock());
        state->requestSetNodePayloadCache.emplace(nodeHash, payload);
    }
    return payload;
}

bool TracingDecisionGraph::collectTrieMembers(const Hash & nodeHash, std::vector<RequestHash> & out)
{
    auto payload = getRequestSetNodePayload(nodeHash);
    if (!payload)
        return false;
    auto node = dg_parseTrieNode(*payload);
    if (node.isLeaf) {
        for (auto & h : node.members)
            out.push_back(h);
        return true;
    }
    for (const auto & [bucket, child] : node.children)
        if (!collectTrieMembers(child, out))
            return false;
    return true;
}

std::optional<std::vector<TracingDecisionGraph::RequestHash>>
TracingDecisionGraph::getRequestSet(const SetHash & h)
{
    if (h == emptySetHash())
        return std::vector<RequestHash>{};
    {
        auto state(_state->lock());
        if (auto it = state->requestSetCache.find(h); it != state->requestSetCache.end())
            return it->second;
    }
    std::vector<RequestHash> members;
    if (!collectTrieMembers(h, members)) {
        auto state(_state->lock());
        state->requestSetCache.emplace(h, std::nullopt);
        return std::nullopt;
    }
    {
        auto state(_state->lock());
        state->requestSetCache.emplace(h, std::optional{members});
    }
    return std::optional{std::move(members)};
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

/* Inner body of record(). Both overloads call this with their
   pre-built (responseFor, allRequests). The body doesn't mutate
   either; it tracks a local curRequests for "what I've consumed
   so far". remaining-as-set = allRequests \ curRequests. */
static void dg_recordImpl(
    TracingDecisionGraph & g,
    const Hash & q,
    const Hash & factSetHash,
    const Hash & result,
    const std::unordered_map<Hash, Hash> & responseFor,
    const std::unordered_set<Hash> & allRequests)
{
    auto cur = TracingDecisionGraph::emptySetHash();
    std::unordered_set<Hash> curRequests;

    auto isInRemaining = [&](const Hash & req) {
        return allRequests.count(req) && !curRequests.count(req);
    };

    auto extendCur = [&](const std::vector<Hash> & reqs) {
        for (const auto & req : reqs) {
            assert(!curRequests.count(req));
            auto it = responseFor.find(req);
            assert(it != responseFor.end());
            cur = dg_xorHash(cur, dg_factElementHash(req, it->second));
            curRequests.insert(req);
        }
    };

    auto curExtendedBy = [&](const std::vector<Hash> & reqs) -> Hash {
        Hash h = cur;
        for (const auto & req : reqs) {
            assert(!curRequests.count(req));
            auto it = responseFor.find(req);
            assert(it != responseFor.end());
            h = dg_xorHash(h, dg_factElementHash(req, it->second));
        }
        return h;
    };

    while (curRequests.size() < allRequests.size()) {
        /* Eager Patricia split pass: any existing edge whose
           usefulDispatch partially overlaps remaining gets split. */
        for (const auto & rsHash : g.getAsks(q, cur)) {
            auto rsMembers = g.getRequestSet(rsHash);
            if (!rsMembers)
                continue;
            auto useful = TracingDecisionGraph::usefulDispatch(*rsMembers, curRequests);
            if (useful.empty())
                continue;

            std::vector<Hash> shared;
            shared.reserve(useful.size());
            for (const auto & req : useful)
                if (isInRemaining(req))
                    shared.push_back(req);
            if (shared.empty() || shared.size() == useful.size())
                continue;

            auto sharedRsHash = g.insertRequestSet(shared);
            auto intermediate = curExtendedBy(shared);
            g.insertAsks(q, cur, sharedRsHash);
            g.insertAsks(q, intermediate, rsHash);
            g.removeAsks(q, cur, rsHash);
        }

        std::optional<std::vector<Hash>> followUseful;
        for (const auto & rsHash : g.getAsks(q, cur)) {
            auto rsMembers = g.getRequestSet(rsHash);
            if (!rsMembers)
                continue;
            auto useful = TracingDecisionGraph::usefulDispatch(*rsMembers, curRequests);
            if (useful.empty())
                continue;
            bool subset = std::all_of(useful.begin(), useful.end(),
                [&](const auto & req) { return isInRemaining(req); });
            if (subset) {
                followUseful = std::move(useful);
                break;
            }
        }

        if (followUseful) {
            extendCur(*followUseful);
        } else {
            std::vector<Hash> remainingVec;
            remainingVec.reserve(allRequests.size() - curRequests.size());
            for (const auto & req : allRequests)
                if (!curRequests.count(req))
                    remainingVec.push_back(req);
            auto rsHash = g.insertRequestSet(remainingVec);
            g.insertAsks(q, cur, rsHash);
            extendCur(remainingVec);
        }
    }

    g.insertTerminal(q, factSetHash, result);
}

void TracingDecisionGraph::record(
    const QueryHash & q,
    const SetHash & factSetHash,
    const ResultHash & result)
{
    auto facts = getFactSet(factSetHash);
    if (!facts)
        throw Error("decision-graph: record(Q, factSet, result) called with FactSet hash not in the in-process cache");

    /* Build the per-call responseFor / allRequests from the cached
       factSet members. Callers with these maintained incrementally
       should use the fast-path overload below instead. */
    std::unordered_map<Hash, Hash> responseFor;
    responseFor.reserve(facts->size());
    std::unordered_set<Hash> allRequests;
    allRequests.reserve(facts->size());
    for (const auto & f : *facts) {
        responseFor.emplace(f.request, f.response);
        allRequests.insert(f.request);
    }
    dg_recordImpl(*this, q, factSetHash, result, responseFor, allRequests);
}

void TracingDecisionGraph::record(
    const QueryHash & q,
    const SetHash & factSetHash,
    const ResultHash & result,
    const std::unordered_map<Hash, Hash> & responseFor,
    const std::unordered_set<Hash> & allRequests)
{
    dg_recordImpl(*this, q, factSetHash, result, responseFor, allRequests);
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
    /* curRequests speeds up the "is this request already in cur?"
       filter on each edge, and (since dispatch filters them out
       too) guarantees the XOR-extension below isn't fed a fact
       that's already folded into cur. */
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

            /* Dispatch only the useful part of the edge — the requests
               not already in cur's facts. The dispatched (req, resp)
               pairs are by construction disjoint from cur, so XOR-fold
               is a safe set extension. */
            auto useful = usefulDispatch(*requestSetOpt, curRequests);
            if (useful.empty())
                continue; // degenerate edge — all its requests already in cur

            Hash nextCur = cur;
            for (const auto & req : useful) {
                auto resp = dispatch(req);
                nextCur = dg_xorHash(nextCur, dg_factElementHash(req, resp));
            }

            /* Validate that some recording for THIS query reaches
               (Q, nextCur) — i.e., the dispatched responses lead to
               a position where the recording continues or terminates. */
            if (!hasAnyEdge(q, nextCur))
                continue; // wrong branch

            cur = nextCur;
            for (const auto & req : useful)
                curRequests.insert(req);
            advanced = true;
            break;
        }

        if (!advanced)
            return std::nullopt;
    }
}

} // namespace nix
