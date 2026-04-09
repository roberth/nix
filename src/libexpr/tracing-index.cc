#include "nix/expr/tracing-index.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>

namespace nix {

static const char * schema = R"sql(
-- Query nodes in the trie
-- nodeHash = hash(afterHash, queryHash)
CREATE TABLE IF NOT EXISTS Queries (
    nodeHash BLOB PRIMARY KEY,
    queryHash BLOB NOT NULL,
    afterHash BLOB,
    structuralParent BLOB
);

-- Query payloads (cold storage for inspection)
CREATE TABLE IF NOT EXISTS QueryPayloads (
    queryHash BLOB PRIMARY KEY,
    payload BLOB NOT NULL
);

-- Response nodes (Request/Response pairs)
-- nodeHash = hash(afterHash, request, response)
CREATE TABLE IF NOT EXISTS Responses (
    nodeHash BLOB PRIMARY KEY,
    afterHash BLOB NOT NULL,
    request BLOB NOT NULL,
    response BLOB NOT NULL
);

-- Result nodes
-- nodeHash = hash(afterHash, payload)
CREATE TABLE IF NOT EXISTS Results (
    nodeHash BLOB PRIMARY KEY,
    afterHash BLOB NOT NULL,
    payload BLOB NOT NULL
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
CREATE INDEX IF NOT EXISTS ResponsesAfter ON Responses(afterHash, request);
CREATE INDEX IF NOT EXISTS ResultsAfter ON Results(afterHash);

-- Index for structural lookup
CREATE INDEX IF NOT EXISTS QueriesStructural ON Queries(structuralParent, queryHash);
)sql";

struct TracingIndex::State
{
    SQLite db;

    // Insert statements
    SQLiteStmt insertQuery;
    SQLiteStmt insertQueryPayload;
    SQLiteStmt insertResponse;
    SQLiteStmt insertResult;
    SQLiteStmt insertShortcut;

    // Query statements (0..1)
    SQLiteStmt getQuery;
    SQLiteStmt getResponse;
    SQLiteStmt getResult;
    SQLiteStmt getQueryPayload;

    // Single-row child lookups
    SQLiteStmt getChildResult;
    SQLiteStmt getChildRequests;

    // Select statements (0..many)
    SQLiteStmt selectShortcuts;
    SQLiteStmt selectChildQueries;
    SQLiteStmt selectChildResponses;
    SQLiteStmt selectChildResults;
    SQLiteStmt selectStructuralChildren;
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

    // Prepare insert statements
    state->insertQuery.create(
        state->db,
        "INSERT OR IGNORE INTO Queries(nodeHash, queryHash, afterHash, structuralParent) "
        "VALUES (?, ?, ?, ?)");

    state->insertQueryPayload.create(
        state->db, "INSERT OR IGNORE INTO QueryPayloads(queryHash, payload) VALUES (?, ?)");

    state->insertResponse.create(
        state->db,
        "INSERT OR IGNORE INTO Responses(nodeHash, afterHash, request, response) "
        "VALUES (?, ?, ?, ?)");

    state->insertResult.create(
        state->db, "INSERT OR IGNORE INTO Results(nodeHash, afterHash, payload) VALUES (?, ?, ?)");

    state->insertShortcut.create(state->db, "INSERT OR IGNORE INTO Shortcuts(queryHash, nodeHash) VALUES (?, ?)");

    // Prepare get statements (0..1)
    state->getQuery.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent FROM Queries WHERE nodeHash = ?");

    state->getResponse.create(
        state->db, "SELECT nodeHash, afterHash, request, response FROM Responses WHERE nodeHash = ?");

    state->getResult.create(state->db, "SELECT nodeHash, afterHash, payload FROM Results WHERE nodeHash = ?");

    state->getQueryPayload.create(state->db, "SELECT payload FROM QueryPayloads WHERE queryHash = ?");

    // Prepare single-row child lookups
    state->getChildResult.create(
        state->db, "SELECT nodeHash, afterHash, payload FROM Results WHERE afterHash = ? LIMIT 1");
    state->getChildRequests.create(
        state->db, "SELECT DISTINCT request FROM Responses WHERE afterHash = ?");

    // Prepare select statements (0..many)
    state->selectShortcuts.create(
        state->db, "SELECT nodeHash, createdAt FROM Shortcuts WHERE queryHash = ? ORDER BY createdAt DESC");

    state->selectChildQueries.create(
        state->db, "SELECT nodeHash, queryHash, afterHash, structuralParent FROM Queries WHERE afterHash = ?");

    state->selectChildResponses.create(
        state->db, "SELECT nodeHash, afterHash, request, response FROM Responses WHERE afterHash = ?");

    state->selectChildResults.create(state->db, "SELECT nodeHash, afterHash, payload FROM Results WHERE afterHash = ?");

    state->selectStructuralChildren.create(
        state->db,
        "SELECT nodeHash, queryHash, afterHash, structuralParent FROM Queries "
        "WHERE structuralParent = ? AND queryHash = ?");
}

TracingIndex::~TracingIndex() = default;

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

NodeHash TracingIndex::computeResponseNodeHash(
    const NodeHash & afterHash, const std::string & request, const std::string & response)
{
    HashSink sink(HashAlgorithm::SHA256);
    auto astr = afterHash.to_string(HashFormat::Base16, false);
    sink << astr;
    sink << request;
    sink << response;
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
    const std::optional<NodeHash> & structuralParent)
{
    auto nodeHash = computeQueryNodeHash(afterHash, queryHash);

    auto state(_state->lock());

    // Insert query node
    auto use = state->insertQuery.use();
    use(hashToBlob(nodeHash));
    use(hashToBlob(queryHash));
    if (afterHash)
        use(hashToBlob(*afterHash));
    else
        use.bind(); // NULL
    if (structuralParent)
        use(hashToBlob(*structuralParent));
    else
        use.bind(); // NULL
    use.exec();

    // Insert payload (cold storage)
    state->insertQueryPayload.use()(hashToBlob(queryHash))(payload).exec();

    // Insert shortcut for fast lookup
    state->insertShortcut.use()(hashToBlob(queryHash))(hashToBlob(nodeHash)).exec();

    return nodeHash;
}

NodeHash
TracingIndex::insertResponse(const NodeHash & afterHash, const std::string & request, const std::string & response)
{
    auto nodeHash = computeResponseNodeHash(afterHash, request, response);

    auto state(_state->lock());
    state->insertResponse.use()(hashToBlob(nodeHash))(hashToBlob(afterHash))(request) (response).exec();

    return nodeHash;
}

NodeHash TracingIndex::insertResult(const NodeHash & afterHash, const std::string & payload)
{
    auto nodeHash = computeResultNodeHash(afterHash, payload);

    auto state(_state->lock());
    state->insertResult.use()(hashToBlob(nodeHash))(hashToBlob(afterHash))(payload).exec();

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
    };
}

std::optional<ResponseNode> TracingIndex::getResponse(const NodeHash & nodeHash)
{
    auto state(_state->lock());
    auto query = state->getResponse.use()(hashToBlob(nodeHash));

    if (!query.next())
        return std::nullopt;

    return ResponseNode{
        .nodeHash = blobToHash(query.getStr(0)),
        .afterHash = blobToHash(query.getStr(1)),
        .request = query.getStr(2),
        .response = query.getStr(3),
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
        .payload = query.getStr(2),
    };
}

std::optional<std::string> TracingIndex::getQueryPayload(const QueryHash & queryHash)
{
    auto state(_state->lock());
    auto query = state->getQueryPayload.use()(hashToBlob(queryHash));

    if (!query.next())
        return std::nullopt;

    return query.getStr(0);
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
            });
    }

    return result;
}

std::vector<ResponseNode> TracingIndex::selectChildResponses(const NodeHash & afterHash)
{
    std::vector<ResponseNode> result;

    auto state(_state->lock());
    auto query = state->selectChildResponses.use()(hashToBlob(afterHash));

    while (query.next()) {
        result.push_back(
            ResponseNode{
                .nodeHash = blobToHash(query.getStr(0)),
                .afterHash = blobToHash(query.getStr(1)),
                .request = query.getStr(2),
                .response = query.getStr(3),
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
                .payload = query.getStr(2),
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
        .payload = query.getStr(2),
    };
}

std::vector<std::string> TracingIndex::getChildRequests(const NodeHash & afterHash)
{
    std::vector<std::string> result;

    auto state(_state->lock());
    auto query = state->getChildRequests.use()(hashToBlob(afterHash));

    while (query.next())
        result.push_back(query.getStr(0));

    return result;
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
            });
    }

    return result;
}

std::vector<ResponseNode> TracingIndex::selectDependencies(const NodeHash & queryNodeHash)
{
    std::vector<ResponseNode> result;

    // Start from the query node and trace back to root
    auto queryNode = getQuery(queryNodeHash);
    if (!queryNode || !queryNode->afterHash)
        return result;

    // Traverse back through the trie, collecting responses
    std::optional<NodeHash> current = queryNode->afterHash;

    while (current) {
        // Try Response first (most common in the middle of a trace)
        if (auto resp = getResponse(*current)) {
            result.push_back(*resp);
            current = resp->afterHash;
            continue;
        }

        // Try Result (predecessor of a query that follows another query's result)
        if (auto res = getResult(*current)) {
            current = res->afterHash;
            continue;
        }

        // Try Query (for root queries or unusual patterns)
        if (auto q = getQuery(*current)) {
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

std::vector<ResponseNode> TracingIndex::selectDependenciesUntilValidated(
    const NodeHash & queryNodeHash, const std::set<NodeHash> & validatedNodes, bool & reachedValidated)
{
    std::vector<ResponseNode> result;
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

        if (auto resp = getResponse(*current)) {
            result.push_back(*resp);
            current = resp->afterHash;
            continue;
        }

        if (auto res = getResult(*current)) {
            if (validatedNodes.count(res->nodeHash)) {
                reachedValidated = true;
                break;
            }
            current = res->afterHash;
            continue;
        }

        if (auto q = getQuery(*current)) {
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
