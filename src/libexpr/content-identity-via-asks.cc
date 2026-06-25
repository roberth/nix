#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"  // for jsonToCborString
#include "nix/util/error.hh"

#include <nlohmann/json.hpp>

namespace nix::cidasks {

static std::string hashHex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

std::optional<Subject> subjectFromObjectIdentity(const ObjectIdentityLike & id)
{
    if (id.subject)
        return *id.subject;
    if (id.cdiHex) {
        try {
            auto h = Hash::parseNonSRIUnprefixed(*id.cdiHex, HashAlgorithm::SHA256);
            return Subject{OpaqueContentSubject{h}};
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }
    return std::nullopt;
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

const Subject & rootSubjectOf(const Subject & subject)
{
    if (auto * d = std::get_if<DerivedSubject>(&subject.data))
        return rootSubjectOf(*d->parent);
    if (auto * a = std::get_if<ApplyResultSubject>(&subject.data))
        /* Single-root assumption (= same cb_arg supplies fn and arg)
           lets us pick either side. Multi-root applies would need
           fromCIDs[] entries for both fn-root and arg-root; deferred
           until the simple case lands. */
        return rootSubjectOf(*a->fn);
    return subject;
}

/* Subject leaf equality — used by the path builder to dedupe roots
   (= two derivation chains rooted at the same PositionalSeed or
   OpaqueContentSubject share one entry in fromCIDs). Only meaningful
   for leaf forms; the builder only compares leaves. */
static bool sameLeaf(const Subject & a, const Subject & b)
{
    if (auto * ap = std::get_if<PositionalSeed>(&a.data)) {
        if (auto * bp = std::get_if<PositionalSeed>(&b.data))
            return ap->depth == bp->depth;
        return false;
    }
    if (auto * ap = std::get_if<OpaqueContentSubject>(&a.data)) {
        if (auto * bp = std::get_if<OpaqueContentSubject>(&b.data))
            return ap->hash == bp->hash;
        return false;
    }
    return false;
}

PathAndRoots pathAndRootsFromSubject(const Subject & subject)
{
    /* Builder collects roots depth-first into a single vector.
       DerivedSubject pushes a step onto its parent's path.
       ApplyResultSubject emits a single Apply step whose sub-paths
       carry their own (absolute-index) root references. Leaf
       subjects (PositionalSeed, OpaqueContentSubject) deduplicate
       against previously-collected roots so shared cb_args (= fn
       and arg derived from the same outer arg) collapse to one
       fromCIDs entry. */
    struct Builder
    {
        std::vector<Subject> roots;

        size_t findOrInsert(const Subject & leaf)
        {
            for (size_t i = 0; i < roots.size(); ++i)
                if (sameLeaf(roots[i], leaf))
                    return i;
            roots.push_back(leaf);
            return roots.size() - 1;
        }

        /* Returns (path, rootIndex). rootIndex is the index of the
           subject's *natural* root in `roots`: the leaf for non-apply
           subjects, or the fn's root for apply subjects (by
           convention — chosen so DerivedSubject{Apply, ...} starts
           navigation from fn's root, which then immediately gets
           used by the Apply step's fnRootIndex). */
        std::pair<trace::PathExpr, size_t> build(const Subject & s)
        {
            return std::visit(
                [&](const auto & alt) -> std::pair<trace::PathExpr, size_t> {
                    using T = std::decay_t<decltype(alt)>;
                    if constexpr (std::is_same_v<T, PositionalSeed>
                                  || std::is_same_v<T, OpaqueContentSubject>) {
                        size_t idx = findOrInsert(s);
                        return {{}, idx};
                    } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                        auto [parentPath, parentIdx] = build(*alt.parent);
                        trace::PathStep step;
                        step.kind = alt.kind == DerivedSubject::Kind::GetAttr
                            ? trace::PathStep::Kind::GetAttr
                            : trace::PathStep::Kind::GetListElem;
                        step.name = alt.name;
                        step.index = alt.index;
                        parentPath.steps.push_back(std::move(step));
                        return {std::move(parentPath), parentIdx};
                    } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                        auto [fnPath, fnIdx] = build(*alt.fn);
                        auto [argPath, argIdx] = build(*alt.arg);
                        trace::PathStep step;
                        step.kind = trace::PathStep::Kind::Apply;
                        step.fnPath = std::make_shared<trace::PathExpr>(std::move(fnPath));
                        step.argPath = std::make_shared<trace::PathExpr>(std::move(argPath));
                        step.fnRootIndex = fnIdx;
                        step.argRootIndex = argIdx;
                        trace::PathExpr path;
                        path.steps.push_back(std::move(step));
                        return {std::move(path), fnIdx};
                    } else {
                        return {{}, 0};
                    }
                },
                s.data);
        }
    };

    Builder b;
    auto [path, _] = b.build(subject);
    return {std::move(path), std::move(b.roots)};
}

trace::PathExpr pathFromSubject(const Subject & subject)
{
    return pathAndRootsFromSubject(subject).path;
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

trace::QueryApply makeApplyResultQuery(
    const Subject & applyResultSubject, const Hash & scope,
    const std::vector<Edge> & walk, size_t edgeIndex)
{
    if (!std::holds_alternative<ApplyResultSubject>(applyResultSubject.data))
        throw Error("cidasks::makeApplyResultQuery: subject is not an ApplyResultSubject");

    auto par = pathAndRootsFromSubject(applyResultSubject);
    if (par.path.steps.size() != 1
        || par.path.steps[0].kind != trace::PathStep::Kind::Apply
        || !par.path.steps[0].fnPath
        || !par.path.steps[0].argPath)
        throw Error("cidasks::makeApplyResultQuery: unexpected path shape");
    const auto & applyStep = par.path.steps[0];

    trace::QueryApply q;
    q.fromCIDs.reserve(par.roots.size());
    for (auto & root : par.roots) {
        auto cid = contentIdAt(root, scope, walk, edgeIndex);
        q.fromCIDs.emplace_back(hashHex(cid));
    }
    q.fnPath = *applyStep.fnPath;
    q.argPath = *applyStep.argPath;
    q.fnRootIndex = applyStep.fnRootIndex;
    q.argRootIndex = applyStep.argRootIndex;
    return q;
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
                    /* Derived subjects have no CDI — only an address
                       (= producer query hash). Callers that need an
                       address for any subject use `structuralAddress`;
                       reaching this branch via `contentIdAt` means a
                       caller passed a derived subject where the design
                       requires an argument-level subject. */
                    nix::unreachable();
                } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                    /* Apply-result composes its constituents' CDIs.
                       Constituents may be Derived → route through
                       structuralAddress (which dispatches Derived to
                       the producer-query-hash path). */
                    auto fnAtK = structuralAddress(*alt.fn, scope, walk, k);
                    auto argAtK = structuralAddress(*alt.arg, scope, walk, k);
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

Hash structuralAddress(
    const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex)
{
    /* For non-derived subjects, the structural address IS the CDI.
       For DerivedSubject, contentIdAt traps; we compute the
       producer query hash (= what a `from = root_cdi` flush would
       hash for a query naming this derived value) directly. */
    if (auto * d = std::get_if<DerivedSubject>(&subject.data)) {
        auto [pathToParent, parentRoots] = pathAndRootsFromSubject(*d->parent);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(parentRoots.size());
        for (auto & root : parentRoots) {
            auto cid = contentIdAt(root, scope, walk, edgeIndex);
            fromCIDs.emplace_back(hashHex(cid));
        }
        auto fromLeaf = fromCIDs.empty() ? trace::QueryLeaf("") : fromCIDs[0];
        nlohmann::json qj;
        if (d->kind == DerivedSubject::Kind::GetAttr) {
            trace::QueryGetAttr q{d->name, fromLeaf};
            q.path = pathToParent;
            q.fromCIDs = fromCIDs;
            qj = q;
        } else {
            trace::QueryGetListElem q{fromLeaf, d->index};
            q.path = pathToParent;
            q.fromCIDs = fromCIDs;
            qj = q;
        }
        return hashString(HashAlgorithm::SHA256, qj.dump());
    }
    return contentIdAt(subject, scope, walk, edgeIndex);
}

Hash structuralAddressAfter(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk)
{
    return structuralAddress(subject, scope, walk, walk.size());
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
