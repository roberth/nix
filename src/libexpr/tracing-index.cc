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
    std::function<bool(const std::string & queryPayload, const NodeHash & resultNodeHash, const std::string & resultPayload)> validator)
{
    auto state(_state->lock());

    // Lambdas that query under the held lock
    auto childResults = [&](const NodeHash & after) -> std::vector<ResultNode> {
        std::vector<ResultNode> results;
        auto q = state->selectChildResults.use();
        bindBlob(q, hashToBlob(after));
        while (q.next()) {
            results.push_back(ResultNode{
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
            result.push_back(QueryNode{
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

    // Recursive walk with backtracking. At each position:
    // - Check for the target Result (queryNodeHash matches)
    // - Try advancing through each non-target Result
    // - Try advancing through depth=0 children (nested queries)
    // - Validate depth>0 children (env events) and try each
    std::function<std::optional<ResultNode>(const NodeHash &)> walk;
    walk = [&](const NodeHash & current) -> std::optional<ResultNode> {
        auto results = childResults(current);
        for (const auto & r : results) {
            if (r.queryNodeHash && *r.queryNodeHash == queryNodeHash)
                return r;  // Target Result found
        }
        // Try advancing through non-target Results
        for (const auto & r : results) {
            if (auto found = walk(r.nodeHash))
                return found;
        }

        for (const auto & child : childQueries(current)) {
            if (child.depth == 0) {
                if (auto found = walk(child.nodeHash))
                    return found;
                continue;
            }

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

} // namespace nix
