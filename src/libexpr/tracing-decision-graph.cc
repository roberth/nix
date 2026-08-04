#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/request-set-trie.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/store/sqlite.hh"
#include <sqlite3.h>
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace nix {

/* ---- Async writer thread (revived from v12 #1592361d6 + follow-ups) ----

   The DG's insert paths pay per-statement SQLite transaction overhead
   (~7% of cycles in the profile: btreeBeginTrans + VdbeHalt on every
   INSERT OR IGNORE). Batching writes into transactions collapses this.

   Shape (following the mature v12 pattern matured across #27ad15029,
   #52eafc558, #1b52d14d3, #f25c31f06):
   - WriteOp variants describe each queued insert.
   - WriteQueue owns a std::thread with its own SQLite connection.
   - Writer batches all pending ops into one SQLiteTxn, then PRAGMA
     wal_checkpoint(PASSIVE) so other in-process connections see the data.
   - Registry + std::atexit(WriteQueue::flushAll) to drain queued
     writes at process exit even when EvalState (via Boehm GC) skips
     destructors.
   - Readers use in-memory caches (payload caches on the atom layer,
     askEdgesCache + terminalCache on the DG layer) as the authoritative
     in-session view. On a cache miss the reader loads from DB (running
     a PASSIVE checkpoint first so writer-committed data is visible on
     the reader's connection). Readers never block on the writer
     thread — the ordering constraint is that writes update the cache
     synchronously before enqueueing.
   - waitForWrites (public) drains the queue for cross-process
     visibility. Not used on any read path. */

namespace {

struct WriteInsertRequest        { TracingHash hash; std::string payload; };
struct WriteInsertSelector       { TracingHash hash; std::string payload; };
struct WriteInsertResult         { TracingHash hash; std::string payload; };
struct WriteInsertObservationSet { TracingHash hash; std::string payload; };
struct WriteInsertRequestSetNode { TracingHash hash; std::string payload; };
struct WriteInsertAsk {
    TracingHash selectorHash;
    TracingHash factSetHash;
    TracingHash requestSetHash;
    std::optional<TracingHash> altRequestSetHash;
};
struct WriteInsertTerminal {
    TracingHash selectorHash;
    TracingHash factSetHash;
    TracingHash resultHash;
};
struct WriteDeleteAsk {
    TracingHash selectorHash;
    TracingHash factSetHash;
    TracingHash requestSetHash;
};

using WriteOp = std::variant<
    WriteInsertRequest,
    WriteInsertSelector,
    WriteInsertResult,
    WriteInsertObservationSet,
    WriteInsertRequestSetNode,
    WriteInsertAsk,
    WriteInsertTerminal,
    WriteDeleteAsk>;

static std::string hashToBlob(const TracingHash & h)
{
    return std::string(reinterpret_cast<const char *>(h.bytes.data()), TracingHash::size);
}

static void bindHashBlob(SQLiteStmt::Use & use, const TracingHash & h)
{
    auto blob = hashToBlob(h);
    use(reinterpret_cast<const unsigned char *>(blob.data()), blob.size());
}

static void bindBytesBlob(SQLiteStmt::Use & use, std::string_view p)
{
    use(reinterpret_cast<const unsigned char *>(p.data()), p.size());
}

class WriteQueue
{
public:
    Sync<std::vector<WriteOp>> pending;
    std::condition_variable wakeup;
    std::thread thread;
    std::atomic<bool> done{false};

    /* Drain tracking: enqueueCount counts ops as they're handed off,
       processCount is bumped by the writer thread once a batch has
       been committed to disk. waitDrained snapshots enqueueCount and
       waits until processCount catches up — does NOT join the thread,
       so the WriteQueue remains usable afterwards. */
    std::atomic<uint64_t> enqueueCount{0};
    std::atomic<uint64_t> processCount{0};
    std::mutex drainMutex;
    std::condition_variable drainWakeup;

    /* Global registry so std::atexit can flush all active WriteQueues.
       Needed because EvalState (which owns TracingDecisionGraph via
       CacheState) uses Boehm GC — its destructor may never run before
       process exit, so we can't rely on ~WriteQueue draining. */
    static Sync<std::set<WriteQueue *>> & registry()
    {
        /* Intentionally leaked: registry must outlive every other
           static and every atexit handler, including flushAll. A
           function-local static's destructor could run before flushAll
           and leave it iterating a destroyed set. */
        static auto * instance = new Sync<std::set<WriteQueue *>>;
        return *instance;
    }

    static void flushAll()
    {
        auto queues = registry().lock();
        for (auto * q : *queues)
            q->shutdown();
        queues->clear();
    }

    WriteQueue(std::filesystem::path dbPath)
    {
        static std::once_flag atexitRegistered;
        std::call_once(atexitRegistered, [] { std::atexit(flushAll); });
        thread = std::thread([this, dbPath = std::move(dbPath)] { run(dbPath); });
        registry().lock()->insert(this);
    }

    ~WriteQueue()
    {
        shutdown();
        registry().lock()->erase(this);
    }

    void shutdown()
    {
        if (!done.exchange(true)) {
            wakeup.notify_one();
            if (thread.joinable())
                thread.join();
            {
                std::lock_guard<std::mutex> lock(drainMutex);
            }
            drainWakeup.notify_all();
        }
    }

    void enqueue(WriteOp op)
    {
        enqueueCount.fetch_add(1);
        {
            auto q = pending.lock();
            q->push_back(std::move(op));
        }
        wakeup.notify_one();
    }

    /* Wait until every op enqueued before this call has been committed
       by the writer thread. Does not join the thread; subsequent
       enqueues continue to work. */
    void waitDrained()
    {
        uint64_t target = enqueueCount.load();
        std::unique_lock<std::mutex> lock(drainMutex);
        drainWakeup.wait(lock, [&] {
            return processCount.load() >= target || done.load();
        });
    }

private:
    void run(const std::filesystem::path & dbPath)
    {
        try {
            SQLite db(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
            db.isCache();

            SQLiteStmt insertRequest, insertSelector, insertResult;
            SQLiteStmt insertObservationSet, insertRequestSetNode;
            SQLiteStmt insertAsk, insertTerminal, deleteAsks;
            insertRequest.create(db,
                "INSERT OR IGNORE INTO Requests(requestHash, payload) VALUES (?, ?)");
            insertSelector.create(db,
                "INSERT OR IGNORE INTO Selectors(selectorHash, payload) VALUES (?, ?)");
            insertResult.create(db,
                "INSERT OR IGNORE INTO Results(resultHash, payload) VALUES (?, ?)");
            insertObservationSet.create(db,
                "INSERT OR IGNORE INTO ObservationSet(setHash, payload) VALUES (?, ?)");
            insertRequestSetNode.create(db,
                "INSERT OR IGNORE INTO RequestSetNodes(nodeHash, payload) VALUES (?, ?)");
            insertAsk.create(db,
                "INSERT OR IGNORE INTO Ask(selectorHash, factSetHash, requestSetHash, altRequestSetHash) VALUES (?, ?, ?, ?)");
            insertTerminal.create(db,
                "INSERT OR IGNORE INTO Terminal(selectorHash, factSetHash, resultHash) VALUES (?, ?, ?)");
            deleteAsks.create(db,
                "DELETE FROM Ask WHERE selectorHash = ? AND factSetHash = ? AND requestSetHash = ?");

            while (true) {
                std::vector<WriteOp> batch;
                {
                    auto q = pending.lock();
                    while (q->empty() && !done.load())
                        q.wait(wakeup);
                    /* Coalesce: if the queue is small, wait briefly for
                       more work to accumulate before draining. Turns
                       a stream of one-op batches (avg 1.9 ops/batch was
                       observed in a NixOS eval) into fewer, larger
                       transactions — cuts the per-batch BEGIN/COMMIT
                       overhead proportionally. Timeout is short enough
                       to not add meaningful latency to interactive
                       workloads. Skip while done is set so shutdown
                       stays prompt. */
                    if (!done.load() && q->size() < 256) {
                        q.wait_for(wakeup, std::chrono::milliseconds(2), [&] {
                            return q->size() >= 256 || done.load();
                        });
                    }
                    batch = std::move(*q);
                    q->clear();
                }

                if (batch.empty()) {
                    if (done.load()) break;
                    continue;
                }

                SQLiteTxn txn(db);
                for (auto & op : batch) {
                    std::visit(overloaded{
                        [&](const WriteInsertRequest & w) {
                            auto use = insertRequest.use();
                            bindHashBlob(use, w.hash);
                            bindBytesBlob(use, w.payload);
                            use.exec();
                        },
                        [&](const WriteInsertSelector & w) {
                            auto use = insertSelector.use();
                            bindHashBlob(use, w.hash);
                            bindBytesBlob(use, w.payload);
                            use.exec();
                        },
                        [&](const WriteInsertResult & w) {
                            auto use = insertResult.use();
                            bindHashBlob(use, w.hash);
                            bindBytesBlob(use, w.payload);
                            use.exec();
                        },
                        [&](const WriteInsertObservationSet & w) {
                            auto use = insertObservationSet.use();
                            bindHashBlob(use, w.hash);
                            bindBytesBlob(use, w.payload);
                            use.exec();
                        },
                        [&](const WriteInsertRequestSetNode & w) {
                            auto use = insertRequestSetNode.use();
                            bindHashBlob(use, w.hash);
                            bindBytesBlob(use, w.payload);
                            use.exec();
                        },
                        [&](const WriteInsertAsk & w) {
                            auto use = insertAsk.use();
                            bindHashBlob(use, w.selectorHash);
                            bindHashBlob(use, w.factSetHash);
                            bindHashBlob(use, w.requestSetHash);
                            if (w.altRequestSetHash)
                                bindHashBlob(use, *w.altRequestSetHash);
                            else
                                use.bind();
                            use.exec();
                        },
                        [&](const WriteInsertTerminal & w) {
                            auto use = insertTerminal.use();
                            bindHashBlob(use, w.selectorHash);
                            bindHashBlob(use, w.factSetHash);
                            bindHashBlob(use, w.resultHash);
                            use.exec();
                        },
                        [&](const WriteDeleteAsk & w) {
                            auto use = deleteAsks.use();
                            bindHashBlob(use, w.selectorHash);
                            bindHashBlob(use, w.factSetHash);
                            bindHashBlob(use, w.requestSetHash);
                            use.exec();
                        },
                    }, op);
                }
                txn.commit();

                /* No per-batch wal_checkpoint: in-process readers are
                   served from in-memory caches (writes update the cache
                   synchronously before enqueue), and cross-process
                   visibility can wait for auto-checkpoint / process
                   exit. Doing PASSIVE per-batch dominated write time
                   at high batch counts (625k batches in developer
                   NixOS eval → 625k WAL scans). */

                processCount.fetch_add(batch.size());
                {
                    std::lock_guard<std::mutex> lock(drainMutex);
                }
                drainWakeup.notify_all();
            }
        } catch (std::exception & e) {
            ignoreExceptionInDestructor();
        }
    }
};

} // namespace

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

    /* Storage + DG layer — reads. */
    SQLiteStmt selectRequest, selectSelector, selectResult;
    SQLiteStmt selectObservationSet;
    SQLiteStmt selectRequestSetNode;
    SQLiteStmt countAsks, countTerminals;
    SQLiteStmt selectAsks;
    SQLiteStmt selectTerminal;

    /* Async writer thread — batches all DG inserts into transactions
       on its own SQLite connection (WAL allows concurrent writer +
       readers). Wait/drain lifecycle preserved via
       std::atexit(WriteQueue::flushAll) in case the enclosing
       EvalState is GC-allocated and never destroyed. */
    std::unique_ptr<WriteQueue> writeQueue;

    /* In-memory caches of parsed sets and payloads. Populated lazily on
       first read or write so that subsequent operations within the same
       process avoid the SQLite round-trip and the CBOR decode.
       std::optional<vector<...>> distinguishes a known-empty result from
       a known-missing one.

       RequestSet has no vector-form cache — getRequestSet routes
       through requestSetTrieCache + FrozenNode::allMembers, memoized
       on the FrozenNode itself. */
    std::unordered_map<TracingHash, std::optional<std::vector<TracingDecisionGraph::Fact>>> factSetCache;
    std::unordered_map<TracingHash, std::optional<std::string>> requestPayloadCache;
    std::unordered_map<TracingHash, std::optional<std::string>> resultPayloadCache;
    /* Selector payloads were formerly written PLAIN because SelectorPool
       tracks in-memory identity and no in-session reader looked up the
       payload. Under async writing, in-session reads (e.g. tests that
       round-trip via getSelectorPayload, or any cross-session lookup
       arriving before the writer commits) must not miss the queued
       write. Cache the payload on insert, same as Request/Result. */
    std::unordered_map<TracingHash, std::optional<std::string>> selectorPayloadCache;
    /* Typed-form caches. `getRequest` / `getResult` decode from CBOR
       once per hash and cache the typed variant. Consumers use the
       typed accessors and never see raw bytes. */
    std::unordered_map<TracingHash, std::optional<trace::Request>> requestCache;
    std::unordered_map<TracingHash, std::optional<trace::ResultVariant>> resultCache;
    /* RequestSet trie *node* cache. Different RequestSets that share
       subtrees (via content addressing) hit the same node hashes;
       caching per-node lets second-and-later getRequestSet calls reuse
       the SQLite reads from the first. */
    std::unordered_map<TracingHash, std::optional<std::string>> requestSetNodePayloadCache;

    /* In-memory RequestSet trie cache. Shared across all insertRequestSet
       callers so structurally-overlapping request sets reuse identical
       subtrees without re-hashing / re-persisting. persistedFlag on each
       FrozenNode short-circuits the persist walk for already-enqueued
       subtrees. */
    trace::rst::FrozenNodeCache requestSetTrieCache;
    /* ObservationSet SCA-nesting-depth memo. Populated at
       insertObservationSet time: depth = 1 + max depth over argObsSets
       of any SelectorCallbackApply in the set (0 if none). Since
       nested obsSets are always inserted before their outer parent
       (my TCA::queryApply's applyFn recursion is depth-first, and my
       RCA::queryApply's O17 similarly), the memo has all children
       populated by the time a parent computes its depth. */
    std::unordered_map<TracingHash, std::uint32_t> obsSetDepthMemo;

    /* In-memory buffers for the DG-layer edges. Populated lazily on
       first read (or eagerly on insert/remove), then kept as the
       authoritative in-session view. Async-writer inserts flush to
       DB on the writer thread's own connection, but readers use these
       caches instead of racing the writer via SQL. Cache indexed by
       (queryHash, factSetHash) — the same key SQLite uses. */
    std::map<std::pair<TracingHash, TracingHash>, std::vector<AskEdge>> askEdgesCache;
    std::map<std::pair<TracingHash, TracingHash>, std::optional<ResultHash>> terminalCache;
};

/* ─────────────────────────────────────────────────────────────────────
   Helpers. Names are dg_-prefixed because unity builds merge this
   TU with tracing-index.cc which defines same-named statics.
   ───────────────────────────────────────────────────────────────────── */

static std::string dg_hashToBlob(const TracingHash & h)
{
    return std::string(reinterpret_cast<const char *>(h.bytes.data()), TracingHash::size);
}

static TracingHash dg_blobToHash(std::string_view blob)
{
    /* Tracing hashes are 16 raw bytes; blob rows carry that fixed size. */
    if (blob.size() != TracingHash::size)
        throw Error("decision-graph: malformed hash blob (size=%d)", blob.size());
    TracingHash h;
    std::memcpy(h.bytes.data(), blob.data(), blob.size());
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
TracingHash TracingDecisionGraph::factElementHash(const TracingHash & request, const TracingHash & response)
{
    std::string buf;
    buf.reserve(2 * TracingHash::size);
    buf.append(reinterpret_cast<const char *>(request.bytes.data()), TracingHash::size);
    buf.append(reinterpret_cast<const char *>(response.bytes.data()), TracingHash::size);
    return trace::tracingHash(buf);
}

TracingHash TracingDecisionGraph::xorHashes(const TracingHash & a, const TracingHash & b)
{
    return a.xorWith(b);
}

/* ──────────────────────────────────────────────────────────────────────
   RequestSet storage lives in the rst layer (see
   `nix/expr/request-set-trie.hh`) — top-down HAMT with XOR-of-members
   identity. See that file for the canonical shape and payload format.
   ────────────────────────────────────────────────────────────────────── */

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

TracingHash TracingDecisionGraph::computeResponseHash(const std::string & payload)
{
    return trace::tracingHash(payload);
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

    state->selectRequest.create(state->db,
        "SELECT payload FROM Requests WHERE requestHash = ?");
    state->selectSelector.create(state->db,
        "SELECT payload FROM Selectors WHERE selectorHash = ?");
    state->selectResult.create(state->db,
        "SELECT payload FROM Results WHERE resultHash = ?");
    state->selectObservationSet.create(state->db,
        "SELECT payload FROM ObservationSet WHERE setHash = ?");
    /* Drop obsolete tables from earlier schema versions. */
    state->db.exec("DROP TABLE IF EXISTS FactSets;");
    state->db.exec("DROP TABLE IF EXISTS EdgeResponses;");
    state->db.exec("DROP TABLE IF EXISTS SubjectStampSites;");
    state->db.exec("DROP TABLE IF EXISTS ApplyResultProducers;");

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
    state->selectAsks.create(state->db,
        "SELECT requestSetHash, altRequestSetHash FROM Ask WHERE selectorHash = ? AND factSetHash = ?");
    state->selectTerminal.create(state->db,
        "SELECT resultHash FROM Terminal WHERE selectorHash = ? AND factSetHash = ?");
    state->countAsks.create(state->db,
        "SELECT 1 FROM Ask WHERE selectorHash = ? AND factSetHash = ? LIMIT 1");
    state->countTerminals.create(state->db,
        "SELECT 1 FROM Terminal WHERE selectorHash = ? AND factSetHash = ? LIMIT 1");

    /* Writer thread runs on its own SQLite connection to the same DB
       file (WAL mode allows concurrent writer + readers). Schema
       exists by the time the thread first opens; INSERT statements
       are prepared inside run() against that connection. */
    state->writeQueue = std::make_unique<WriteQueue>(dbPath);
}

TracingDecisionGraph::~TracingDecisionGraph() = default;

void TracingDecisionGraph::waitForWrites()
{
    /* Block until every write enqueued before this call has been
       committed by the writer thread's connection. Not on any read
       path — reads consult in-memory caches without waiting. Callers
       who need cross-process visibility (e.g. inspecting the DB file
       from another tool) use this. */
    WriteQueue * wq;
    {
        auto state(_state->lock());
        wq = state->writeQueue.get();
    }
    wq->waitDrained();
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
#define ATOM_INSERT_CACHED(NAME, CACHE, OP)                                      \
    void TracingDecisionGraph::insert##NAME(const TracingHash & th, std::string_view p) \
    {                                                                            \
        auto state(_state->lock());                                              \
        /* Mirror INSERT OR IGNORE in memory. On repeat, skip enqueue. */        \
        auto [it, inserted] = state->CACHE.try_emplace(                          \
            th, std::optional{std::string(p)});                                  \
        if (!inserted)                                                           \
            return;                                                              \
        state->writeQueue->enqueue(OP{th, std::string(p)});                      \
    }

#define ATOM_INSERT_PLAIN(NAME, OP)                                              \
    void TracingDecisionGraph::insert##NAME(const TracingHash & th, std::string_view p) \
    {                                                                            \
        auto state(_state->lock());                                              \
        state->writeQueue->enqueue(OP{th, std::string(p)});                      \
    }

ATOM_INSERT_CACHED(Request, requestPayloadCache, WriteInsertRequest)
ATOM_INSERT_CACHED(Selector, selectorPayloadCache, WriteInsertSelector)
ATOM_INSERT_CACHED(Result, resultPayloadCache, WriteInsertResult)
#undef ATOM_INSERT_CACHED
#undef ATOM_INSERT_PLAIN

#define ATOM_GET_CACHED(NAME, CACHE)                                            \
    std::optional<std::string> TracingDecisionGraph::get##NAME##Payload(        \
        const TracingHash & th)                                                 \
    {                                                                           \
        auto state(_state->lock());                                             \
        if (auto it = state->CACHE.find(th); it != state->CACHE.end())          \
            return it->second;                                                  \
        auto query = state->select##NAME.use();                                 \
        dg_bindBlob(query, dg_hashToBlob(th));                                  \
        std::optional<std::string> payload;                                     \
        if (query.next())                                                       \
            payload = query.getBlob(0);                                         \
        state->CACHE.emplace(th, payload);                                      \
        return payload;                                                         \
    }

#define ATOM_GET_PLAIN(NAME)                                                    \
    std::optional<std::string> TracingDecisionGraph::get##NAME##Payload(        \
        const TracingHash & th)                                                 \
    {                                                                           \
        auto state(_state->lock());                                             \
        auto query = state->select##NAME.use();                                 \
        dg_bindBlob(query, dg_hashToBlob(th));                                  \
        if (!query.next())                                                      \
            return std::nullopt;                                                \
        return query.getBlob(0);                                                \
    }

ATOM_GET_CACHED(Request, requestPayloadCache)
ATOM_GET_CACHED(Selector, selectorPayloadCache)
ATOM_GET_CACHED(Result, resultPayloadCache)
#undef ATOM_GET_CACHED
#undef ATOM_GET_PLAIN

std::optional<trace::Request> TracingDecisionGraph::getRequest(const TracingHash & th)
{
    {
        auto state(_state->lock());
        if (auto it = state->requestCache.find(th); it != state->requestCache.end())
            return it->second;
    }
    auto payload = getRequestPayload(th);
    if (!payload) {
        auto state(_state->lock());
        state->requestCache.emplace(th, std::nullopt);
        return std::nullopt;
    }
    std::optional<trace::Request> typed;
    try {
        auto j = nlohmann::json::from_cbor(*payload);
        typed = trace::decodeRequest(j, selectorPool);
    } catch (...) {}
    auto state(_state->lock());
    state->requestCache.emplace(th, typed);
    return typed;
}

std::optional<trace::ResultVariant> TracingDecisionGraph::getResult(const TracingHash & th)
{
    {
        auto state(_state->lock());
        if (auto it = state->resultCache.find(th); it != state->resultCache.end())
            return it->second;
    }
    auto payload = getResultPayload(th);
    if (!payload) {
        auto state(_state->lock());
        state->resultCache.emplace(th, std::nullopt);
        return std::nullopt;
    }
    std::optional<trace::ResultVariant> typed;
    try {
        auto j = nlohmann::json::from_cbor(*payload);
        typed = trace::decodeResult(j);
    } catch (...) {}
    auto state(_state->lock());
    state->resultCache.emplace(th, typed);
    return typed;
}

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
    const std::vector<TracingDecisionGraph::InlineFact> & sortedMembers)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto & m : sortedMembers) {
        /* Response payload embedded as a JSON binary value —
           nlohmann::json's CBOR encoder handles it natively as a
           byte string. Preserves exact CBOR bytes without any hex
           encoding overhead. */
        arr.push_back({
            {"q", m.reqHash.toHex()},
            {"p", nlohmann::json::binary(std::vector<std::uint8_t>(
                m.responsePayload.begin(), m.responsePayload.end()))},
        });
    }
    auto cbor = nlohmann::json::to_cbor(arr);
    return std::string(reinterpret_cast<const char *>(cbor.data()), cbor.size());
}

/* Per-obsSet-member content hash: BLAKE3(reqHash || responsePayload).
   The set identity is the XOR-fold of these — order-independent
   (commutative XOR) so no sort is needed on either the hash side or,
   given set semantics, the storage side. */
static TracingHash dg_obsSetMemberHash(const TracingDecisionGraph::InlineFact & m)
{
    std::string buf;
    buf.reserve(TracingHash::size + m.responsePayload.size());
    buf.append(reinterpret_cast<const char *>(m.reqHash.bytes.data()), TracingHash::size);
    buf.append(m.responsePayload);
    return trace::tracingHash(buf);
}

TracingHash TracingDecisionGraph::insertObservationSet(
    std::vector<TracingDecisionGraph::InlineFact> members)
{
    /* Sort+dedup first: XOR-fold cancels duplicates, so we'd key
       {A, A} and {} identically without dedup — breaking obsSet
       identity for callers that push the same fact twice per firing.
       Sort cost is O(N log N) on small N (typically < 50 members),
       tolerated. */
    auto sorted = dg_sortAndDedup(std::move(members));

    /* Cheap identity via XOR-fold of per-member hashes on the deduped
       list. Under H2 compositional recording the same obsSet recurs
       across many callback firings; on the repeat path we memo-hit
       here and skip the CBOR-build + storage-round-trip. */
    TracingHash th = trace::tracingZeroHash();
    for (const auto & m : sorted)
        th.xorInPlace(dg_obsSetMemberHash(m));

    {
        auto state(_state->lock());
        if (state->obsSetDepthMemo.contains(th))
            return th;
    }

    auto payload = dg_observationSetPayload(sorted);

    /* SCA-nesting depth: derived from the ACTUAL obsSet content, not
       from execution ordering. For each member whose reqHash resolves
       to a SelectorCallbackApply, look up its argObsSet in the memo
       and take max(depth) + 1. If no SCA members, depth = 0. */
    std::uint32_t depth = 0;
    for (const auto & m : sorted) {
        auto selOpt = selectorPool.find(m.reqHash);
        if (!selOpt) continue;
        auto * sca = std::get_if<trace::SelectorCallbackApply>(&(*selOpt)->node);
        if (!sca) continue;
        std::uint32_t childDepth = 0;
        {
            auto state(_state->lock());
            auto it = state->obsSetDepthMemo.find(sca->argObsSet);
            if (it != state->obsSetDepthMemo.end())
                childDepth = it->second;
        }
        if (childDepth + 1 > depth)
            depth = childDepth + 1;
    }

    {
        auto state(_state->lock());
        state->writeQueue->enqueue(WriteInsertObservationSet{th, payload});
        state->obsSetDepthMemo[th] = depth;
    }

    auto & stats = tracingCacheStats();
    if (depth > stats.maxCallbackObsSetNestingDepth)
        stats.maxCallbackObsSetNestingDepth = depth;

    return th;
}

std::optional<std::vector<TracingDecisionGraph::InlineFact>>
TracingDecisionGraph::getObservationSet(const TracingHash & th)
{
    std::optional<std::string> payload;
    {
        auto state(_state->lock());
        auto query = state->selectObservationSet.use();
        dg_bindBlob(query, dg_hashToBlob(th));
        if (query.next())
            payload = query.getBlob(0);
    }
    if (!payload)
        return std::nullopt;
    auto bytes = reinterpret_cast<const uint8_t *>(payload->data());
    auto arr = nlohmann::json::from_cbor(bytes, bytes + payload->size());
    std::vector<InlineFact> members;
    members.reserve(arr.size());
    for (const auto & elt : arr) {
        InlineFact m;
        m.reqHash = trace::parseTracingHex(elt.at("q").get<std::string>());
        auto & binVal = elt.at("p").get_binary();
        m.responsePayload.assign(binVal.begin(), binVal.end());
        members.push_back(std::move(m));
    }
    return members;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeRequestSetHash(const std::vector<RequestHash> & members)
{
    /* XOR-fold of the dedup'd member set — matches rst::FrozenNode's
       identity so this and insertRequestSet(members) always agree
       on the canonical hash. Empty set → all-zero (emptySetHash). */
    SetHash acc = emptySetHash();
    std::unordered_set<RequestHash> seen;
    seen.reserve(members.size());
    for (const auto & m : members)
        if (seen.insert(m).second)
            acc = acc.xorWith(m);
    return acc;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::computeFactSetHash(const std::vector<Fact> & members)
{
    auto canonical = dg_sortAndDedup(members);
    SetHash out = emptySetHash();
    for (const auto & f : canonical)
        out.xorInPlace(factElementHash(f.request, f.response));
    return out;
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::emptySetHash()
{
    /* All-zero tracing hash: XOR identity, so H(∅ ∪ {e}) = H_element(e). */
    return trace::tracingZeroHash();
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
    auto req = getRequest(h);
    if (!req) return false;
    auto * ovr = std::get_if<trace::OuterValueRequest>(&*req);
    if (!ovr) return false;
    return std::holds_alternative<trace::SelectorApply>(ovr->query->node);
}

trace::rst::FrozenNodePtr
TracingDecisionGraph::internRequestSet(std::vector<RequestHash> members)
{
    auto state(_state->lock());
    return state->requestSetTrieCache.internSet(std::move(members));
}

std::optional<trace::rst::FrozenNodePtr>
TracingDecisionGraph::tryFindRequestSet(const TracingHash & identity)
{
    auto state(_state->lock());
    return state->requestSetTrieCache.lookup(identity);
}

trace::rst::FrozenNodePtr
TracingDecisionGraph::insertSortedMembers(
    const trace::rst::FrozenNodePtr & node,
    std::span<const RequestHash> sortedMembers)
{
    auto state(_state->lock());
    return trace::rst::insertSortedMembers(
        node, sortedMembers, state->requestSetTrieCache);
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::insertRequestSet(trace::rst::FrozenNodePtr node)
{
    if (node->size() == 0)
        return emptySetHash();
    auto state(_state->lock());
    trace::rst::FrozenNodeCache::PersistSink sink =
        [&](const TracingHash & nodeHash, std::string_view payload) {
            auto [it, inserted] = state->requestSetNodePayloadCache.try_emplace(
                nodeHash, std::optional<std::string>{std::string(payload)});
            if (!inserted)
                return;
            state->writeQueue->enqueue(
                WriteInsertRequestSetNode{nodeHash, std::string(payload)});
        };
    state->requestSetTrieCache.persist(node, sink);
    return node->hash;
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
        setHash.xorInPlace(factElementHash(f.request, f.response));
    auto state(_state->lock());
    state->factSetCache.try_emplace(setHash, std::optional{std::move(canonical)});
    return setHash;
}

TracingHash TracingDecisionGraph::xorFactIntoHash(
    const TracingHash & h, const TracingHash & request, const TracingHash & response)
{
    return h.xorWith(factElementHash(request, response));
}

void TracingDecisionGraph::persistRequestSetNode(
    const TracingHash & nodeHash, std::string_view payload)
{
    auto state(_state->lock());
    auto [it, inserted] = state->requestSetNodePayloadCache.try_emplace(
        nodeHash, std::optional<std::string>{std::string(payload)});
    if (!inserted)
        return;
    state->writeQueue->enqueue(WriteInsertRequestSetNode{nodeHash, std::string(payload)});
}

TracingDecisionGraph::SetHash
TracingDecisionGraph::extendRequestSet(const SetHash & parent, const std::vector<RequestHash> & extras)
{
    /* Trie-native extension: seed a MutableNode from the parent trie,
       insert extras (CoW along the modified paths — untouched sibling
       subtrees stay pointer-identical to parent's), freeze into the
       shared cache. */
    auto parentNode = getRequestSetNode(parent);
    trace::rst::MutableNode mut;
    if (parentNode)
        mut = trace::rst::MutableNode(*parentNode);
    for (const auto & e : extras)
        mut.insert(e);
    auto node = [&] {
        auto state(_state->lock());
        return mut.freeze(state->requestSetTrieCache);
    }();
    return insertRequestSet(node);
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
std::optional<std::string> TracingDecisionGraph::getRequestSetNodePayload(const TracingHash & nodeHash)
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

std::optional<std::vector<TracingDecisionGraph::RequestHash>>
TracingDecisionGraph::getRequestSet(const SetHash & h)
{
    /* Route through the rst trie cache. FrozenNode::allMembers is
       memoized on the interned node itself, so repeat calls for the
       same set hash return the same materialized vector without
       re-walking. */
    if (h == emptySetHash())
        return std::vector<RequestHash>{};
    auto node = getRequestSetNode(h);
    if (!node)
        return std::nullopt;
    return (*node)->allMembers();
}

std::optional<trace::rst::FrozenNodePtr>
TracingDecisionGraph::getRequestSetNode(const SetHash & h)
{
    /* Empty set: hand back the interned empty node. */
    if (h == emptySetHash()) {
        auto state(_state->lock());
        return state->requestSetTrieCache.internSet({});
    }
    auto state(_state->lock());
    /* Quick hit: node already in the trie cache. */
    if (auto existing = state->requestSetTrieCache.lookup(h))
        return *existing;
    /* Load top-down: fetch our payload, recursively load children (so
       they're interned before we intern ourselves — required by
       FrozenNodeCache::intern's contract). */
    std::function<std::optional<trace::rst::FrozenNodePtr>(const TracingHash &)> loadNode;
    loadNode = [&](const TracingHash & nodeHash) -> std::optional<trace::rst::FrozenNodePtr> {
        if (auto existing = state->requestSetTrieCache.lookup(nodeHash))
            return *existing;
        /* Fetch payload from the payload cache (populated on write) or
           from SQLite. Duplicates getRequestSetNodePayload's logic but
           without releasing the state lock — we hold it throughout the
           top-down walk so the load-then-intern is atomic. */
        std::optional<std::string> payload;
        if (auto it = state->requestSetNodePayloadCache.find(nodeHash);
            it != state->requestSetNodePayloadCache.end())
            payload = it->second;
        else {
            auto query = state->selectRequestSetNode.use();
            dg_bindBlob(query, dg_hashToBlob(nodeHash));
            if (query.next())
                payload = query.getBlob(0);
            state->requestSetNodePayloadCache.emplace(nodeHash, payload);
        }
        if (!payload)
            return std::nullopt;
        /* Recursively load children before interning the parent —
           FrozenNodeCache::intern requires child hashes cached. */
        for (const auto & childHash : trace::rst::childHashesInPayload(*payload))
            if (!loadNode(childHash))
                return std::nullopt;
        return state->requestSetTrieCache.intern(nodeHash, *payload);
    };
    return loadNode(h);
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
    auto key = std::make_pair(q, factSet);
    /* Populate cache from DB if not seen yet, then append the new
       edge. On a cache miss the DB load runs synchronously (no writer
       thread involvement — this Ask isn't queued yet). Subsequent
       getAsks / removeAsk / hasAnyEdge see the authoritative state
       without waiting for the writer. */
    auto it = state->askEdgesCache.find(key);
    if (it == state->askEdgesCache.end()) {
            auto query = state->selectAsks.use();
        dg_bindBlob(query, dg_hashToBlob(q));
        dg_bindBlob(query, dg_hashToBlob(factSet));
        std::vector<AskEdge> loaded;
        while (query.next()) {
            AskEdge e{dg_blobToHash(query.getBlob(0)), std::nullopt};
            if (!query.isNull(1))
                e.altRequestSet = dg_blobToHash(query.getBlob(1));
            loaded.push_back(std::move(e));
        }
        it = state->askEdgesCache.emplace(key, std::move(loaded)).first;
    }
    /* Match INSERT OR IGNORE semantics: primary key is
       (selectorHash, factSetHash, requestSetHash) — altRequestSet is
       NOT part of the key. A second insertAsk with same primary and
       different alt is dropped (the first alt sticks). */
    for (const auto & e : it->second) {
        if (e.requestSet == requestSet)
            return;
    }
    it->second.push_back(AskEdge{requestSet, altRequestSet});
    state->writeQueue->enqueue(WriteInsertAsk{
        q, factSet, requestSet, altRequestSet});
}

std::vector<TracingDecisionGraph::AskEdge>
TracingDecisionGraph::getAsks(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto key = std::make_pair(q, factSet);
    /* In-memory cache is authoritative for (q, factSet) once populated.
       Populated lazily on first read from DB, then kept in sync by
       insertAsk / removeAsk. Avoids blocking readers on the async
       writer thread. */
    auto it = state->askEdgesCache.find(key);
    if (it != state->askEdgesCache.end())
        return it->second;
    /* First read: load from DB and cache. */
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
    state->askEdgesCache.emplace(key, out);
    return out;
}

void TracingDecisionGraph::removeAsk(
    const QueryHash & q, const SetHash & factSet, const SetHash & requestSet)
{
    auto state(_state->lock());
    auto key = std::make_pair(q, factSet);
    auto it = state->askEdgesCache.find(key);
    if (it != state->askEdgesCache.end()) {
        auto & v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(),
            [&](const AskEdge & e) { return e.requestSet == requestSet; }),
            v.end());
    }
    /* Note: if the cache isn't populated, the removal still gets
       queued for the DB. Subsequent getAsks will hit DB, which will
       reflect the removal after the writer flushes. Missing an
       in-session cache load here is fine because we don't need to
       report the removed rows. */
    state->writeQueue->enqueue(WriteDeleteAsk{
        q, factSet, requestSet});
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

    /* Track remaining's XOR-identity incrementally across split steps.
       Compute it once now over the initial facts; each successful
       split subtracts the consumed reqs' XOR from it (XOR is
       self-inverse). Avoids re-folding remaining every trySplitOne
       call. */
    TracingHash remainingXor = emptySetHash();
    for (const auto & f : remaining)
        remainingXor.xorInPlace(f.request);

    struct SplitStep
    {
        TracingHash newCur;
        std::vector<Fact> remainingAfterConsume;
        TracingHash remainingXorAfterConsume;
    };

    /* trySplitOne: find an overlap between remaining and an existing
       edge at (q, cur), execute the split, return the advanced (cur,
       remaining). Returns nullopt when no overlap is found (caller
       inserts the remainder as a plain edge).

       The shared-prefix calc uses trie intersection with hash-equal
       subtree short-circuit — under cumulative request sets, existing
       edges and the new remaining set share large subtrees and the
       intersection collapses to shared subtree pointers rather than
       walking every member. */
    auto trySplitOne =
        [&](const SetHash & cur,
            const std::vector<Fact> & remaining,
            const TracingHash & remainingXor) -> std::optional<SplitStep>
    {
        /* XOR-first-lookup: skip the vector build when remainingNode's
           identity is already cached (heavy reuse under matching-until-
           divergence). */
        trace::rst::FrozenNodePtr remainingNode = [&] {
            if (auto existing = tryFindRequestSet(remainingXor))
                return *existing;
            std::vector<RequestHash> vec;
            vec.reserve(remaining.size());
            for (const auto & f : remaining) vec.push_back(f.request);
            return internRequestSet(std::move(vec));
        }();

        for (const auto & edge : getAsks(q, cur)) {
            auto exRsHash = edge.requestSet;
            auto exNodeOpt = getRequestSetNode(exRsHash);
            if (!exNodeOpt)
                continue;
            auto exNode = *exNodeOpt;

            /* Intersect via trie — hash-equal subtrees short-circuit,
               so under high overlap this is O(|shared|) rather than
               O(|exNode| + |remaining|). */
            trace::rst::FrozenNodePtr sharedNode = [&] {
                auto state(_state->lock());
                return trace::rst::intersection(exNode, remainingNode,
                    state->requestSetTrieCache);
            }();
            auto sharedSize = sharedNode->size();
            if (sharedSize == 0)
                continue;

            /* Keep exUseful for the split-shape check + tail
               construction — its size drives whether we do a "shared
               == existing" collapse or need to insert a separate tail
               rs. Route through the trie node we already have from
               getRequestSetNode above — allMembers is memoised on the
               FrozenNode, so no re-walk on repeat visits. */
            auto exAllMembers = exNode->allMembers();
            auto exUseful = usefulDispatch(exAllMembers, dispatchedSoFar);
            if (exUseful.empty())
                continue;

            /* Full identity: existing rs is exactly remaining. Nothing
               to insert; signal completion by returning empty remaining. */
            if (sharedSize == exUseful.size() && sharedSize == remaining.size())
                return SplitStep{cur, {}, emptySetHash()};

            /* Fold shared facts into cur → intermediate; partition
               remaining into consumed vs tailNew. Route membership
               through sharedNode->contains — the HAMT lookup is
               O(depth), same complexity as an unordered_set hit but
               without the intermediate set construction.

               Accumulate consumed reqs' XOR into consumedXor as we
               go, so the next outer iteration inherits an incremental
               remainingXor without re-folding. */
            TracingHash intermediate = cur;
            TracingHash consumedXor = emptySetHash();
            std::vector<Fact> tailNew;
            tailNew.reserve(remaining.size() - sharedSize);
            for (const auto & f : remaining) {
                if (sharedNode->contains(f.request)) {
                    intermediate = xorFactIntoHash(intermediate, f.request, f.response);
                    consumedXor.xorInPlace(f.request);
                } else {
                    tailNew.push_back(f);
                }
            }
            TracingHash newRemainingXor = remainingXor;
            newRemainingXor.xorInPlace(consumedXor);

            /* Persist shared via the trie-native overload — no
               vector-flatten hop, and the FrozenNode is already
               interned in the cache. */
            auto sharedRsHash = insertRequestSet(sharedNode);

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
            if (sharedSize != exUseful.size()) {
                /* Compute the tail's XOR-identity streaming so we can
                   cache-lookup without materialising the tail vector.
                   Under matching-until-divergence the same tail
                   identity shows up across splits and hits often. */
                TracingHash tailXor = emptySetHash();
                for (const auto & req : exUseful)
                    if (!sharedNode->contains(req))
                        tailXor.xorInPlace(req);
                auto tailNode = tryFindRequestSet(tailXor);
                if (!tailNode) {
                    std::vector<RequestHash> tail;
                    tail.reserve(exUseful.size() - sharedSize);
                    for (const auto & req : exUseful)
                        if (!sharedNode->contains(req))
                            tail.push_back(req);
                    tailNode = internRequestSet(std::move(tail));
                }
                auto tailRsHash = insertRequestSet(*tailNode);
                insertAsk(q, cur, sharedRsHash);
                insertAsk(q, intermediate, tailRsHash, edge.altRequestSet);
                removeAsk(q, cur, exRsHash);
            } else {
                insertAsk(q, cur, sharedRsHash);
            }

            tracingCacheLog(
                "insertAskSplitting Q=%s split at cur=%s: shared=%zu, exUseful=%zu, newRemaining=%zu "
                "(intermediate=%s, sharedRS=%s, exRS=%s)",
                q.toHex().substr(0, 12).c_str(),
                cur.toHex().substr(0, 12).c_str(),
                sharedSize, exUseful.size(), remaining.size(),
                intermediate.toHex().substr(0, 12).c_str(),
                sharedRsHash.toHex().substr(0, 12).c_str(),
                exRsHash.toHex().substr(0, 12).c_str());

            return SplitStep{intermediate, std::move(tailNew), newRemainingXor};
        }
        return std::nullopt;
    };

    /* Iterate split steps until no overlap remains. Each successful
       split drops alt (the row's identity changed) and shrinks
       remainingXor by the consumed reqs' XOR. */
    while (auto step = trySplitOne(cur, remaining, remainingXor)) {
        cur = step->newCur;
        remaining = std::move(step->remainingAfterConsume);
        remainingXor = step->remainingXorAfterConsume;
        alt = std::nullopt;
    }

    /* No split available at this cur. Insert whatever's left as a plain
       edge, preserving alt. Empty remaining = nothing to insert. */
    if (remaining.empty())
        return;
    /* XOR-first-lookup fast path — remainingXor is maintained
       incrementally by the trySplitOne loop, so we already have it. */
    auto newNode = tryFindRequestSet(remainingXor);
    if (!newNode) {
        std::vector<RequestHash> newReqs;
        newReqs.reserve(remaining.size());
        for (const auto & f : remaining) newReqs.push_back(f.request);
        newNode = internRequestSet(std::move(newReqs));
    }
    auto newRsHash = insertRequestSet(*newNode);
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
    auto key = std::make_pair(q, factSet);
    /* Mirror INSERT OR IGNORE: only the first Terminal wins for a
       given (q, factSet). If a cached Terminal already exists, don't
       overwrite it. */
    auto [it, inserted] = state->terminalCache.try_emplace(key, std::optional<ResultHash>(result));
    if (!inserted && it->second.has_value())
        return;
    it->second = result;
    state->writeQueue->enqueue(WriteInsertTerminal{
        q, factSet, result});
}

std::optional<TracingDecisionGraph::ResultHash>
TracingDecisionGraph::getTerminal(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto key = std::make_pair(q, factSet);
    auto it = state->terminalCache.find(key);
    if (it != state->terminalCache.end()) {
        tracingCacheLog("getTerminal(q=%s, fs=%s) CACHE %s",
                        q.toHex().c_str(),
                        factSet.toHex().c_str(),
                        it->second ? "HIT" : "MISS");
        return it->second;
    }
    auto query = state->selectTerminal.use();
    dg_bindBlob(query, dg_hashToBlob(q));
    dg_bindBlob(query, dg_hashToBlob(factSet));
    std::optional<ResultHash> result;
    if (query.next())
        result = dg_blobToHash(query.getBlob(0));
    tracingCacheLog("getTerminal(q=%s, fs=%s) SQL %s",
                    q.toHex().c_str(),
                    factSet.toHex().c_str(),
                    result ? "HIT" : "MISS");
    state->terminalCache.emplace(key, result);
    return result;
}

/* ─────────────────────────────────────────────────────────────────────
   Recording and replay
   ───────────────────────────────────────────────────────────────────── */

/* Inner body of record(). The body doesn't mutate the passed
   collections; it tracks a local dispatchedSoFar for "what I've
   consumed so far". remaining-as-set = allRequests \ dispatchedSoFar. */
static void dg_recordImpl(
    TracingDecisionGraph & g,
    const TracingHash & q,
    const TracingHash & factSetHash,
    const TracingHash & result,
    const std::unordered_map<TracingHash, TracingHash> & responseFor,
    const std::unordered_set<TracingHash> & allRequests,
    TracingHash startFactSetHash = TracingDecisionGraph::emptySetHash())
{
    auto cur = startFactSetHash;
    std::unordered_set<TracingHash> dispatchedSoFar;

    auto isInRemaining = [&](const TracingHash & req) {
        return allRequests.count(req) && !dispatchedSoFar.count(req);
    };

    auto extendCurFromReqs = [&](const std::vector<TracingHash> & reqs) {
        for (const auto & req : reqs) {
            assert(!dispatchedSoFar.count(req));
            auto it = responseFor.find(req);
            assert(it != responseFor.end());
            cur = TracingDecisionGraph::xorFactIntoHash(cur, req, it->second);
            dispatchedSoFar.insert(req);
        }
    };
    auto extendCurFromFacts = [&](const std::vector<TracingDecisionGraph::Fact> & facts) {
        for (const auto & f : facts) {
            assert(!dispatchedSoFar.count(f.request));
            cur = TracingDecisionGraph::xorFactIntoHash(cur, f.request, f.response);
            dispatchedSoFar.insert(f.request);
        }
    };

    while (dispatchedSoFar.size() < allRequests.size()) {
        /* Patricia split is now handled inside insertAskSplitting
           (called on the fallback insert below). No separate eager
           pass here — followUseful handles the discovery-of-existing
           optimisation, and any residual Ask insert splits against
           existing at the same cur. */

        std::optional<std::vector<TracingHash>> followUseful;
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
            extendCurFromReqs(*followUseful);
        } else {
            /* Build the (req, resp) facts for the current remaining and
               route through insertAskSplitting so any partial overlap
               with an existing Ask at (q, cur) gets factored via
               Patricia split. */
            std::vector<TracingDecisionGraph::Fact> remainingFacts;
            remainingFacts.reserve(allRequests.size() - dispatchedSoFar.size());
            for (const auto & req : allRequests)
                if (!dispatchedSoFar.count(req)) {
                    auto it = responseFor.find(req);
                    assert(it != responseFor.end());
                    remainingFacts.push_back({req, it->second});
                }
            g.insertAskSplitting(q, cur, remainingFacts, dispatchedSoFar);
            extendCurFromFacts(remainingFacts);
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
    std::unordered_map<TracingHash, TracingHash> responseFor;
    responseFor.reserve(facts->size());
    std::unordered_set<TracingHash> allRequests;
    allRequests.reserve(facts->size());
    for (const auto & f : *facts) {
        responseFor.emplace(f.request, f.response);
        allRequests.insert(f.request);
    }
    dg_recordImpl(*this, q, factSetHash, result, responseFor, allRequests);
}

bool TracingDecisionGraph::hasAnyEdge(const QueryHash & q, const SetHash & factSet)
{
    auto state(_state->lock());
    auto key = std::make_pair(q, factSet);
    /* If either cache has a positive answer, no DB round-trip needed. */
    if (auto it = state->askEdgesCache.find(key); it != state->askEdgesCache.end() && !it->second.empty())
        return true;
    if (auto it = state->terminalCache.find(key); it != state->terminalCache.end() && it->second.has_value())
        return true;
    /* Fall back to DB. If either cache is populated with a negative
       (empty vector or nullopt), we still have to check the OTHER
       table on disk because the caches are per-table. */
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
    const std::function<ResponseHash(const RequestHash &)> & dispatch,
    const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt,
    const SetHash & startCur)
{
    TracingHash q = q_initial;
    auto cur = startCur;
    /* dispatchedSoFar speeds up the "is this request already in cur?"
       filter on each edge, and (since dispatch filters them out
       too) guarantees the XOR-extension below isn't fed a fact
       that's already folded into cur. */
    std::unordered_set<RequestHash> dispatchedSoFar;
    tracingCacheLog("history Q=%s startCur=%s",
                    q.toHex().substr(0, 12),
                    cur.toHex().substr(0, 12));
    for (;;) {
        if (auto term = getTerminal(q, cur)) {
            tracingCacheLog("history Q=%s TERMINAL at cur=%s",
                            q.toHex().substr(0, 12),
                            cur.toHex().substr(0, 12));
            return WalkHit{*term, cur};
        }

        auto outgoing = getAsks(q, cur);
        if (outgoing.empty()) {
            tracingCacheLog("history Q=%s NO OUTGOING at cur=%s -> miss",
                            q.toHex().substr(0, 12),
                            cur.toHex().substr(0, 12));
            return std::nullopt; // no path forward, no terminal
        }

        tracingCacheLog("history Q=%s cur=%s outgoing=%zu",
                        q.toHex().substr(0, 12),
                        cur.toHex().substr(0, 12),
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
            std::vector<RequestHash> * outUseful,
            TracingHash * outNextCur) -> bool
        {
            auto rsOpt = getRequestSet(rsHash);
            if (!rsOpt)
                return false;
            auto useful = usefulDispatch(*rsOpt, dispatchedSoFar);
            if (useful.empty())
                return false;
            TracingHash nextCur = cur;
            for (const auto & req : useful) {
                auto resp = dispatch(req);
                nextCur = xorFactIntoHash(nextCur, req, resp);
            }
            *outUseful = std::move(useful);
            *outNextCur = nextCur;
            return true;
        };

        for (const auto & edge : outgoing) {
            auto requestSetHash = edge.requestSet;

            std::vector<RequestHash> useful;
            TracingHash nextCur = TracingHash::zero();
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
                                q.toHex().substr(0, 12),
                                requestSetHash.toHex().substr(0, 12),
                                useful.size(),
                                nextCur.toHex().substr(0, 12));
                if (onEdgeAttempt)
                    onEdgeAttempt(/*committed=*/ false, useful);

                /* One-shot alt-fallback: if the primary's fold lands
                   at a dead-end but the row carries an altRequestSet,
                   try that. On alt hit, copy the discovered outgoing
                   state at nextCur_alt onto nextCur_primary so future
                   walks reach it via primary directly. */
                if (edge.altRequestSet) {
                    std::vector<RequestHash> altUseful;
                    TracingHash altNextCur = TracingHash::zero();
                    if (tryDispatchRs(*edge.altRequestSet, &altUseful, &altNextCur)
                        && hasAnyEdge(q, altNextCur))
                    {
                        tracingCacheLog(
                            "history Q=%s ALT-FALLBACK altRs=%s useful=%zu cur=%s -> altNextCur=%s (copying to primary %s)",
                            q.toHex().substr(0, 12),
                            edge.altRequestSet->toHex().substr(0, 12),
                            altUseful.size(),
                            cur.toHex().substr(0, 12),
                            altNextCur.toHex().substr(0, 12),
                            nextCur.toHex().substr(0, 12));
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
                            q.toHex().substr(0, 12),
                            requestSetHash.toHex().substr(0, 12),
                            useful.size(),
                            cur.toHex().substr(0, 12),
                            nextCur.toHex().substr(0, 12));
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
            advanced = true;
            break;
        }

        if (!advanced) {
            tracingCacheLog("history Q=%s NO EDGE COMMITTED at cur=%s -> miss",
                            q.toHex().substr(0, 12),
                            cur.toHex().substr(0, 12));
            return std::nullopt;
        }
    }
}

} // namespace nix
