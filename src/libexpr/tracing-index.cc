#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/store/sqlite.hh"
#include <sqlite3.h>
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>
#include <variant>

namespace nix {

static const char * tracingIndexSchema = R"sql(
-- Query nodes in the trie
-- nodeHash = hash(afterHash, queryHash)
-- depth 0 = top-level evaluation queries
-- depth 1 = environment events (file reads, env vars, ambient outgoing)
-- depth 2 = ambient incoming (external accessing local values during callbacks)
CREATE TABLE IF NOT EXISTS Queries (
    nodeHash BLOB PRIMARY KEY,
    queryHash BLOB NOT NULL,
    afterHash BLOB,
    structuralParent BLOB,
    depth INTEGER NOT NULL DEFAULT 0
);

-- Query payloads (cold storage for inspection)
CREATE TABLE IF NOT EXISTS QueryPayloads (
    queryHash BLOB PRIMARY KEY,
    payload BLOB NOT NULL
);

-- Result nodes
-- nodeHash = hash(afterHash, payload)
-- queryNodeHash links this Result to the Query it answers
CREATE TABLE IF NOT EXISTS Results (
    nodeHash BLOB PRIMARY KEY,
    afterHash BLOB NOT NULL,
    payload BLOB NOT NULL,
    queryNodeHash BLOB
);

-- Shortcut index: queryHash → nodeHash for fast lookup
CREATE TABLE IF NOT EXISTS Shortcuts (
    queryHash BLOB NOT NULL,
    nodeHash BLOB NOT NULL,
    createdAt INTEGER DEFAULT (unixepoch()),
    PRIMARY KEY (queryHash, nodeHash)
);

-- Indexes for shortcut lookup (by queryHash)
CREATE INDEX IF NOT EXISTS ShortcutsQueryHash ON Shortcuts(queryHash);

-- Indexes for forward traversal (temporal trie)
CREATE INDEX IF NOT EXISTS QueriesAfter ON Queries(afterHash);
CREATE INDEX IF NOT EXISTS ResultsAfter ON Results(afterHash);

-- Index for structural lookup
CREATE INDEX IF NOT EXISTS QueriesStructural ON Queries(structuralParent, queryHash);

-- Sets-based index (see doc/tracing-sets-index-data-model.md).
-- PreconditionSets: content-addressed unordered sets of
--   (d>0 queryHash, d>0 responseHash) pairs. `members` is the CBOR
--   encoding of those pairs in queryHash-sorted order; `setHash` is
--   the SHA-256 of `members`.
CREATE TABLE IF NOT EXISTS PreconditionSets (
    setHash BLOB PRIMARY KEY,
    members BLOB NOT NULL
);

-- SetResponses: response payloads referenced by Bindings.
-- responseHash = SHA-256 of the CBOR payload.
CREATE TABLE IF NOT EXISTS SetResponses (
    responseHash BLOB PRIMARY KEY,
    payload BLOB NOT NULL
);

-- Bindings: for each granular Query (queryHash), the recorded
-- Response when its precondition holds.
CREATE TABLE IF NOT EXISTS Bindings (
    queryHash BLOB NOT NULL,
    preconditionHash BLOB NOT NULL,
    responseHash BLOB NOT NULL,
    createdAt INTEGER DEFAULT (unixepoch()),
    PRIMARY KEY (queryHash, preconditionHash)
);

CREATE INDEX IF NOT EXISTS BindingsByQueryHash ON Bindings(queryHash);
)sql";

// ---- Write operations queued for background persistence ----

struct WriteInsertQuery
{
    std::string nodeHash;
    std::string queryHash;
    std::optional<std::string> afterHash;
    std::optional<std::string> structuralParent;
    int depth;
    std::string payload; // CBOR
};

struct WriteInsertResult
{
    std::string nodeHash;
    std::string afterHash;
    std::string payload; // CBOR
    std::optional<std::string> queryNodeHash;
};

struct WriteInsertPreconditionSet
{
    std::string setHash;
    std::string members; // binary: N × (queryHash || responseHash)
};

struct WriteInsertSetResponse
{
    std::string responseHash;
    std::string payload;
};

struct WriteInsertBinding
{
    std::string queryHash;
    std::string preconditionHash;
    std::string responseHash;
};

using WriteOp = std::
    variant<WriteInsertQuery, WriteInsertResult, WriteInsertPreconditionSet, WriteInsertSetResponse, WriteInsertBinding>;

// ---- Reader state (main thread) ----

struct TracingIndex::State
{
    SQLite db;

    /**
     * Ensure reader sees data committed by the background writer.
     * WAL mode doesn't automatically make writer-committed data
     * visible to a reader on a different connection in the same
     * process. A passive checkpoint transfers WAL data to the
     * main DB file without blocking.
     */
    void checkpoint()
    {
        db.exec("PRAGMA wal_checkpoint(PASSIVE)");
    }

    // Query statements (0..1)
    SQLiteStmt getQuery;
    SQLiteStmt getResult;
    SQLiteStmt getQueryPayload;

    // Single-row child lookups
    SQLiteStmt getChildResult;

    // Select statements (0..many)
    SQLiteStmt selectShortcuts;
    SQLiteStmt selectChildQueries;
    SQLiteStmt selectChildResults;
    SQLiteStmt selectStructuralChildren;

    // Sets-based index statements
    SQLiteStmt getPreconditionSet;
    SQLiteStmt getSetResponse;
    SQLiteStmt selectBindings;
};

// ---- Hash ↔ blob helpers ----

static std::string hashToBlob(const Hash & h)
{
    return std::string(reinterpret_cast<const char *>(h.hash), h.hashSize);
}

static Hash blobToHash(const std::string & blob)
{
    Hash h(HashAlgorithm::SHA256);
    if (blob.size() != h.hashSize)
        throw Error("invalid hash blob size %d (expected %d)", blob.size(), h.hashSize);
    std::memcpy(h.hash, blob.data(), h.hashSize);
    return h;
}

static SQLiteStmt::Use & bindBlob(SQLiteStmt::Use & use, const std::string & blob)
{
    return use(reinterpret_cast<const unsigned char *>(blob.data()), blob.size());
}

static Hash readHash(SQLiteStmt::Use & query, int col)
{
    return blobToHash(query.getBlob(col));
}

static std::optional<Hash> readHashOpt(SQLiteStmt::Use & query, int col)
{
    if (query.isNull(col))
        return std::nullopt;
    return blobToHash(query.getBlob(col));
}

// ---- Background writer ----

struct TracingIndex::WriteQueue
{
    Sync<std::vector<WriteOp>> pending;
    std::condition_variable wakeup;
    std::thread thread;
    std::atomic<bool> done{false};

    /**
     * Global registry of active WriteQueues. Ensures all background writer
     * threads are joined and data is flushed before process exit, even when
     * the owning TracingIndex is GC-allocated and its destructor may not run.
     */
    static Sync<std::set<WriteQueue *>> & registry()
    {
        // Intentionally leaked: the registry must outlive every other static
        // and every atexit handler, including flushAll (registered below).
        // A function-local `static Sync<...> instance;` would have its
        // destructor registered alongside atexit handlers and could run
        // before flushAll, leaving flushAll to iterate a destroyed std::set
        // and crash in std::_Rb_tree_increment.
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

    WriteQueue(const std::filesystem::path & dbPath)
    {
        static std::once_flag atexitRegistered;
        std::call_once(atexitRegistered, [] { std::atexit(flushAll); });
        thread = std::thread([this, dbPath] { run(dbPath); });
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
            thread.join();
        }
    }

    void enqueue(WriteOp op)
    {
        {
            auto q = pending.lock();
            q->push_back(std::move(op));
        }
        wakeup.notify_one();
    }

private:
    void run(const std::filesystem::path & dbPath)
    {
        try {
            SQLite db(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
            db.isCache();

            SQLiteStmt insertQuery, insertQueryPayload, insertResult, insertShortcut;
            SQLiteStmt insertPreconditionSet, insertSetResponse, insertBinding;
            insertQuery.create(
                db,
                "INSERT OR IGNORE INTO Queries(nodeHash, queryHash, afterHash, structuralParent, depth) "
                "VALUES (?, ?, ?, ?, ?)");
            insertQueryPayload.create(db, "INSERT OR IGNORE INTO QueryPayloads(queryHash, payload) VALUES (?, ?)");
            insertResult.create(
                db, "INSERT OR IGNORE INTO Results(nodeHash, afterHash, payload, queryNodeHash) VALUES (?, ?, ?, ?)");
            insertShortcut.create(db, "INSERT OR IGNORE INTO Shortcuts(queryHash, nodeHash) VALUES (?, ?)");
            insertPreconditionSet.create(
                db, "INSERT OR IGNORE INTO PreconditionSets(setHash, members) VALUES (?, ?)");
            insertSetResponse.create(db, "INSERT OR IGNORE INTO SetResponses(responseHash, payload) VALUES (?, ?)");
            insertBinding.create(
                db,
                "INSERT OR IGNORE INTO Bindings(queryHash, preconditionHash, responseHash) "
                "VALUES (?, ?, ?)");

            while (true) {
                std::vector<WriteOp> batch;
                {
                    auto q = pending.lock();
                    while (q->empty() && !done.load())
                        q.wait(wakeup);
                    batch = std::move(*q);
                    q->clear();
                }

                if (batch.empty() && done.load())
                    break;

                if (batch.empty())
                    continue;

                SQLiteTxn txn(db);
                for (auto & op : batch) {
                    std::visit(
                        overloaded{
                            [&](const WriteInsertQuery & w) {
                                {
                                    auto use = insertQuery.use();
                                    bindBlob(use, w.nodeHash);
                                    bindBlob(use, w.queryHash);
                                    if (w.afterHash)
                                        bindBlob(use, *w.afterHash);
                                    else
                                        use.bind();
                                    if (w.structuralParent)
                                        bindBlob(use, *w.structuralParent);
                                    else
                                        use.bind();
                                    use(static_cast<int64_t>(w.depth));
                                    use.exec();
                                }
                                {
                                    auto use = insertQueryPayload.use();
                                    bindBlob(use, w.queryHash);
                                    use(reinterpret_cast<const unsigned char *>(w.payload.data()), w.payload.size());
                                    use.exec();
                                }
                                {
                                    auto use = insertShortcut.use();
                                    bindBlob(use, w.queryHash);
                                    bindBlob(use, w.nodeHash);
                                    use.exec();
                                }
                            },
                            [&](const WriteInsertResult & w) {
                                auto use = insertResult.use();
                                bindBlob(use, w.nodeHash);
                                bindBlob(use, w.afterHash);
                                use(reinterpret_cast<const unsigned char *>(w.payload.data()), w.payload.size());
                                if (w.queryNodeHash)
                                    bindBlob(use, *w.queryNodeHash);
                                else
                                    use.bind();
                                use.exec();
                            },
                            [&](const WriteInsertPreconditionSet & w) {
                                auto use = insertPreconditionSet.use();
                                bindBlob(use, w.setHash);
                                use(reinterpret_cast<const unsigned char *>(w.members.data()), w.members.size());
                                use.exec();
                            },
                            [&](const WriteInsertSetResponse & w) {
                                auto use = insertSetResponse.use();
                                bindBlob(use, w.responseHash);
                                use(reinterpret_cast<const unsigned char *>(w.payload.data()), w.payload.size());
                                use.exec();
                            },
                            [&](const WriteInsertBinding & w) {
                                auto use = insertBinding.use();
                                bindBlob(use, w.queryHash);
                                bindBlob(use, w.preconditionHash);
                                bindBlob(use, w.responseHash);
                                use.exec();
                            },
                        },
                        op);
                }
                txn.commit();

                // Checkpoint WAL into the main DB so other processes
                // (and the in-process reader connection) see committed
                // data immediately. EvalState is GC-allocated so its
                // destructor may never run — we can't defer this to
                // shutdown.
                db.exec("PRAGMA wal_checkpoint(PASSIVE)");
            }
        } catch (std::exception & e) {
            ignoreExceptionInDestructor();
        }
    }
};

static std::filesystem::path defaultDbPath()
{
    auto cacheDir = std::filesystem::path(getCacheDir()) / "eval-tracing-index-v2";
    return cacheDir / "index.sqlite";
}

TracingIndex::TracingIndex()
    : TracingIndex(defaultDbPath())
{
}

TracingIndex::TracingIndex(const std::filesystem::path & dbPath)
    : _state(std::make_unique<Sync<State>>())
{
    if (dbPath.has_parent_path()) {
        createDirs(dbPath.parent_path());
    }

    auto state(_state->lock());

    debug("opening tracing index: %s", dbPath.string());
    state->db = SQLite(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
    state->db.isCache();
    state->db.exec(tracingIndexSchema);

    // Prepare read statements (0..1)
    state->getQuery.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries WHERE nodeHash = ?");

    state->getResult.create(
        state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE nodeHash = ?");

    state->getQueryPayload.create(state->db, "SELECT payload FROM QueryPayloads WHERE queryHash = ?");

    // Prepare single-row child lookups
    state->getChildResult.create(
        state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE afterHash = ? LIMIT 1");

    // Prepare select statements (0..many)
    state->selectShortcuts.create(
        state->db, "SELECT nodeHash, createdAt FROM Shortcuts WHERE queryHash = ? ORDER BY createdAt DESC");

    state->selectChildQueries.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries WHERE afterHash = ?");

    state->selectChildResults.create(
        state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE afterHash = ?");

    state->selectStructuralChildren.create(
        state->db,
        "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries "
        "WHERE structuralParent = ? AND queryHash = ?");

    state->getPreconditionSet.create(state->db, "SELECT members FROM PreconditionSets WHERE setHash = ?");
    state->getSetResponse.create(state->db, "SELECT payload FROM SetResponses WHERE responseHash = ?");
    state->selectBindings.create(
        state->db,
        "SELECT preconditionHash, responseHash FROM Bindings WHERE queryHash = ? ORDER BY createdAt DESC");

    // Background writer opens its own connection
    _writeQueue = std::make_unique<WriteQueue>(dbPath);
}

TracingIndex::~TracingIndex()
{
    // WriteQueue destructor joins the background thread
    _writeQueue.reset();
}

void TracingIndex::flushAllWriteQueues()
{
    WriteQueue::flushAll();
}

// (hash utilities moved above WriteQueue)

NodeHash TracingIndex::computeQueryNodeHash(const std::optional<NodeHash> & afterHash, const QueryHash & queryHash)
{
    HashSink sink(HashAlgorithm::SHA256);
    if (afterHash) {
        auto s = afterHash->to_string(HashFormat::Base16, false);
        sink << s;
    } else {
        std::string_view root = "root";
        sink << root;
    }
    auto qstr = queryHash.to_string(HashFormat::Base16, false);
    sink << qstr;
    return sink.finish().hash;
}

NodeHash TracingIndex::computeResultNodeHash(
    const NodeHash & afterHash, const std::string & payload, const std::optional<NodeHash> & queryNodeHash)
{
    HashSink sink(HashAlgorithm::SHA256);
    auto astr = afterHash.to_string(HashFormat::Base16, false);
    sink << astr;
    sink << payload;
    if (queryNodeHash) {
        auto qstr = queryNodeHash->to_string(HashFormat::Base16, false);
        sink << qstr;
    }
    return sink.finish().hash;
}

// -----------------------------------------------------------------------------
// Recording operations
// -----------------------------------------------------------------------------

NodeHash TracingIndex::insertQuery(
    const std::optional<NodeHash> & afterHash,
    const QueryHash & queryHash,
    const std::string & payload,
    const std::optional<NodeHash> & structuralParent,
    int depth)
{
    auto nodeHash = computeQueryNodeHash(afterHash, queryHash);

    _writeQueue->enqueue(
        WriteInsertQuery{
            .nodeHash = hashToBlob(nodeHash),
            .queryHash = hashToBlob(queryHash),
            .afterHash = afterHash ? std::optional{hashToBlob(*afterHash)} : std::nullopt,
            .structuralParent = structuralParent ? std::optional{hashToBlob(*structuralParent)} : std::nullopt,
            .depth = depth,
            .payload = payload,
        });

    return nodeHash;
}

NodeHash TracingIndex::insertResult(
    const NodeHash & afterHash, const std::string & payload, const std::optional<NodeHash> & queryNodeHash)
{
    auto nodeHash = computeResultNodeHash(afterHash, payload, queryNodeHash);

    _writeQueue->enqueue(
        WriteInsertResult{
            .nodeHash = hashToBlob(nodeHash),
            .afterHash = hashToBlob(afterHash),
            .payload = payload,
            .queryNodeHash = queryNodeHash ? std::optional{hashToBlob(*queryNodeHash)} : std::nullopt,
        });

    return nodeHash;
}

// -----------------------------------------------------------------------------
// Query operations
// -----------------------------------------------------------------------------

std::vector<ShortcutEntry> TracingIndex::selectShortcuts(const QueryHash & queryHash)
{
    std::vector<ShortcutEntry> result;

    auto state(_state->lock());
    state->checkpoint();
    auto query = state->selectShortcuts.use();
    bindBlob(query, hashToBlob(queryHash));

    while (query.next()) {
        result.push_back(
            ShortcutEntry{
                .nodeHash = readHash(query, 0),
                .createdAt = query.getInt(1),
            });
    }

    return result;
}

std::optional<QueryNode> TracingIndex::getQuery(const NodeHash & nodeHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto query = state->getQuery.use();
    bindBlob(query, hashToBlob(nodeHash));

    if (!query.next())
        return std::nullopt;

    return QueryNode{
        .nodeHash = readHash(query, 0),
        .queryHash = readHash(query, 1),
        .afterHash = readHashOpt(query, 2),
        .structuralParent = readHashOpt(query, 3),
        .depth = static_cast<int>(query.getInt(4)),
    };
}

std::optional<ResultNode> TracingIndex::getResult(const NodeHash & nodeHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto query = state->getResult.use();
    bindBlob(query, hashToBlob(nodeHash));

    if (!query.next())
        return std::nullopt;

    return ResultNode{
        .nodeHash = readHash(query, 0),
        .afterHash = readHash(query, 1),
        .payload = query.getBlob(2),
        .queryNodeHash = readHashOpt(query, 3),
    };
}

std::optional<std::string> TracingIndex::getQueryPayload(const QueryHash & queryHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto query = state->getQueryPayload.use();
    bindBlob(query, hashToBlob(queryHash));

    if (!query.next())
        return std::nullopt;

    return query.getBlob(0);
}

// -----------------------------------------------------------------------------
// Select operations (0..many)
// -----------------------------------------------------------------------------

std::vector<QueryNode> TracingIndex::selectChildQueries(const NodeHash & resultNodeHash)
{
    std::vector<QueryNode> result;

    auto state(_state->lock());
    state->checkpoint();
    auto query = state->selectChildQueries.use();
    bindBlob(query, hashToBlob(resultNodeHash));

    while (query.next()) {
        result.push_back(
            QueryNode{
                .nodeHash = readHash(query, 0),
                .queryHash = readHash(query, 1),
                .afterHash = readHashOpt(query, 2),
                .structuralParent = readHashOpt(query, 3),
                .depth = static_cast<int>(query.getInt(4)),
            });
    }

    return result;
}

std::vector<ResultNode> TracingIndex::selectChildResults(const NodeHash & afterHash)
{
    std::vector<ResultNode> result;

    auto state(_state->lock());
    state->checkpoint();
    auto query = state->selectChildResults.use();
    bindBlob(query, hashToBlob(afterHash));

    while (query.next()) {
        result.push_back(
            ResultNode{
                .nodeHash = readHash(query, 0),
                .afterHash = readHash(query, 1),
                .payload = query.getBlob(2),
            });
    }

    return result;
}

std::optional<ResultNode> TracingIndex::getChildResult(const NodeHash & afterHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto query = state->getChildResult.use();
    bindBlob(query, hashToBlob(afterHash));

    if (!query.next())
        return std::nullopt;

    return ResultNode{
        .nodeHash = readHash(query, 0),
        .afterHash = readHash(query, 1),
        .payload = query.getBlob(2),
        .queryNodeHash = readHashOpt(query, 3),
    };
}

std::vector<QueryNode>
TracingIndex::selectStructuralChildren(const NodeHash & structuralParent, const QueryHash & queryHash)
{
    std::vector<QueryNode> result;

    auto state(_state->lock());
    state->checkpoint();
    auto query = state->selectStructuralChildren.use();
    bindBlob(query, hashToBlob(structuralParent));
    bindBlob(query, hashToBlob(queryHash));

    while (query.next()) {
        result.push_back(
            QueryNode{
                .nodeHash = readHash(query, 0),
                .queryHash = readHash(query, 1),
                .afterHash = readHashOpt(query, 2),
                .structuralParent = readHashOpt(query, 3),
                .depth = static_cast<int>(query.getInt(4)),
            });
    }

    return result;
}

std::optional<ResultNode> TracingIndex::findResult(
    const NodeHash & queryNodeHash,
    std::function<
        bool(const std::string & queryPayload, const NodeHash & resultNodeHash, const std::string & resultPayload)>
        validator)
{
    auto state(_state->lock());
    state->checkpoint();

    // Lambdas that query under the held lock
    auto childResults = [&](const NodeHash & after) -> std::vector<ResultNode> {
        std::vector<ResultNode> results;
        auto q = state->selectChildResults.use();
        bindBlob(q, hashToBlob(after));
        while (q.next()) {
            results.push_back(
                ResultNode{
                    .nodeHash = readHash(q, 0),
                    .afterHash = readHash(q, 1),
                    .payload = q.getBlob(2),
                    .queryNodeHash = readHashOpt(q, 3),
                });
        }
        return results;
    };

    auto childQueries = [&](const NodeHash & after) -> std::vector<QueryNode> {
        std::vector<QueryNode> result;
        auto q = state->selectChildQueries.use();
        bindBlob(q, hashToBlob(after));
        while (q.next()) {
            result.push_back(
                QueryNode{
                    .nodeHash = readHash(q, 0),
                    .queryHash = readHash(q, 1),
                    .afterHash = readHashOpt(q, 2),
                    .structuralParent = readHashOpt(q, 3),
                    .depth = static_cast<int>(q.getInt(4)),
                });
        }
        return result;
    };

    auto queryPayload = [&](const QueryHash & qh) -> std::optional<std::string> {
        auto q = state->getQueryPayload.use();
        bindBlob(q, hashToBlob(qh));
        if (!q.next())
            return std::nullopt;
        return q.getBlob(0);
    };

    auto queryByNodeHash = [&](const NodeHash & nh) -> std::optional<QueryNode> {
        auto q = state->getQuery.use();
        bindBlob(q, hashToBlob(nh));
        if (!q.next())
            return std::nullopt;
        return QueryNode{
            .nodeHash = readHash(q, 0),
            .queryHash = readHash(q, 1),
            .afterHash = readHashOpt(q, 2),
            .structuralParent = readHashOpt(q, 3),
            .depth = static_cast<int>(q.getInt(4)),
        };
    };

    /* Recursive forward walk with backtracking. The walker has to
       reach the target Result, which may sit several pass-through
       nodes downstream of the shortcut start (the recording layer
       can insert intermediate d=0 Query markers and Results
       belonging to other queries between a Query and its own
       Result). The walker traverses those pass-through nodes
       transparently while still validating every d>0 environment
       event it encounters on the path. The `seen` set caps the
       worst case at O(N) — three traversal branches without it
       can revisit nodes combinatorially on dense subtries. */
    std::unordered_set<NodeHash> seen;
    std::function<std::optional<ResultNode>(const NodeHash &)> walk;
    walk = [&](const NodeHash & current) -> std::optional<ResultNode> {
        if (!seen.insert(current).second)
            return std::nullopt;

        auto results = childResults(current);

        // Target Result is a direct child — exact match wins.
        for (const auto & r : results) {
            if (r.queryNodeHash && *r.queryNodeHash == queryNodeHash)
                return r;
        }

        auto queries = childQueries(current);

        /* d>0 child Queries: each represents an environment event
           that has to validate before we can traverse its branch. */
        for (const auto & child : queries) {
            if (child.depth == 0)
                continue;
            auto payload = queryPayload(child.queryHash);
            if (!payload)
                continue;
            for (const auto & eventResult : childResults(child.nodeHash)) {
                if (validator(*payload, eventResult.nodeHash, eventResult.payload)) {
                    if (auto found = walk(eventResult.nodeHash))
                        return found;
                }
            }
        }

        /* d=0 child Queries are pure trie position markers — not
           environment events. Walk through them transparently. */
        for (const auto & child : queries) {
            if (child.depth != 0)
                continue;
            if (auto found = walk(child.nodeHash))
                return found;
        }

        /* Non-target child Results: chained Results from other
           queries. If the owning Query is d>0 the Result represents
           an environment event and must be validated like an
           ordinary d>0 child; if the owning Query is d=0 (or
           absent) the Result is a pass-through and we just
           traverse. */
        for (const auto & r : results) {
            if (r.queryNodeHash && *r.queryNodeHash == queryNodeHash)
                continue; // handled by target check above

            bool validated = true;
            if (r.queryNodeHash) {
                if (auto owner = queryByNodeHash(*r.queryNodeHash); owner && owner->depth > 0) {
                    auto payload = queryPayload(owner->queryHash);
                    validated = payload && validator(*payload, r.nodeHash, r.payload);
                }
            }
            if (!validated)
                continue;
            if (auto found = walk(r.nodeHash))
                return found;
        }

        return std::nullopt;
    };

    return walk(queryNodeHash);
}

std::vector<std::pair<QueryNode, ResultNode>> TracingIndex::selectDependencies(const NodeHash & queryNodeHash)
{
    std::vector<std::pair<QueryNode, ResultNode>> result;

    // Start from the query node and trace back to root,
    // collecting depth>0 Query/Result pairs (environment events).
    auto queryNode = getQuery(queryNodeHash);
    if (!queryNode || !queryNode->afterHash)
        return result;

    std::optional<NodeHash> current = queryNode->afterHash;

    while (current) {
        // Try Result (predecessor of a query that follows another query's result)
        if (auto res = getResult(current.value())) {
            // If the result answers a depth>0 query, collect the pair
            if (res->queryNodeHash) {
                if (auto q = getQuery(*res->queryNodeHash)) {
                    if (q->depth > 0)
                        result.emplace_back(*q, *res);
                }
            }
            current = res->afterHash;
            continue;
        }

        // Try Query (skip through it to its afterHash)
        if (auto q = getQuery(current.value())) {
            current = q->afterHash;
            continue;
        }

        // Unknown node type, stop traversal
        break;
    }

    // Reverse to get root-to-query order
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<std::pair<QueryNode, ResultNode>> TracingIndex::selectDependenciesUntilValidated(
    const NodeHash & queryNodeHash, const std::set<NodeHash> & validatedNodes, bool & reachedValidated)
{
    std::vector<std::pair<QueryNode, ResultNode>> result;
    reachedValidated = false;

    auto queryNode = getQuery(queryNodeHash);
    if (!queryNode || !queryNode->afterHash)
        return result;

    std::optional<NodeHash> current = queryNode->afterHash;

    while (current) {
        // Check if we've reached a validated node
        if (validatedNodes.count(*current)) {
            reachedValidated = true;
            break;
        }

        if (auto res = getResult(current.value())) {
            if (validatedNodes.count(res->nodeHash)) {
                reachedValidated = true;
                break;
            }
            // If the result answers a depth>0 query, collect the pair
            if (res->queryNodeHash) {
                if (auto q = getQuery(*res->queryNodeHash)) {
                    if (q->depth > 0)
                        result.emplace_back(*q, *res);
                }
            }
            current = res->afterHash;
            continue;
        }

        if (auto q = getQuery(current.value())) {
            if (validatedNodes.count(q->nodeHash)) {
                reachedValidated = true;
                break;
            }
            current = q->afterHash;
            continue;
        }

        break;
    }

    // Reverse to get validated-to-query order
    std::reverse(result.begin(), result.end());
    return result;
}

// -----------------------------------------------------------------------------
// Sets-based index
// -----------------------------------------------------------------------------

namespace {

/* On-disk format for a PreconditionSet.members blob:
   concatenation of N records, each (queryHashSize + responseHashSize)
   bytes. Both hashes are SHA-256 (32 bytes). Records are sorted
   ascending by queryHash bytes, then by responseHash bytes.

   Format chosen for trivial iteration (read fixed-size records) and
   trivial sortedness checking (lexicographic on the raw bytes). */

constexpr size_t kPreconditionRecordSize = 64; // 32 + 32

std::string serializeMembers(const TracingIndex::SetMembers & members)
{
    std::string out;
    out.reserve(members.size() * kPreconditionRecordSize);
    for (const auto & m : members) {
        auto qb = hashToBlob(m.queryHash);
        auto rb = hashToBlob(m.responseHash);
        assert(qb.size() == 32 && rb.size() == 32);
        out.append(qb);
        out.append(rb);
    }
    return out;
}

TracingIndex::SetMembers deserializeMembers(const std::string & blob)
{
    if (blob.size() % kPreconditionRecordSize != 0)
        throw Error("corrupt PreconditionSet members blob: size %zu not a multiple of %zu",
                    blob.size(), kPreconditionRecordSize);
    TracingIndex::SetMembers out;
    out.reserve(blob.size() / kPreconditionRecordSize);
    for (size_t off = 0; off < blob.size(); off += kPreconditionRecordSize) {
        out.push_back(TracingIndex::SetMember{
            .queryHash = blobToHash(blob.substr(off, 32)),
            .responseHash = blobToHash(blob.substr(off + 32, 32)),
        });
    }
    return out;
}

} // namespace

Hash TracingIndex::computePreconditionSetHash(const SetMembers & members)
{
    HashSink sink(HashAlgorithm::SHA256);
    auto blob = serializeMembers(members);
    sink(std::string_view(blob));
    return sink.finish().hash;
}

Hash TracingIndex::computeResponseHash(const std::string & payload)
{
    HashSink sink(HashAlgorithm::SHA256);
    sink(std::string_view(payload));
    return sink.finish().hash;
}

Hash TracingIndex::insertPreconditionSet(const SetMembers & members)
{
    auto setHash = computePreconditionSetHash(members);
    _writeQueue->enqueue(WriteInsertPreconditionSet{
        .setHash = hashToBlob(setHash),
        .members = serializeMembers(members),
    });
    return setHash;
}

std::optional<TracingIndex::SetMembers> TracingIndex::getPreconditionSet(const Hash & setHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto q = state->getPreconditionSet.use();
    bindBlob(q, hashToBlob(setHash));
    if (!q.next())
        return std::nullopt;
    return deserializeMembers(q.getBlob(0));
}

Hash TracingIndex::insertSetResponse(const std::string & payload)
{
    auto responseHash = computeResponseHash(payload);
    _writeQueue->enqueue(WriteInsertSetResponse{
        .responseHash = hashToBlob(responseHash),
        .payload = payload,
    });
    return responseHash;
}

std::optional<std::string> TracingIndex::getSetResponse(const Hash & responseHash)
{
    auto state(_state->lock());
    state->checkpoint();
    auto q = state->getSetResponse.use();
    bindBlob(q, hashToBlob(responseHash));
    if (!q.next())
        return std::nullopt;
    return q.getBlob(0);
}

void TracingIndex::insertBinding(
    const QueryHash & queryHash, const Hash & preconditionHash, const Hash & responseHash)
{
    _writeQueue->enqueue(WriteInsertBinding{
        .queryHash = hashToBlob(queryHash),
        .preconditionHash = hashToBlob(preconditionHash),
        .responseHash = hashToBlob(responseHash),
    });
}

bool TracingIndex::isSubset(const SetMembers & precondition, const SetMembers & current)
{
    /* Linear two-pointer merge. Both inputs are sorted ascending by
       queryHash (and by responseHash within equal queryHashes). For
       every entry in `precondition` we need an entry in `current`
       with the same queryHash AND the same responseHash. */
    size_t i = 0, j = 0;
    while (i < precondition.size()) {
        if (j >= current.size())
            return false;
        if (precondition[i].queryHash == current[j].queryHash) {
            if (precondition[i].responseHash != current[j].responseHash)
                return false;
            ++i;
            ++j;
        } else if (current[j].queryHash < precondition[i].queryHash) {
            ++j;
        } else {
            return false;
        }
    }
    return true;
}

std::optional<std::string> TracingIndex::lookupSetsReplay(const QueryHash & queryHash, const SetMembers & current)
{
    std::vector<std::pair<Hash, Hash>> candidates;
    {
        auto state(_state->lock());
        state->checkpoint();
        auto q = state->selectBindings.use();
        bindBlob(q, hashToBlob(queryHash));
        while (q.next())
            candidates.emplace_back(readHash(q, 0), readHash(q, 1));
    }

    for (const auto & [preconditionHash, responseHash] : candidates) {
        auto pre = getPreconditionSet(preconditionHash);
        if (!pre)
            continue;
        if (isSubset(*pre, current))
            return getSetResponse(responseHash);
    }
    return std::nullopt;
}

} // namespace nix
