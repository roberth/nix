#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
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

CREATE TABLE IF NOT EXISTS Selectors (
    selectorHash BLOB PRIMARY KEY,
    payload   BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS Results (
    resultHash BLOB PRIMARY KEY,
    payload    BLOB NOT NULL
);

-- ObservationSet CAS pool: content-addressed sets of (selectorHash,
-- responseHash) tuples. Referenced from SelectorCallbackApply payloads
-- to identify the specific observations an outer callback made on
-- an inner-supplied contra-arg during one callback firing. Distinct
-- observation sets → distinct SelectorCallbackApply queryHashes →
-- distinct DB rows. Same set → same hash → shared row.
CREATE TABLE IF NOT EXISTS ObservationSet (
    setHash BLOB PRIMARY KEY,
    payload BLOB NOT NULL
) WITHOUT ROWID;

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
-- by (selectorHash, factSetHash), so a recording reaching some
-- intermediate position is detectable via "any Asks/Terminal row at
-- (Q, factSetHash)?". walk() maintains the current FactSet members
-- in-process.

CREATE TABLE IF NOT EXISTS RequestSetNodes (
    nodeHash BLOB PRIMARY KEY,
    payload  BLOB NOT NULL
) WITHOUT ROWID;

-- Decision graph layer: two edge tables, both keyed by (selectorHash, factSetHash).

-- No separate (selectorHash, factSetHash) index is needed: the primary
-- key prefix already covers WHERE-by-(selectorHash, factSetHash) lookups.
-- WITHOUT ROWID stores rows directly in the PK B-tree instead of in a
-- separate heap with a duplicate PK index — a ~50% reduction in
-- on-disk size for these all-blob, no-other-payload tables.
CREATE TABLE IF NOT EXISTS Ask (
    selectorHash      BLOB NOT NULL,
    factSetHash    BLOB NOT NULL,
    requestSetHash BLOB NOT NULL,
    -- Optional alternative requestset. Walker tries primary first;
    -- on fold reaching a dead-end, folds via alt and copies the
    -- discovered edge onto primary's fold target. See
    -- "state/observation-creep canonicalisation" in the main doc.
    altRequestSetHash BLOB,
    PRIMARY KEY (selectorHash, factSetHash, requestSetHash)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Terminal (
    selectorHash   BLOB NOT NULL,
    factSetHash BLOB NOT NULL,
    resultHash  BLOB NOT NULL,
    PRIMARY KEY (selectorHash, factSetHash, resultHash)
) WITHOUT ROWID;

-- Clean up indexes from earlier schema versions, if present.
DROP INDEX IF EXISTS AsksByQF;
DROP INDEX IF EXISTS TerminalsByQF;

-- Clean up SubjectEvolutionEdge from earlier schema versions (walker-
-- side per-subject observation trie replaced by a local `obs.fromHash
-- == cur` check in resolveIdentity's K > 0 loop).
DROP TABLE IF EXISTS SubjectEvolutionEdge;
)sql";

struct TracingDecisionGraph::State
{
    SQLite db;

    /* Storage layer */
    SQLiteStmt insertRequest, insertSelector, insertResult;
    SQLiteStmt selectRequest, selectSelector, selectResult;
    SQLiteStmt insertObservationSet, selectObservationSet;
    SQLiteStmt insertRequestSetNode;
    SQLiteStmt selectRequestSetNode;
    SQLiteStmt countAsks, countTerminals;

    /* Decision graph layer */
    SQLiteStmt insertAsk, selectAsks, deleteAsks;
    SQLiteStmt insertTerminal, selectTerminal;

    /* (subject-evolution fast-path) — populated by cold's
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

Hash TracingDecisionGraph::xorHashes(const Hash & a, const Hash & b)
{
    return dg_xorHash(a, b);
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
    /* Pool routes intern/find through this graph's DB backing. */
    selectorPool.bind(*this);

    auto state(_state->lock());

    auto parent = dbPath.parent_path();
    if (!parent.empty())
        createDirs(parent);

    state->db = SQLite(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
    state->db.isCache();
    state->db.exec(decisionGraphSchema);

    state->insertRequest.create(state->db,
        "INSERT OR IGNORE INTO Requests(requestHash, payload) VALUES (?, ?)");
    state->insertSelector.create(state->db,
        "INSERT OR IGNORE INTO Selectors(selectorHash, payload) VALUES (?, ?)");
    state->insertResult.create(state->db,
        "INSERT OR IGNORE INTO Results(resultHash, payload) VALUES (?, ?)");
    state->selectRequest.create(state->db,
        "SELECT payload FROM Requests WHERE requestHash = ?");
    state->selectSelector.create(state->db,
        "SELECT payload FROM Selectors WHERE selectorHash = ?");
    state->selectResult.create(state->db,
        "SELECT payload FROM Results WHERE resultHash = ?");
    state->insertObservationSet.create(state->db,
        "INSERT OR IGNORE INTO ObservationSet(setHash, payload) VALUES (?, ?)");
    state->selectObservationSet.create(state->db,
        "SELECT payload FROM ObservationSet WHERE setHash = ?");
    /* Drop obsolete tables from earlier schema versions. */
    state->db.exec("DROP TABLE IF EXISTS FactSets;");
    state->db.exec("DROP TABLE IF EXISTS EdgeResponses;");
    state->db.exec("DROP TABLE IF EXISTS SubjectStampSites;");
    state->db.exec("DROP TABLE IF EXISTS ApplyResultProducers;");

    state->insertRequestSetNode.create(state->db,
        "INSERT OR IGNORE INTO RequestSetNodes(nodeHash, payload) VALUES (?, ?)");
    state->selectRequestSetNode.create(state->db,
        "SELECT payload FROM RequestSetNodes WHERE nodeHash = ?");

    /* Drop the previous flat-blob RequestSets table from earlier
       schema versions if present (incompatible payload format). */
    state->db.exec("DROP TABLE IF EXISTS RequestSets;");
    /* Drop the obsolete InnerValueResponse table — callback-arg probe
       responses now live in the ObservationSet CAS via each
       SelectorCallbackApply's `argObsSet`. */
    state->db.exec("DROP TABLE IF EXISTS InnerValueResponse;");

    /* Idempotent ALTER for pre-existing dev DBs that predate the
       altRequestSetHash column. Newly created DBs already have it from
       the CREATE TABLE above. */
    try {
        state->db.exec("ALTER TABLE Ask ADD COLUMN altRequestSetHash BLOB");
    } catch (SQLiteError &) {
        /* Column already exists (idempotent re-run). */
    }
    state->insertAsk.create(state->db,
        "INSERT OR IGNORE INTO Ask(selectorHash, factSetHash, requestSetHash, altRequestSetHash) VALUES (?, ?, ?, ?)");
    state->selectAsks.create(state->db,
        "SELECT requestSetHash, altRequestSetHash FROM Ask WHERE selectorHash = ? AND factSetHash = ?");
    state->deleteAsks.create(state->db,
        "DELETE FROM Ask WHERE selectorHash = ? AND factSetHash = ? AND requestSetHash = ?");
    state->insertTerminal.create(state->db,
        "INSERT OR IGNORE INTO Terminal(selectorHash, factSetHash, resultHash) VALUES (?, ?, ?)");
    state->selectTerminal.create(state->db,
        "SELECT resultHash FROM Terminal WHERE selectorHash = ? AND factSetHash = ?");
    state->countAsks.create(state->db,
        "SELECT 1 FROM Ask WHERE selectorHash = ? AND factSetHash = ? LIMIT 1");
    state->countTerminals.create(state->db,
        "SELECT 1 FROM Terminal WHERE selectorHash = ? AND factSetHash = ? LIMIT 1");
}

TracingDecisionGraph::~TracingDecisionGraph() = default;

void TracingDecisionGraph::waitForWrites()
{
    /* Synchronous writes for Phase 1; nothing to wait for. */
}

/* ─────────────────────────────────────────────────────────────────────
   Storage layer: atoms
   ───────────────────────────────────────────────────────────────────── */

/* The payload here is raw CBOR — not UTF-8-safe, so we don't
   embed it verbatim in provenance details (nlohmann's JSON
   serializer will throw on non-UTF-8 bytes). Callers who have
   the pre-encoded JSON form should record provenance at their
   own level with the JSON payload. This macro just records the
   kind + size. */
#define ATOM_INSERT_CACHED(NAME, CACHE)                                          \
    void TracingDecisionGraph::insert##NAME(const Hash & h, std::string_view p) \
    {                                                                            \
        {                                                                        \
            auto state(_state->lock());                                          \
            auto use = state->insert##NAME.use();                                \
            dg_bindBlob(use, dg_hashToBlob(h));                                  \
            dg_bindBlob(use, p);                                                 \
            use.exec();                                                          \
            /* Mirror INSERT OR IGNORE: only the first payload wins. */          \
            state->CACHE.try_emplace(h, std::optional{std::string(p)});          \
        }                                                                        \
        if (provenanceEnabled())                                                 \
            recordProvenance(h, #NAME "Hash",                                    \
                             {{"payload_len", p.size()}});                       \
    }

#define ATOM_INSERT_PLAIN(NAME)                                                  \
    void TracingDecisionGraph::insert##NAME(const Hash & h, std::string_view p) \
    {                                                                            \
        {                                                                        \
            auto state(_state->lock());                                          \
            auto use = state->insert##NAME.use();                                \
            dg_bindBlob(use, dg_hashToBlob(h));                                  \
            dg_bindBlob(use, p);                                                 \
            use.exec();                                                          \
        }                                                                        \
        if (provenanceEnabled())                                                 \
            recordProvenance(h, #NAME "Hash",                                    \
                             {{"payload_len", p.size()}});                       \
    }

ATOM_INSERT_CACHED(Request, requestPayloadCache)
ATOM_INSERT_PLAIN(Selector)
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
ATOM_GET_PLAIN(Selector)
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

/* ObservationSet CAS pool. Flat CBOR payload of sorted-dedup member
   list; hash = SHA-256 of payload. Distinct from RequestSet's trie
   because expected member counts are small (per callback firing),
   and lookup is by whole-set hash — no set-difference operations
   needed here. */
static std::string dg_observationSetPayload(
    const std::vector<TracingDecisionGraph::Observation> & sortedMembers)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto & m : sortedMembers) {
        /* Response payload embedded as a JSON binary value —
           nlohmann::json's CBOR encoder handles it natively as a
           byte string. Preserves exact CBOR bytes without any hex
           encoding overhead. */
        arr.push_back({
            {"q", m.selectorHash.to_string(HashFormat::Base16, false)},
            {"p", nlohmann::json::binary(std::vector<std::uint8_t>(
                m.responsePayload.begin(), m.responsePayload.end()))},
        });
    }
    auto cbor = nlohmann::json::to_cbor(arr);
    return std::string(reinterpret_cast<const char *>(cbor.data()), cbor.size());
}

Hash TracingDecisionGraph::computeObservationSetHash(
    std::vector<TracingDecisionGraph::Observation> members)
{
    auto sorted = dg_sortAndDedup(std::move(members));
    return hashString(HashAlgorithm::SHA256, dg_observationSetPayload(sorted));
}

Hash TracingDecisionGraph::insertObservationSet(
    std::vector<TracingDecisionGraph::Observation> members)
{
    auto sorted = dg_sortAndDedup(std::move(members));
    auto payload = dg_observationSetPayload(sorted);
    auto h = hashString(HashAlgorithm::SHA256, payload);
    {
        auto state(_state->lock());
        auto use = state->insertObservationSet.use();
        dg_bindBlob(use, dg_hashToBlob(h));
        dg_bindBlob(use, payload);
        use.exec();
    }
    return h;
}

std::optional<std::vector<TracingDecisionGraph::Observation>>
TracingDecisionGraph::getObservationSet(const Hash & h)
{
    std::optional<std::string> payload;
    {
        auto state(_state->lock());
        auto query = state->selectObservationSet.use();
        dg_bindBlob(query, dg_hashToBlob(h));
        if (query.next())
            payload = query.getBlob(0);
    }
    if (!payload)
        return std::nullopt;
    auto bytes = reinterpret_cast<const uint8_t *>(payload->data());
    auto arr = nlohmann::json::from_cbor(bytes, bytes + payload->size());
    std::vector<Observation> members;
    members.reserve(arr.size());
    for (const auto & elt : arr) {
        Observation m;
        m.selectorHash = Hash::parseNonSRIUnprefixed(elt.at("q").get<std::string>(), HashAlgorithm::SHA256);
        auto & binVal = elt.at("p").get_binary();
        m.responsePayload.assign(binVal.begin(), binVal.end());
        members.push_back(std::move(m));
    }
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
    const std::unordered_set<RequestHash> & dispatchedSoFar)
{
    std::vector<RequestHash> out;
    out.reserve(edgeRequestSet.size());
    for (const auto & req : edgeRequestSet)
        if (!dispatchedSoFar.count(req))
            out.push_back(req);
    return out;
}

bool TracingDecisionGraph::isApplyRequest(const RequestHash & h)
{
    auto payload = getRequestPayload(h);
    if (!payload)
        return false;
    try {
        auto bytes = reinterpret_cast<const uint8_t *>(payload->data());
        auto js = nlohmann::json::from_cbor(bytes, bytes + payload->size());
        return js.contains("tag") && js["tag"].is_string()
            && js["tag"].get<std::string>() == "apply";
    } catch (...) {
        return false;
    }
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
       caller (record / history) can still inspect them within one
       invocation. */
    auto canonical = dg_sortAndDedup(std::move(members));
    SetHash setHash = emptySetHash();
    for (const auto & f : canonical)
        setHash = dg_xorHash(setHash, dg_factElementHash(f.request, f.response));
    auto state(_state->lock());
    state->factSetCache.try_emplace(setHash, std::optional{std::move(canonical)});
    return setHash;
}

void TracingDecisionGraph::installFactSet(
    const SetHash & hash, const std::vector<Fact> & members)
{
    auto state(_state->lock());
    /* Always store the latest snapshot — the caller's growing envFactSet
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

void TracingDecisionGraph::persistRequestSetNode(
    const Hash & nodeHash, std::string_view payload)
{
    auto state(_state->lock());
    auto [it, inserted] = state->requestSetNodePayloadCache.try_emplace(
        nodeHash, std::optional<std::string>{std::string(payload)});
    if (!inserted)
        return;
    auto use = state->insertRequestSetNode.use();
    dg_bindBlob(use, dg_hashToBlob(nodeHash));
    dg_bindBlob(use, payload);
    use.exec();
}

/* ──────────────────────────────────────────────────────────────────────
   TrieBuilder — incremental in-memory RequestSet trie
   ────────────────────────────────────────────────────────────────────── */

struct TracingDecisionGraph::TrieBuilder::Node
{
    bool isLeaf = true;
    std::vector<Hash> leafMembers; // sorted while isLeaf
    std::array<std::unique_ptr<Node>, TRIE_RADIX> children;
    std::optional<Hash> cachedHash;
    bool persisted = false;

    /* Build the payload bytes for this node's current state. */
    std::string buildPayload()
    {
        if (isLeaf)
            return dg_trieLeafPayload(leafMembers);
        std::vector<std::pair<uint8_t, Hash>> kids;
        kids.reserve(TRIE_RADIX);
        for (uint8_t i = 0; i < TRIE_RADIX; ++i)
            if (children[i])
                kids.emplace_back(i, children[i]->ensureHash());
        return dg_trieInternalPayload(kids);
    }

    /* Compute (or recompute) this node's hash, recursing into dirty
       children. cachedHash is populated; returns the hash. */
    Hash ensureHash()
    {
        if (cachedHash)
            return *cachedHash;
        cachedHash = hashString(HashAlgorithm::SHA256, buildPayload());
        return *cachedHash;
    }

    /* Insert a hash into this subtree at the given trie depth.
       Invalidates cachedHash and persisted flag along the affected
       path. */
    void insertAtDepth(const Hash & h, int depth)
    {
        cachedHash.reset();
        persisted = false;
        if (isLeaf) {
            auto pos = std::lower_bound(leafMembers.begin(), leafMembers.end(), h);
            if (pos != leafMembers.end() && *pos == h)
                return; // duplicate; nothing to do
            leafMembers.insert(pos, h);
            if (leafMembers.size() > TRIE_SPLIT_THRESHOLD) {
                /* Split: convert leaf to internal, redistribute. */
                std::vector<Hash> oldMembers;
                oldMembers.swap(leafMembers);
                isLeaf = false;
                for (const auto & m : oldMembers) {
                    auto bucket = dg_bucketAt(m, depth);
                    if (!children[bucket])
                        children[bucket] = std::make_unique<Node>();
                    children[bucket]->insertAtDepth(m, depth + 1);
                }
            }
        } else {
            auto bucket = dg_bucketAt(h, depth);
            if (!children[bucket])
                children[bucket] = std::make_unique<Node>();
            children[bucket]->insertAtDepth(h, depth + 1);
        }
    }

    /* Recursively persist this and any unpersisted subtrees. */
    void persistTree(TracingDecisionGraph & g)
    {
        if (persisted)
            return;
        if (!isLeaf) {
            for (auto & c : children)
                if (c)
                    c->persistTree(g);
        }
        auto payload = buildPayload();
        auto hash = cachedHash.value_or(hashString(HashAlgorithm::SHA256, payload));
        cachedHash = hash;
        g.persistRequestSetNode(hash, payload);
        persisted = true;
    }

};


TracingDecisionGraph::TrieBuilder::TrieBuilder()
    : root(std::make_unique<Node>())
{
}

TracingDecisionGraph::TrieBuilder::~TrieBuilder() = default;

void TracingDecisionGraph::TrieBuilder::insert(const Hash & request)
{
    root->insertAtDepth(request, 0);
}

Hash TracingDecisionGraph::TrieBuilder::rootHash()
{
    return root->ensureHash();
}

void TracingDecisionGraph::TrieBuilder::persist(TracingDecisionGraph & g)
{
    root->persistTree(g);
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

void TracingDecisionGraph::insertAsk(
    const QueryHash & q,
    const SetHash & factSet,
    const SetHash & requestSet,
    const std::optional<SetHash> & altRequestSet)
{
    auto state(_state->lock());
    auto use = state->insertAsk.use();
    dg_bindBlob(use, dg_hashToBlob(q));
    dg_bindBlob(use, dg_hashToBlob(factSet));
    dg_bindBlob(use, dg_hashToBlob(requestSet));
    if (altRequestSet)
        dg_bindBlob(use, dg_hashToBlob(*altRequestSet));
    else
        use.bind(); // NULL
    use.exec();
}

std::vector<TracingDecisionGraph::AskEdge>
TracingDecisionGraph::getAsks(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto query = state->selectAsks.use();
    dg_bindBlob(query, dg_hashToBlob(q));
    dg_bindBlob(query, dg_hashToBlob(factSet));
    std::vector<AskEdge> out;
    while (query.next()) {
        AskEdge e{dg_blobToHash(query.getBlob(0)), std::nullopt};
        if (!query.isNull(1))
            e.altRequestSet = dg_blobToHash(query.getBlob(1));
        out.push_back(std::move(e));
    }
    return out;
}

void TracingDecisionGraph::removeAsk(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto use = state->deleteAsks.use();
    dg_bindBlob(use, dg_hashToBlob(q));
    dg_bindBlob(use, dg_hashToBlob(factSet));
    dg_bindBlob(use, dg_hashToBlob(requestSet));
    use.exec();
}

void TracingDecisionGraph::insertAskSplitting(
    const QueryHash & q,
    const SetHash & cur_in,
    const std::vector<Fact> & facts_in,
    const std::unordered_set<RequestHash> & dispatchedSoFar,
    const std::optional<SetHash> & alt_in)
{
    auto cur = cur_in;
    auto remaining = facts_in;
    /* Preserve the caller's alt only for a landing without split; on
       any split the alt is dropped (the row's identity changes). */
    auto alt = alt_in;

    struct SplitStep
    {
        Hash newCur;
        std::vector<Fact> remainingAfterConsume;
    };

    /* trySplitOne: find an overlap between remaining and an existing
       edge at (q, cur), execute the split, return the advanced (cur,
       remaining). Returns nullopt when no overlap is found (caller
       inserts the remainder as a plain edge). */
    auto trySplitOne =
        [&](const SetHash & cur, const std::vector<Fact> & remaining) -> std::optional<SplitStep>
    {
        std::unordered_set<Hash> remainingReqs;
        remainingReqs.reserve(remaining.size());
        for (const auto & f : remaining) remainingReqs.insert(f.request);

        for (const auto & edge : getAsks(q, cur)) {
            auto exRsHash = edge.requestSet;
            auto exMembers = getRequestSet(exRsHash);
            if (!exMembers)
                continue;
            /* Filter existing rs against dispatchedSoFar to avoid double-
               XOR when computing the intermediate cur. */
            auto exUseful = usefulDispatch(*exMembers, dispatchedSoFar);
            if (exUseful.empty())
                continue;

            std::vector<Hash> shared;
            shared.reserve(exUseful.size());
            for (const auto & req : exUseful)
                if (remainingReqs.count(req))
                    shared.push_back(req);
            if (shared.empty())
                continue;

            /* Full identity: existing rs is exactly remaining. Nothing
               to insert; signal completion by returning empty remaining. */
            if (shared.size() == exUseful.size() && shared.size() == remaining.size())
                return SplitStep{cur, {}};

            /* Fold shared facts into cur → intermediate; partition
               remaining into consumed vs tailNew. */
            std::unordered_set<Hash> sharedSet(shared.begin(), shared.end());
            Hash intermediate = cur;
            std::vector<Fact> tailNew;
            tailNew.reserve(remaining.size() - shared.size());
            for (const auto & f : remaining) {
                if (sharedSet.count(f.request))
                    intermediate = dg_xorHash(intermediate, dg_factElementHash(f.request, f.response));
                else
                    tailNew.push_back(f);
            }

            auto sharedRsHash = insertRequestSet(shared);

            /* Split shape:
               - shared < exUseful: existing must be re-anchored — insert
                 sharedRsHash at cur, tail (ex_rs \ shared) at
                 intermediate, remove original at cur. The tail must be
                 exactly ex_rs \ shared: storing the whole ex_rs at
                 intermediate would let a walker whose dispatchedSoFar
                 doesn't contain `shared` dispatch and XOR-fold the shared
                 reqs a second time, cancelling them out of cur.
               - shared == exUseful: existing IS the shared prefix;
                 sharedRsHash dedups against it. No re-anchor. */
            if (shared.size() != exUseful.size()) {
                std::vector<Hash> tail;
                tail.reserve(exUseful.size() - shared.size());
                for (const auto & req : exUseful)
                    if (!sharedSet.count(req))
                        tail.push_back(req);
                auto tailRsHash = insertRequestSet(tail);
                insertAsk(q, cur, sharedRsHash);
                insertAsk(q, intermediate, tailRsHash, edge.altRequestSet);
                removeAsk(q, cur, exRsHash);
            } else {
                insertAsk(q, cur, sharedRsHash);
            }

            tracingCacheLog(
                "insertAskSplitting Q=%s split at cur=%s: shared=%zu, exUseful=%zu, newRemaining=%zu "
                "(intermediate=%s, sharedRS=%s, exRS=%s)",
                q.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                cur.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                shared.size(), exUseful.size(), remaining.size(),
                intermediate.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                sharedRsHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                exRsHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str());

            return SplitStep{intermediate, std::move(tailNew)};
        }
        return std::nullopt;
    };

    /* Iterate split steps until no overlap remains. Each successful
       split drops alt (the row's identity changed). */
    while (auto step = trySplitOne(cur, remaining)) {
        cur = step->newCur;
        remaining = std::move(step->remainingAfterConsume);
        alt = std::nullopt;
    }

    /* No split available at this cur. Insert whatever's left as a plain
       edge, preserving alt. Empty remaining = nothing to insert. */
    if (remaining.empty())
        return;
    std::vector<Hash> newReqs;
    newReqs.reserve(remaining.size());
    for (const auto & f : remaining) newReqs.push_back(f.request);
    auto newRsHash = insertRequestSet(newReqs);
    insertAsk(q, cur, newRsHash, alt);
}

void TracingDecisionGraph::copyOutgoing(
    const QueryHash & q, const SetHash & srcCur, const SetHash & dstCur)
{
    if (srcCur == dstCur)
        return;
    auto edges = getAsks(q, srcCur);
    for (const auto & e : edges)
        insertAsk(q, dstCur, e.requestSet, e.altRequestSet);
    if (auto term = getTerminal(q, srcCur))
        insertTerminal(q, dstCur, *term);
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
   either; it tracks a local dispatchedSoFar for "what I've consumed
   so far". remaining-as-set = allRequests \ dispatchedSoFar. */
static void dg_recordImpl(
    TracingDecisionGraph & g,
    const Hash & q,
    const Hash & factSetHash,
    const Hash & result,
    const std::unordered_map<Hash, Hash> & responseFor,
    const std::unordered_set<Hash> & allRequests,
    const Hash * sessionRequestsRsHash = nullptr,
    Hash startFactSetHash = TracingDecisionGraph::emptySetHash())
{
    auto cur = startFactSetHash;
    std::unordered_set<Hash> dispatchedSoFar;

    auto isInRemaining = [&](const Hash & req) {
        return allRequests.count(req) && !dispatchedSoFar.count(req);
    };

    auto extendCur = [&](const std::vector<Hash> & reqs) {
        for (const auto & req : reqs) {
            assert(!dispatchedSoFar.count(req));
            auto it = responseFor.find(req);
            assert(it != responseFor.end());
            cur = dg_xorHash(cur, dg_factElementHash(req, it->second));
            dispatchedSoFar.insert(req);
        }
    };

    auto curExtendedBy = [&](const std::vector<Hash> & reqs) -> Hash {
        Hash h = cur;
        for (const auto & req : reqs) {
            assert(!dispatchedSoFar.count(req));
            auto it = responseFor.find(req);
            assert(it != responseFor.end());
            h = dg_xorHash(h, dg_factElementHash(req, it->second));
        }
        return h;
    };

    while (dispatchedSoFar.size() < allRequests.size()) {
        /* Patricia split is now handled inside insertAskSplitting
           (called on the fallback insert below). No separate eager
           pass here — followUseful handles the discovery-of-existing
           optimisation, and any residual Ask insert splits against
           existing at the same cur. */

        std::optional<std::vector<Hash>> followUseful;
        for (const auto & edge : g.getAsks(q, cur)) {
            auto rsHash = edge.requestSet;
            auto rsMembers = g.getRequestSet(rsHash);
            if (!rsMembers)
                continue;
            auto useful = TracingDecisionGraph::usefulDispatch(*rsMembers, dispatchedSoFar);
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
            /* Build the (req, resp) facts for the current remaining and
               route through insertAskSplitting so any partial overlap
               with an existing Ask at (q, cur) gets factored via
               Patricia split. The fast-path fold cur → factSetHash
               remains, but we can't jump straight to it any more —
               splitting may distribute the insert across multiple curs.
               (Fast-path re-optimisation is a follow-up.) */
            std::vector<TracingDecisionGraph::Fact> remainingFacts;
            remainingFacts.reserve(allRequests.size() - dispatchedSoFar.size());
            std::vector<Hash> remainingVec;
            remainingVec.reserve(allRequests.size() - dispatchedSoFar.size());
            for (const auto & req : allRequests)
                if (!dispatchedSoFar.count(req)) {
                    auto it = responseFor.find(req);
                    assert(it != responseFor.end());
                    remainingFacts.push_back({req, it->second});
                    remainingVec.push_back(req);
                }
            (void) sessionRequestsRsHash; // fast-path shortcut retired
            g.insertAskSplitting(q, cur, remainingFacts, dispatchedSoFar);
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
    const std::unordered_set<Hash> & allRequests,
    SetHash startFactSetHash)
{
    dg_recordImpl(*this, q, factSetHash, result, responseFor, allRequests, nullptr, startFactSetHash);
}

void TracingDecisionGraph::record(
    const QueryHash & q,
    const SetHash & factSetHash,
    const ResultHash & result,
    const std::unordered_map<Hash, Hash> & responseFor,
    const std::unordered_set<Hash> & allRequests,
    const SetHash & sessionRequestsRsHash,
    SetHash startFactSetHash)
{
    dg_recordImpl(*this, q, factSetHash, result, responseFor, allRequests, &sessionRequestsRsHash, startFactSetHash);
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

std::optional<TracingDecisionGraph::WalkHit> TracingDecisionGraph::walk(
    const QueryHash & q_initial,
    const std::function<ResponseHash(const RequestHash &, const EdgeContext &)> & dispatch,
    const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt,
    const SetHash & startCur,
    const std::function<QueryHash(const QueryHash & preFoldQ)> & recomputeQ)
{
    Hash q = q_initial;
    auto cur = startCur;
    /* dispatchedSoFar speeds up the "is this request already in cur?"
       filter on each edge, and (since dispatch filters them out
       too) guarantees the XOR-extension below isn't fed a fact
       that's already folded into cur. */
    std::unordered_set<RequestHash> dispatchedSoFar;
    tracingCacheLog("history Q=%s startCur=%s",
                    q.to_string(HashFormat::Base16, false).substr(0, 12),
                    cur.to_string(HashFormat::Base16, false).substr(0, 12));
    for (;;) {
        if (auto term = getTerminal(q, cur)) {
            tracingCacheLog("history Q=%s TERMINAL at cur=%s",
                            q.to_string(HashFormat::Base16, false).substr(0, 12),
                            cur.to_string(HashFormat::Base16, false).substr(0, 12));
            return WalkHit{*term, cur};
        }

        auto outgoing = getAsks(q, cur);
        if (outgoing.empty()) {
            tracingCacheLog("history Q=%s NO OUTGOING at cur=%s -> miss",
                            q.to_string(HashFormat::Base16, false).substr(0, 12),
                            cur.to_string(HashFormat::Base16, false).substr(0, 12));
            return std::nullopt; // no path forward, no terminal
        }

        tracingCacheLog("history Q=%s cur=%s outgoing=%zu",
                        q.to_string(HashFormat::Base16, false).substr(0, 12),
                        cur.to_string(HashFormat::Base16, false).substr(0, 12),
                        outgoing.size());

        bool advanced = false;
        /* Try each outgoing edge. Each edge has a primary requestSet
           and an optional alt. Attempt primary first; on miss, try
           alt as a one-shot fallback (see main doc's
           "state/observation-creep canonicalisation" note, walk side).
           Returns the (usefulReqs, nextCur) that were folded, plus a
           bool committed. */
        auto tryDispatchRs = [&](
            const SetHash & rsHash,
            std::vector<Hash> * outUseful,
            Hash * outNextCur) -> bool
        {
            auto rsOpt = getRequestSet(rsHash);
            if (!rsOpt)
                return false;
            auto useful = usefulDispatch(*rsOpt, dispatchedSoFar);
            if (useful.empty())
                return false;
            Hash nextCur = cur;
            EdgeContext edgeCtx{q, cur, rsHash};
            for (const auto & req : useful) {
                auto resp = dispatch(req, edgeCtx);
                nextCur = dg_xorHash(nextCur, dg_factElementHash(req, resp));
            }
            *outUseful = std::move(useful);
            *outNextCur = nextCur;
            return true;
        };

        for (const auto & edge : outgoing) {
            auto requestSetHash = edge.requestSet;

            std::vector<Hash> useful;
            Hash nextCur{HashAlgorithm::SHA256};
            if (!tryDispatchRs(requestSetHash, &useful, &nextCur))
                continue;

            /* Trie navigation per design (see
               `doc/design/tracing-eval-cache.md`, "Walk from ∅",
               step 3): "Validate: hasAnyEdge(selectorHash, nextCur)? ...
               If yes, advance cur = nextCur and continue. If no,
               this branch of the recording isn't reachable from
               the current env — try the next outgoing edge."

               Failed candidates never advance cur — no state is
               undone. Single-edge nodes resolve in one step;
               multi-edge nodes exist because v13 preserves
               cross-session merges (Patricia split) without a
               session-tag column on Ask rows. Under lockstep the
               dispatched Requests are idempotent (same request →
               same response), so re-dispatch across candidates is
               observationally identical to a single dispatch.

               No stored-response substitution: a stored-vs-live
               mismatch is the walker's only signal for legitimate
               env change (tested by cb-with-scope-and-tryeval and
               cb-list-args under DISALLOW-mode expected-error
               semantics). If walker-side computation collapses
               distinct siblings onto the same nextCur, the fix
               belongs on the subject-id side — not by papering
               over the divergence with a stored response. */
            if (!hasAnyEdge(q, nextCur)) {
                tracingCacheLog("history Q=%s rs=%s useful=%zu nextCur=%s NO RECORDED EDGE -> try alt/next",
                                q.to_string(HashFormat::Base16, false).substr(0, 12),
                                requestSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                                useful.size(),
                                nextCur.to_string(HashFormat::Base16, false).substr(0, 12));
                if (onEdgeAttempt)
                    onEdgeAttempt(/*committed=*/ false, useful);

                /* One-shot alt-fallback: if the primary's fold lands
                   at a dead-end but the row carries an altRequestSet,
                   try that. On alt hit, copy the discovered outgoing
                   state at nextCur_alt onto nextCur_primary so future
                   walks reach it via primary directly. */
                if (edge.altRequestSet) {
                    std::vector<Hash> altUseful;
                    Hash altNextCur{HashAlgorithm::SHA256};
                    if (tryDispatchRs(*edge.altRequestSet, &altUseful, &altNextCur)
                        && hasAnyEdge(q, altNextCur))
                    {
                        tracingCacheLog(
                            "history Q=%s ALT-FALLBACK altRs=%s useful=%zu cur=%s -> altNextCur=%s (copying to primary %s)",
                            q.to_string(HashFormat::Base16, false).substr(0, 12),
                            edge.altRequestSet->to_string(HashFormat::Base16, false).substr(0, 12),
                            altUseful.size(),
                            cur.to_string(HashFormat::Base16, false).substr(0, 12),
                            altNextCur.to_string(HashFormat::Base16, false).substr(0, 12),
                            nextCur.to_string(HashFormat::Base16, false).substr(0, 12));
                        copyOutgoing(q, altNextCur, nextCur);
                        useful = std::move(altUseful);
                        nextCur = altNextCur;
                        /* Fall through to the commit path below. */
                    } else {
                        continue; // alt didn't help either
                    }
                } else {
                    continue; // no alt on this edge
                }
            }

            tracingCacheLog("history Q=%s rs=%s useful=%zu cur=%s -> nextCur=%s",
                            q.to_string(HashFormat::Base16, false).substr(0, 12),
                            requestSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                            useful.size(),
                            cur.to_string(HashFormat::Base16, false).substr(0, 12),
                            nextCur.to_string(HashFormat::Base16, false).substr(0, 12));
            cur = nextCur;
            for (const auto & req : useful) {
                /* Env-layer requests are stable (same request → same
                   response), so once dispatched they don't need
                   re-dispatch. */
                if (!isApplyRequest(req))
                    dispatchedSoFar.insert(req);
            }
            if (onEdgeAttempt)
                onEdgeAttempt(/*committed=*/ true, useful);
            /* Task #110 Q-evolution: after the edge commits (walker's
               envWalk has grown via onEdgeAttempt), let the caller
               re-derive Q based on the new envWalk state. If Q
               evolved, subsequent Terminal/Ask lookups use the new Q,
               matching the writer's per-observation Q-evolution
               protocol. */
            if (recomputeQ) {
                auto newQ = recomputeQ(q);
                if (newQ != q) {
                    tracingCacheLog("history Q evolved %s -> %s at cur=%s",
                                    q.to_string(HashFormat::Base16, false).substr(0, 12),
                                    newQ.to_string(HashFormat::Base16, false).substr(0, 12),
                                    cur.to_string(HashFormat::Base16, false).substr(0, 12));
                    q = newQ;
                }
            }
            advanced = true;
            break;
        }

        if (!advanced) {
            tracingCacheLog("history Q=%s NO EDGE COMMITTED at cur=%s -> miss",
                            q.to_string(HashFormat::Base16, false).substr(0, 12),
                            cur.to_string(HashFormat::Base16, false).substr(0, 12));
            return std::nullopt;
        }
    }
}

} // namespace nix
