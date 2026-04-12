#include "nix/expr/tracing-index.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>
#include <condition_variable>
#include <thread>
#include <variant>

namespace nix {

static const char * schema = R"sql(
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

using WriteOp = std::variant<WriteInsertQuery, WriteInsertResult>;

// ---- Reader state (main thread) ----

struct TracingIndex::State
{
    SQLite db;

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
};

// ---- Background writer ----

struct TracingIndex::WriteQueue
{
    Sync<std::vector<WriteOp>> pending;
    std::condition_variable wakeup;
    std::thread thread;
    std::atomic<bool> done{false};

    WriteQueue(const std::filesystem::path & dbPath)
    {
        thread = std::thread([this, dbPath] { run(dbPath); });
    }

    ~WriteQueue()
    {
        done = true;
        wakeup.notify_one();
        thread.join();
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
            insertQuery.create(db,
                "INSERT OR IGNORE INTO Queries(nodeHash, queryHash, afterHash, structuralParent, depth) "
                "VALUES (?, ?, ?, ?, ?)");
            insertQueryPayload.create(db,
                "INSERT OR IGNORE INTO QueryPayloads(queryHash, payload) VALUES (?, ?)");
            insertResult.create(db,
                "INSERT OR IGNORE INTO Results(nodeHash, afterHash, payload, queryNodeHash) VALUES (?, ?, ?, ?)");
            insertShortcut.create(db,
                "INSERT OR IGNORE INTO Shortcuts(queryHash, nodeHash) VALUES (?, ?)");

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
                                    use(w.nodeHash);
                                    use(w.queryHash);
                                    if (w.afterHash)
                                        use(*w.afterHash);
                                    else
                                        use.bind();
                                    if (w.structuralParent)
                                        use(*w.structuralParent);
                                    else
                                        use.bind();
                                    use(static_cast<int64_t>(w.depth));
                                    use.exec();
                                }
                                insertQueryPayload.use()(w.queryHash)
                                    (reinterpret_cast<const unsigned char *>(w.payload.data()), w.payload.size())
                                    .exec();
                                insertShortcut.use()(w.queryHash)(w.nodeHash).exec();
                            },
                            [&](const WriteInsertResult & w) {
                                auto use = insertResult.use();
                                use(w.nodeHash);
                                use(w.afterHash);
                                use(reinterpret_cast<const unsigned char *>(w.payload.data()), w.payload.size());
                                if (w.queryNodeHash)
                                    use(*w.queryNodeHash);
                                else
                                    use.bind();
                                use.exec();
                            },
                        },
                        op);
                }
                txn.commit();
            }
        } catch (std::exception & e) {
            ignoreExceptionInDestructor();
        }
    }
};

static std::filesystem::path defaultDbPath()
{
    auto cacheDir = std::filesystem::path(getCacheDir()) / "eval-tracing-index-v1";
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
    state->db.exec(schema);

    // Prepare read statements (0..1)
    state->getQuery.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries WHERE nodeHash = ?");

    state->getResult.create(state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE nodeHash = ?");

    state->getQueryPayload.create(state->db, "SELECT payload FROM QueryPayloads WHERE queryHash = ?");

    // Prepare single-row child lookups
    state->getChildResult.create(
        state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE afterHash = ? LIMIT 1");

    // Prepare select statements (0..many)
    state->selectShortcuts.create(
        state->db, "SELECT nodeHash, createdAt FROM Shortcuts WHERE queryHash = ? ORDER BY createdAt DESC");

    state->selectChildQueries.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries WHERE afterHash = ?");

    state->selectChildResults.create(state->db, "SELECT nodeHash, afterHash, payload, queryNodeHash FROM Results WHERE afterHash = ?");

    state->selectStructuralChildren.create(
        state->db,
        "SELECT nodeHash, queryHash, afterHash, structuralParent, depth FROM Queries "
        "WHERE structuralParent = ? AND queryHash = ?");

    // Background writer opens its own connection
    _writeQueue = std::make_unique<WriteQueue>(dbPath);
}

TracingIndex::~TracingIndex()
{
    // WriteQueue destructor joins the background thread
    _writeQueue.reset();
}

// -----------------------------------------------------------------------------
// Hash computation utilities
// -----------------------------------------------------------------------------

static std::string hashToBlob(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

static Hash blobToHash(const std::string & blob)
{
    return Hash::parseAny(blob, HashAlgorithm::SHA256);
}

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

NodeHash TracingIndex::computeResultNodeHash(const NodeHash & afterHash, const std::string & payload)
{
    HashSink sink(HashAlgorithm::SHA256);
    auto astr = afterHash.to_string(HashFormat::Base16, false);
    sink << astr;
    sink << payload;
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

    _writeQueue->enqueue(WriteInsertQuery{
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
    auto nodeHash = computeResultNodeHash(afterHash, payload);

    _writeQueue->enqueue(WriteInsertResult{
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
    auto query = state->selectShortcuts.use()(hashToBlob(queryHash));

    while (query.next()) {
        result.push_back(
            ShortcutEntry{
                .nodeHash = blobToHash(query.getStr(0)),
                .createdAt = query.getInt(1),
            });
    }

    return result;
}

std::optional<QueryNode> TracingIndex::getQuery(const NodeHash & nodeHash)
{
    auto state(_state->lock());
    auto query = state->getQuery.use()(hashToBlob(nodeHash));

    if (!query.next())
        return std::nullopt;

    return QueryNode{
        .nodeHash = blobToHash(query.getStr(0)),
        .queryHash = blobToHash(query.getStr(1)),
        .afterHash = query.isNull(2) ? std::nullopt : std::optional{blobToHash(query.getStr(2))},
        .structuralParent = query.isNull(3) ? std::nullopt : std::optional{blobToHash(query.getStr(3))},
        .depth = static_cast<int>(query.getInt(4)),
    };
}

std::optional<ResultNode> TracingIndex::getResult(const NodeHash & nodeHash)
{
    auto state(_state->lock());
    auto query = state->getResult.use()(hashToBlob(nodeHash));

    if (!query.next())
        return std::nullopt;

    return ResultNode{
        .nodeHash = blobToHash(query.getStr(0)),
        .afterHash = blobToHash(query.getStr(1)),
        .payload = query.getBlob(2),
        .queryNodeHash = query.isNull(3) ? std::nullopt : std::optional{blobToHash(query.getStr(3))},
    };
}

std::optional<std::string> TracingIndex::getQueryPayload(const QueryHash & queryHash)
{
    auto state(_state->lock());
    auto query = state->getQueryPayload.use()(hashToBlob(queryHash));

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
    auto query = state->selectChildQueries.use()(hashToBlob(resultNodeHash));

    while (query.next()) {
        result.push_back(
            QueryNode{
                .nodeHash = blobToHash(query.getStr(0)),
                .queryHash = blobToHash(query.getStr(1)),
                .afterHash = query.isNull(2) ? std::nullopt : std::optional{blobToHash(query.getStr(2))},
                .structuralParent = query.isNull(3) ? std::nullopt : std::optional{blobToHash(query.getStr(3))},
                .depth = static_cast<int>(query.getInt(4)),
            });
    }

    return result;
}

std::vector<ResultNode> TracingIndex::selectChildResults(const NodeHash & afterHash)
{
    std::vector<ResultNode> result;

    auto state(_state->lock());
    auto query = state->selectChildResults.use()(hashToBlob(afterHash));

    while (query.next()) {
        result.push_back(
            ResultNode{
                .nodeHash = blobToHash(query.getStr(0)),
                .afterHash = blobToHash(query.getStr(1)),
                .payload = query.getBlob(2),
            });
    }

    return result;
}

std::optional<ResultNode> TracingIndex::getChildResult(const NodeHash & afterHash)
{
    auto state(_state->lock());
    auto query = state->getChildResult.use()(hashToBlob(afterHash));

    if (!query.next())
        return std::nullopt;

    return ResultNode{
        .nodeHash = blobToHash(query.getStr(0)),
        .afterHash = blobToHash(query.getStr(1)),
        .payload = query.getBlob(2),
        .queryNodeHash = query.isNull(3) ? std::nullopt : std::optional{blobToHash(query.getStr(3))},
    };
}

std::vector<QueryNode>
TracingIndex::selectStructuralChildren(const NodeHash & structuralParent, const QueryHash & queryHash)
{
    std::vector<QueryNode> result;

    auto state(_state->lock());
    auto query = state->selectStructuralChildren.use()(hashToBlob(structuralParent))(hashToBlob(queryHash));

    while (query.next()) {
        result.push_back(
            QueryNode{
                .nodeHash = blobToHash(query.getStr(0)),
                .queryHash = blobToHash(query.getStr(1)),
                .afterHash = query.isNull(2) ? std::nullopt : std::optional{blobToHash(query.getStr(2))},
                .structuralParent = query.isNull(3) ? std::nullopt : std::optional{blobToHash(query.getStr(3))},
                .depth = static_cast<int>(query.getInt(4)),
            });
    }

    return result;
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

} // namespace nix
