#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"  // for jsonToCborString

#include <nlohmann/json.hpp>

namespace nix::cidasks {

static std::string hashHex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

/** Initial id of a subject — what its content id is at the empty
    factset. Purely structural; no observations involved. */
static Hash initialId(const Subject & subject);

Hash extractFrom(const trace::QueryVariant & query)
{
    return std::visit(
        [](const auto & q) -> Hash {
            using Q = std::decay_t<decltype(q)>;
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

Hash hElement(const Fact & fact)
{
    nlohmann::json qj;
    std::visit([&](const auto & q) { qj = q; }, fact.query);
    auto reqHash = hashString(HashAlgorithm::SHA256, qj.dump());

    nlohmann::json rj;
    std::visit([&](const auto & r) { rj = r; }, fact.result);
    auto respPayload = jsonToCborString(rj);
    auto respHash = TracingDecisionGraph::computeResponseHash(respPayload);

    return TracingDecisionGraph::xorFactIntoHash(Hash(HashAlgorithm::SHA256), reqHash, respHash);
}

static Hash initialId(const Subject & subject)
{
    return std::visit(
        [](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, PositionalSeed>) {
                return hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentId = initialId(*alt.parent);
                nlohmann::json qj;
                if (alt.kind == DerivedSubject::Kind::GetAttr) {
                    qj = trace::QueryGetAttr{alt.name, hashHex(parentId)};
                } else {
                    qj = trace::QueryGetListElem{hashHex(parentId), alt.index};
                }
                return hashString(HashAlgorithm::SHA256, qj.dump());
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                auto fnId = initialId(*alt.fn);
                auto argId = initialId(*alt.arg);
                nlohmann::json qj = trace::QueryApply{hashHex(fnId), hashHex(argId)};
                return hashString(HashAlgorithm::SHA256, qj.dump());
            } else {
                throw Error("cidasks::initialId: unknown subject variant");
            }
        },
        subject.data);
}

Hash contentIdAt(const Subject & subject, const std::vector<Edge> & walk, size_t edgeIndex)
{
    /* Compute subject's content id at the precondition of the
       `edgeIndex`-th edge by replaying the first `edgeIndex` edges'
       effects on the subject's running content id.

       For positional seeds, only direct observations matter:
       facts in earlier edges whose `from` equals the seed's
       precondition-content-id at that edge contribute to its
       evolution.

       For derived and apply-result subjects, the id is recomputed
       at each step from constituent subjects' content ids at the
       current step's precondition — AND own-direct observations
       contribute too (facts whose `from` matched the derived's
       content id at their edge's precondition). */
    return std::visit(
        [&](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;

            // Run the walk and accumulate this subject's own-direct contributions.
            Hash own = Hash(HashAlgorithm::SHA256);
            for (size_t k = 0; k < edgeIndex && k < walk.size(); ++k) {
                // Subject's content id at edge K's precondition
                // = structural-id-at-K XOR (accumulated own contributions through K-1).
                // We recompute structural separately and combine.
                Hash structuralAtK(HashAlgorithm::SHA256);
                if constexpr (std::is_same_v<T, PositionalSeed>) {
                    structuralAtK = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
                } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                    auto parentAtK = contentIdAt(*alt.parent, walk, k);
                    nlohmann::json qj;
                    if (alt.kind == DerivedSubject::Kind::GetAttr) {
                        qj = trace::QueryGetAttr{alt.name, hashHex(parentAtK)};
                    } else {
                        qj = trace::QueryGetListElem{hashHex(parentAtK), alt.index};
                    }
                    structuralAtK = hashString(HashAlgorithm::SHA256, qj.dump());
                } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                    auto fnAtK = contentIdAt(*alt.fn, walk, k);
                    auto argAtK = contentIdAt(*alt.arg, walk, k);
                    nlohmann::json qj = trace::QueryApply{hashHex(fnAtK), hashHex(argAtK)};
                    structuralAtK = hashString(HashAlgorithm::SHA256, qj.dump());
                }

                Hash myCidAtK = TracingDecisionGraph::xorHashes(structuralAtK, own);

                for (auto & fact : walk[k].facts) {
                    Hash from = extractFrom(fact.query);
                    if (from == myCidAtK)
                        own = TracingDecisionGraph::xorHashes(own, hElement(fact));
                }
            }

            // Final structural at edgeIndex.
            Hash structuralAtEdgeIndex(HashAlgorithm::SHA256);
            if constexpr (std::is_same_v<T, PositionalSeed>) {
                structuralAtEdgeIndex = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentAtK = contentIdAt(*alt.parent, walk, edgeIndex);
                nlohmann::json qj;
                if (alt.kind == DerivedSubject::Kind::GetAttr) {
                    qj = trace::QueryGetAttr{alt.name, hashHex(parentAtK)};
                } else {
                    qj = trace::QueryGetListElem{hashHex(parentAtK), alt.index};
                }
                structuralAtEdgeIndex = hashString(HashAlgorithm::SHA256, qj.dump());
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                auto fnAtK = contentIdAt(*alt.fn, walk, edgeIndex);
                auto argAtK = contentIdAt(*alt.arg, walk, edgeIndex);
                nlohmann::json qj = trace::QueryApply{hashHex(fnAtK), hashHex(argAtK)};
                structuralAtEdgeIndex = hashString(HashAlgorithm::SHA256, qj.dump());
            }

            return TracingDecisionGraph::xorHashes(structuralAtEdgeIndex, own);
        },
        subject.data);
}

Hash contentIdAfter(const Subject & subject, const std::vector<Edge> & walk)
{
    return contentIdAt(subject, walk, walk.size());
}

} // namespace nix::cidasks
