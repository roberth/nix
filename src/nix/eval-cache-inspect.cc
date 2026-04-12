#include "nix/cmd/command.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>

using namespace nix;

// ---- Helpers ----

static std::string hashArg(const std::string & s)
{
    // Accept full hex or prefix (for convenience)
    return s;
}

static std::string describePayload(const std::string & cbor)
{
    try {
        auto j = cborStringToJson(cbor);
        return j.dump();
    } catch (...) {
        return "(undecoded " + std::to_string(cbor.size()) + " bytes)";
    }
}

static std::string indent(int depth)
{
    return std::string(depth * 2, ' ');
}

// ---- nix eval-cache inspect <hash> ----

struct CmdEvalCacheInspect : Command
{
    std::string nodeHashHex;

    CmdEvalCacheInspect()
    {
        expectArg("hash", &nodeHashHex);
    }

    std::string description() override
    {
        return "Inspect a node in the tracing cache trie.";
    }

    Category category() override
    {
        return catUtility;
    }

    void run() override
    {
        TracingIndex index;
        auto nodeHash = Hash::parseAny(nodeHashHex, HashAlgorithm::SHA256);
        std::cerr << "Parsed hash: " << nodeHash.to_string(HashFormat::Base16, false) << " (" << nodeHash.hashSize << " bytes)\n";

        if (auto q = index.getQuery(nodeHash)) {
            std::cout << "Type: Query\n";
            std::cout << "NodeHash: " << q->nodeHash.to_string(HashFormat::Base16, false) << "\n";
            std::cout << "QueryHash: " << q->queryHash.to_string(HashFormat::Base16, false) << "\n";
            std::cout << "AfterHash: " << (q->afterHash ? q->afterHash->to_string(HashFormat::Base16, false) : "NULL (root)") << "\n";
            std::cout << "StructuralParent: " << (q->structuralParent ? q->structuralParent->to_string(HashFormat::Base16, false) : "NULL") << "\n";
            std::cout << "Depth: " << q->depth << "\n";
            if (auto payload = index.getQueryPayload(q->queryHash)) {
                std::cout << "Payload: " << describePayload(*payload) << "\n";
            }

            // Show children
            auto childQueries = index.selectChildQueries(nodeHash);
            auto childResults = index.selectChildResults(nodeHash);
            if (!childQueries.empty() || !childResults.empty()) {
                std::cout << "Children:\n";
                for (const auto & cq : childQueries) {
                    auto pl = index.getQueryPayload(cq.queryHash);
                    std::cout << "  Query " << cq.nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) << "..."
                              << " depth=" << cq.depth;
                    if (pl)
                        std::cout << " " << describePayload(*pl);
                    std::cout << "\n";
                }
                for (const auto & cr : childResults) {
                    std::cout << "  Result " << cr.nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) << "..."
                              << " " << describePayload(cr.payload);
                    if (cr.queryNodeHash)
                        std::cout << " qnHash=" << cr.queryNodeHash->to_string(HashFormat::Base16, false).substr(0, 16) << "...";
                    std::cout << "\n";
                }
            }
        } else if (auto r = index.getResult(nodeHash)) {
            std::cout << "Type: Result\n";
            std::cout << "NodeHash: " << r->nodeHash.to_string(HashFormat::Base16, false) << "\n";
            std::cout << "AfterHash: " << r->afterHash.to_string(HashFormat::Base16, false) << "\n";
            std::cout << "QueryNodeHash: " << (r->queryNodeHash ? r->queryNodeHash->to_string(HashFormat::Base16, false) : "NULL") << "\n";
            std::cout << "Payload: " << describePayload(r->payload) << "\n";

            auto childQueries = index.selectChildQueries(nodeHash);
            auto childResults = index.selectChildResults(nodeHash);
            if (!childQueries.empty() || !childResults.empty()) {
                std::cout << "Children:\n";
                for (const auto & cq : childQueries) {
                    auto pl = index.getQueryPayload(cq.queryHash);
                    std::cout << "  Query " << cq.nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) << "..."
                              << " depth=" << cq.depth;
                    if (pl)
                        std::cout << " " << describePayload(*pl);
                    std::cout << "\n";
                }
                for (const auto & cr : childResults) {
                    std::cout << "  Result " << cr.nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) << "..."
                              << " " << describePayload(cr.payload);
                    if (cr.queryNodeHash)
                        std::cout << " qnHash=" << cr.queryNodeHash->to_string(HashFormat::Base16, false).substr(0, 16) << "...";
                    std::cout << "\n";
                }
            }
        } else {
            throw Error("node %s not found in the trie", nodeHashHex);
        }
    }
};

// ---- nix eval-cache trace-back <hash> ----

struct CmdEvalCacheTraceBack : Command
{
    std::string nodeHashHex;

    CmdEvalCacheTraceBack()
    {
        expectArg("hash", &nodeHashHex);
    }

    std::string description() override
    {
        return "Trace backward from a node to the root of its chain.";
    }

    Category category() override
    {
        return catUtility;
    }

    void run() override
    {
        TracingIndex index;
        auto current = Hash::parseAny(nodeHashHex, HashAlgorithm::SHA256);

        std::vector<std::string> chain;
        for (int i = 0; i < 1000; i++) {
            if (auto q = index.getQuery(current)) {
                auto pl = index.getQueryPayload(q->queryHash);
                std::string desc = "Query depth=" + std::to_string(q->depth);
                if (pl)
                    desc += " " + describePayload(*pl);
                chain.push_back(q->nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) + " " + desc);
                if (!q->afterHash)
                    break;
                current = *q->afterHash;
            } else if (auto r = index.getResult(current)) {
                std::string desc = "Result " + describePayload(r->payload);
                if (r->queryNodeHash)
                    desc += " qnHash=" + r->queryNodeHash->to_string(HashFormat::Base16, false).substr(0, 16);
                chain.push_back(r->nodeHash.to_string(HashFormat::Base16, false).substr(0, 16) + " " + desc);
                current = r->afterHash;
            } else {
                chain.push_back(current.to_string(HashFormat::Base16, false).substr(0, 16) + " (unknown node)");
                break;
            }
        }

        // Print in reverse (root first)
        for (int i = chain.size() - 1; i >= 0; i--) {
            std::cout << indent(chain.size() - 1 - i) << chain[i] << "\n";
        }
    }
};

// ---- nix eval-cache tree <hash> [--depth N] ----

struct CmdEvalCacheTree : Command
{
    std::string nodeHashHex;
    int maxDepth = 5;

    CmdEvalCacheTree()
    {
        expectArg("hash", &nodeHashHex);
        addFlag({
            .longName = "depth",
            .description = "Maximum tree depth to display.",
            .labels = {"n"},
            .handler = {&maxDepth},
        });
    }

    std::string description() override
    {
        return "Show the forward tree from a node, collapsing linear chains.";
    }

    Category category() override
    {
        return catUtility;
    }

    void run() override
    {
        TracingIndex index;
        auto root = Hash::parseAny(nodeHashHex, HashAlgorithm::SHA256);
        printTree(index, root, 0);
    }

private:
    void printTree(TracingIndex & index, const NodeHash & nodeHash, int depth)
    {
        if (depth > maxDepth) {
            std::cout << indent(depth) << "... (max depth)\n";
            return;
        }

        // Describe this node
        if (auto q = index.getQuery(nodeHash)) {
            auto pl = index.getQueryPayload(q->queryHash);
            std::cout << indent(depth) << "Q d=" << q->depth << " "
                      << nodeHash.to_string(HashFormat::Base16, false).substr(0, 12);
            if (pl)
                std::cout << " " << describePayload(*pl);
            std::cout << "\n";
        } else if (auto r = index.getResult(nodeHash)) {
            std::cout << indent(depth) << "R "
                      << nodeHash.to_string(HashFormat::Base16, false).substr(0, 12)
                      << " " << describePayload(r->payload);
            if (r->queryNodeHash)
                std::cout << " →" << r->queryNodeHash->to_string(HashFormat::Base16, false).substr(0, 12);
            std::cout << "\n";
        } else {
            std::cout << indent(depth) << "? " << nodeHash.to_string(HashFormat::Base16, false).substr(0, 12) << "\n";
            return;
        }

        // Collect children
        auto childQueries = index.selectChildQueries(nodeHash);
        auto childResults = index.selectChildResults(nodeHash);
        auto totalChildren = childQueries.size() + childResults.size();

        if (totalChildren == 0)
            return;

        if (totalChildren == 1) {
            // Linear chain — collapse
            if (!childResults.empty())
                printTree(index, childResults[0].nodeHash, depth);
            else
                printTree(index, childQueries[0].nodeHash, depth);
            return;
        }

        // Branch point
        std::cout << indent(depth) << "⊕ " << totalChildren << " children:\n";
        for (const auto & cr : childResults) {
            printTree(index, cr.nodeHash, depth + 1);
        }
        for (const auto & cq : childQueries) {
            printTree(index, cq.nodeHash, depth + 1);
        }
    }
};

// ---- nix eval-cache shortcuts <queryHash> ----

struct CmdEvalCacheShortcuts : Command
{
    std::string queryHashHex;

    CmdEvalCacheShortcuts()
    {
        expectArg("queryhash", &queryHashHex);
    }

    std::string description() override
    {
        return "List shortcut candidates for a query hash.";
    }

    Category category() override
    {
        return catUtility;
    }

    void run() override
    {
        TracingIndex index;
        auto queryHash = Hash::parseAny(queryHashHex, HashAlgorithm::SHA256);

        if (auto payload = index.getQueryPayload(queryHash)) {
            std::cout << "Query: " << describePayload(*payload) << "\n\n";
        }

        auto shortcuts = index.selectShortcuts(queryHash);
        std::cout << shortcuts.size() << " shortcut(s):\n";
        for (const auto & s : shortcuts) {
            std::cout << "  " << s.nodeHash.to_string(HashFormat::Base16, false).substr(0, 16)
                      << " created=" << s.createdAt << "\n";
        }
    }
};

// ---- nix eval-cache stats ----

struct CmdEvalCacheStats : Command
{
    std::string description() override
    {
        return "Show statistics about the tracing cache.";
    }

    Category category() override
    {
        return catUtility;
    }

    void run() override
    {
        TracingIndex index;

        // Find all root queries (afterHash = NULL)
        // and count shortcuts, queries, results
        auto shortcuts = index.selectShortcuts(Hash(HashAlgorithm::SHA256)); // dummy, we need a stats method
        // For now, just show what we can access
        std::cout << "Trie location: ~/.cache/nix/eval-tracing-index-v1/index.sqlite\n";
        std::cout << "(Use 'nix eval-cache inspect <hash>' to explore nodes)\n";
    }
};

static auto rInspect = registerCommand2<CmdEvalCacheInspect>({"eval-cache", "inspect"});
static auto rTraceBack = registerCommand2<CmdEvalCacheTraceBack>({"eval-cache", "trace-back"});
static auto rTree = registerCommand2<CmdEvalCacheTree>({"eval-cache", "tree"});
static auto rShortcuts = registerCommand2<CmdEvalCacheShortcuts>({"eval-cache", "shortcuts"});
static auto rStats = registerCommand2<CmdEvalCacheStats>({"eval-cache", "stats"});
