#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"  // for jsonToCborString

#include <nlohmann/json.hpp>

namespace nix::cidasks {

static std::string hashHex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

Hash extractFrom(const trace::QueryVariant & query)
{
    return std::visit(
        [](const auto & q) -> Hash {
            if constexpr (requires { q.from; }) {
                if (!q.from.isContent())
                    throw Error("cidasks::extractFrom: query.from is not a ContentLeaf");
                return Hash::parseNonSRIUnprefixed(q.from.contentHash(), HashAlgorithm::SHA256);
            } else {
                throw Error("cidasks::extractFrom: query type has no `from` field");
            }
        },
        query);
}

Fact factFromQR(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    nlohmann::json qj;
    std::visit([&](const auto & q) { qj = q; }, query);
    auto reqHash = hashString(HashAlgorithm::SHA256, qj.dump());

    nlohmann::json rj;
    std::visit([&](const auto & r) { rj = r; }, result);
    auto respPayload = jsonToCborString(rj);
    auto respHash = TracingDecisionGraph::computeResponseHash(respPayload);

    return Fact{
        .fromHash = extractFrom(query),
        .elementHash = TracingDecisionGraph::xorFactIntoHash(Hash(HashAlgorithm::SHA256), reqHash, respHash),
    };
}

Hash contentIdAt(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex)
{
    /* Compute subject's content id at the precondition of the
       `edgeIndex`-th edge by replaying the first `edgeIndex` edges'
       effects on the subject's running content id.

       Inheritance: `scope` is the XOR of outer-scope CDIs (chiefly
       the cached call's CDI(Q) at the cb-apply boundary). Passing
       zero gives the pure structural id. Leaf subjects
       (PositionalSeed, OpaqueContentSubject) XOR `scope` into their
       base hash. Composite subjects (DerivedSubject,
       ApplyResultSubject) propagate `scope` recursively through
       their constituents' content ids; the structural derivation
       at this level uses those scoped constituents' values in its
       query payload, so inheritance ripples through naturally
       without a second XOR at this level.

       For positional seeds, only direct observations matter:
       facts in earlier edges whose `from` equals the seed's
       precondition-content-id at that edge contribute to its
       evolution. */
    return std::visit(
        [&](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;

            auto structuralAt = [&](size_t k) -> Hash {
                if constexpr (std::is_same_v<T, PositionalSeed>) {
                    auto base = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
                    return TracingDecisionGraph::xorHashes(base, scope);
                } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                    auto parentAtK = contentIdAt(*alt.parent, scope, walk, k);
                    nlohmann::json qj;
                    if (alt.kind == DerivedSubject::Kind::GetAttr) {
                        qj = trace::QueryGetAttr{alt.name, hashHex(parentAtK)};
                    } else {
                        qj = trace::QueryGetListElem{hashHex(parentAtK), alt.index};
                    }
                    return hashString(HashAlgorithm::SHA256, qj.dump());
                } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                    auto fnAtK = contentIdAt(*alt.fn, scope, walk, k);
                    auto argAtK = contentIdAt(*alt.arg, scope, walk, k);
                    nlohmann::json qj = trace::QueryApply{hashHex(fnAtK), hashHex(argAtK)};
                    return hashString(HashAlgorithm::SHA256, qj.dump());
                } else if constexpr (std::is_same_v<T, OpaqueContentSubject>) {
                    return TracingDecisionGraph::xorHashes(alt.hash, scope);
                } else {
                    throw Error("cidasks::contentIdAt: unknown subject variant");
                }
            };

            // Run the walk and accumulate this subject's own-direct contributions.
            Hash own = Hash(HashAlgorithm::SHA256);
            for (size_t k = 0; k < edgeIndex && k < walk.size(); ++k) {
                Hash myCidAtK = TracingDecisionGraph::xorHashes(structuralAt(k), own);
                for (auto & fact : walk[k].facts) {
                    if (fact.fromHash == myCidAtK)
                        own = TracingDecisionGraph::xorHashes(own, fact.elementHash);
                }
            }

            return TracingDecisionGraph::xorHashes(structuralAt(edgeIndex), own);
        },
        subject.data);
}

Hash contentIdAfter(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk)
{
    return contentIdAt(subject, scope, walk, walk.size());
}

std::string describe(const Subject & subject)
{
    return std::visit(
        [](const auto & alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, PositionalSeed>) {
                return "seed(" + std::to_string(alt.depth) + ")";
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                std::string kind =
                    alt.kind == DerivedSubject::Kind::GetAttr ? "getAttr" : "getListElem";
                std::string sel = alt.kind == DerivedSubject::Kind::GetAttr
                    ? "\"" + alt.name + "\""
                    : std::to_string(alt.index);
                return kind + "(" + describe(*alt.parent) + ", " + sel + ")";
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                return "applyResult(" + describe(*alt.fn) + ", " + describe(*alt.arg) + ")";
            } else if constexpr (std::is_same_v<T, OpaqueContentSubject>) {
                return "opaque(" + alt.hash.to_string(HashFormat::Base16, false).substr(0, 12) + "...)";
            } else {
                return "?";
            }
        },
        subject.data);
}

} // namespace nix::cidasks
