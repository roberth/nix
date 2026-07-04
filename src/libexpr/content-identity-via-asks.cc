#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"  // for jsonToCborString
#include "nix/util/error.hh"

#include <cstring>
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

const Subject & rootSubjectOf(const Subject & subject)
{
    if (auto * d = std::get_if<DerivedSubject>(&subject.data))
        return rootSubjectOf(*d->parent);
    if (auto * a = std::get_if<ApplyResultSubject>(&subject.data))
        /* Single-root assumption (= same cb_arg supplies fn and arg)
           lets us pick either side. Multi-root applies would need
           fromCIDs[] entries for both fn-root and arg-root; follow-up
           work once the simple case lands. */
        return rootSubjectOf(*a->fn);
    return subject;
}

/* Subject leaf equality — used by the path builder to dedupe roots
   (= two derivation chains rooted at the same PositionalSeed or
   PostulatedIdempotentRead share one entry in fromCIDs). Only meaningful
   for leaf forms; the builder only compares leaves. */
static bool sameLeaf(const Subject & a, const Subject & b)
{
    if (auto * ap = std::get_if<PositionalSeed>(&a.data)) {
        if (auto * bp = std::get_if<PositionalSeed>(&b.data))
            return ap->depth == bp->depth;
        return false;
    }
    if (auto * ap = std::get_if<PostulatedIdempotentRead>(&a.data)) {
        if (auto * bp = std::get_if<PostulatedIdempotentRead>(&b.data))
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
       subjects (PositionalSeed, PostulatedIdempotentRead) deduplicate
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
                                  || std::is_same_v<T, PostulatedIdempotentRead>) {
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

Observation observationFromQR(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    nlohmann::json qj;
    std::visit([&](const auto & q) { qj = q; }, query);
    auto reqHash = hashString(HashAlgorithm::SHA256, qj.dump());

    nlohmann::json rj;
    std::visit([&](const auto & r) { rj = r; }, result);
    auto respPayload = jsonToCborString(rj);
    auto respHash = TracingDecisionGraph::computeResponseHash(respPayload);

    return Observation{
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
        auto cid = scopeStateIdAt(root, scope, walk, edgeIndex);
        q.fromCIDs.emplace_back(hashHex(cid));
    }
    q.fnPath = *applyStep.fnPath;
    q.argPath = *applyStep.argPath;
    q.fnRootIndex = applyStep.fnRootIndex;
    q.argRootIndex = applyStep.argRootIndex;
    return q;
}

Hash scopeStateIdAtWithHook(
    const Subject & subject,
    const Hash & scope,
    const std::vector<Edge> & walk,
    size_t edgeIndex,
    const std::function<void(const EvolutionStep &)> & hook)
{
    /* Mirrors scopeStateIdAt's fold logic, emitting one
       EvolutionStep per matched observation. Path 3 walker will
       navigate via a table stamped from these emissions.

       Within a single walk edge, all observations are matched
       against the edge-entry scopeStateId (not the accumulated
       one). This means multiple observations in the same edge
       fold into the SAME curBefore; the emitted `curAfter` is
       curBefore XOR obs.elem (per-observation), not the
       edge-cumulative fold. Path 3 walker will replicate this
       edge-scoped semantics — cur updates at edge boundaries,
       not per-observation. */
    Hash result = scopeStateIdAt(subject, scope, walk, edgeIndex);
    Hash subjectSelfHash = scopeStateIdAt(subject, Hash(HashAlgorithm::SHA256), {}, 0);
    Hash selfFactFold = Hash(HashAlgorithm::SHA256);
    for (size_t k = 0; k < edgeIndex && k < walk.size(); ++k) {
        Hash myScopeStateIdAtK = TracingDecisionGraph::xorHashes(
            scopeStateIdAt(subject, scope, walk, k), Hash(HashAlgorithm::SHA256));
        /* Above is `scopeStateIdAt(subject, scope, walk, k)` —
           subject's state at edge k's precondition. */
        for (auto & obs : walk[k].observations) {
            if (obs.fromHash == myScopeStateIdAtK) {
                Hash curAfter = TracingDecisionGraph::xorHashes(myScopeStateIdAtK, obs.elementHash);
                if (hook)
                    hook(EvolutionStep{myScopeStateIdAtK, obs.fromHash, obs.elementHash, curAfter});
                selfFactFold = TracingDecisionGraph::xorHashes(selfFactFold, obs.elementHash);
            }
        }
    }
    (void) subjectSelfHash;  /* Reserved for stamping the subject's Merkle key. */
    return result;
}

Hash scopeStateIdAt(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex)
{
    /* Compute subject's scope state id at the precondition of the
       `edgeIndex`-th edge by replaying the first `edgeIndex` edges'
       effects on the subject's running scope state id.

       Inheritance: `scope` is the XOR of outer-scope argStateIds (chiefly
       the cached call's argStateId(Q) at the cb-apply boundary). Passing
       zero gives the pure structural id. Leaf subjects
       (PositionalSeed, PostulatedIdempotentRead) XOR `scope` into their
       base hash. Composite subjects (DerivedSubject,
       ApplyResultSubject) propagate `scope` recursively through
       their constituents' scope state ids; the structural derivation
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

            /* The subject's id at edge step `k`, BEFORE this subject's
               selfFactFold gets XOR'd in. Naming note: not pure
               De-Bruijn-style "structural" for all variants. For
               PositionalSeed / PostulatedIdempotentRead it IS k-invariant
               pure position. For ApplyResultSubject it depends on `k`
               because it composes the constituents' *fully evolved*
               scopeStateIds (= constituents' scopeStateIdAt at the
               same k) into a SHA-sealed shape — so the apply's id
               varies with k via constituent evolution even before
               this subject's selfFactFold contributes. */
            auto subjectIdAt = [&](size_t k) -> Hash {
                if constexpr (std::is_same_v<T, PositionalSeed>) {
                    auto base = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
                    return TracingDecisionGraph::xorHashes(base, scope);
                } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                    /* Derived subjects have no argStateId — only an address
                       (= producer query hash). Callers that need an
                       address for any subject use `structuralAddress`;
                       reaching this branch via `scopeStateIdAt` means a
                       caller passed a derived subject where the design
                       requires an argument-level subject. */
                    nix::unreachable();
                } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                    /* Apply-result composes its constituents' argStateIds.
                       Constituents may be Derived → route through
                       structuralAddress (which dispatches Derived to
                       the producer-query-hash path). */
                    auto fnAtK = structuralAddress(*alt.fn, scope, walk, k);
                    auto argAtK = structuralAddress(*alt.arg, scope, walk, k);
                    nlohmann::json qj = trace::QueryApply{hashHex(fnAtK), hashHex(argAtK)};
                    return hashString(HashAlgorithm::SHA256, qj.dump());
                } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                    /* X is treated as scope-saturated. Callers pass
                       hashes that already encode the relevant scope
                       — `AmbientObject::getCdi()` returns
                       structuralAddressAfter with inheritedScope
                       baked in; ReplayLocalObject's localId is
                       scopeStateIdAfter(PositionalSeed{D}, callScope, {})
                       which is also scope-saturated. Re-XORing scope
                       here would either double-XOR (= scope-saturated
                       inputs) or under-XOR (= un-scoped inputs) — the
                       per-arg-completion doc (= option 1) avoids
                       both by treating PostulatedIdempotentRead as a
                       pre-computed-argStateId atom. */
                    return alt.hash;
                } else {
                    throw Error("cidasks::scopeStateIdAt: unknown subject variant");
                }
            };

            /* Walk the chain and accumulate the XOR-fold of v13-fact
               element hashes from observations that point at THIS
               subject. An observation in `walk[k]` points at this
               subject iff `obs.fromHash` (= the recorder-stamped
               subject pointer carried in the `from` field of the
               query that produced the fact) equals this subject's
               running scopeStateId at step k. The result is the
               contribution to scopeStateId that comes from v13 facts
               about self. */
            Hash selfFactFold = Hash(HashAlgorithm::SHA256);
            std::string foldTrace;
            for (size_t k = 0; k < edgeIndex && k < walk.size(); ++k) {
                Hash myScopeStateIdAtK = TracingDecisionGraph::xorHashes(subjectIdAt(k), selfFactFold);
                for (auto & obs : walk[k].observations) {
                    bool matches = (obs.fromHash == myScopeStateIdAtK);
                    foldTrace += "\n    k=" + std::to_string(k)
                        + " myId=" + hashHex(myScopeStateIdAtK).substr(0, 12)
                        + " obs.from=" + hashHex(obs.fromHash).substr(0, 12)
                        + " obs.elem=" + hashHex(obs.elementHash).substr(0, 12)
                        + (matches ? " MATCH→fold" : " skip");
                    if (matches)
                        selfFactFold = TracingDecisionGraph::xorHashes(selfFactFold, obs.elementHash);
                }
            }

            auto result = TracingDecisionGraph::xorHashes(subjectIdAt(edgeIndex), selfFactFold);
            tracingCacheLog(
                "scopeStateIdAt: subject=%s scope=%s walk.size=%zu edgeIndex=%zu\n"
                "  subjectIdAt(edgeIndex)=%s selfFactFold=%s result=%s%s",
                describe(subject),
                hashHex(scope).substr(0, 12),
                walk.size(), edgeIndex,
                hashHex(subjectIdAt(edgeIndex)).substr(0, 12),
                hashHex(selfFactFold).substr(0, 12),
                hashHex(result).substr(0, 12),
                foldTrace);
            return result;
        },
        subject.data);
}

Hash scopeStateIdAfter(const Subject & subject, const Hash & scope, const std::vector<Edge> & walk)
{
    return scopeStateIdAt(subject, scope, walk, walk.size());
}

Hash structuralAddress(
    const Subject & subject, const Hash & scope, const std::vector<Edge> & walk, size_t edgeIndex)
{
    /* For non-derived subjects, the structural address IS the argStateId.
       For DerivedSubject, scopeStateIdAt traps; we compute the
       producer query hash (= what a `from = root_cdi` flush would
       hash for a query naming this derived value) directly. */
    if (auto * d = std::get_if<DerivedSubject>(&subject.data)) {
        auto [pathToParent, parentRoots] = pathAndRootsFromSubject(*d->parent);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(parentRoots.size());
        for (auto & root : parentRoots) {
            auto cid = scopeStateIdAt(root, scope, walk, edgeIndex);
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
    return scopeStateIdAt(subject, scope, walk, edgeIndex);
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
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                return "opaque(" + alt.hash.to_string(HashFormat::Base16, false).substr(0, 12) + "...)";
            } else {
                return "?";
            }
        },
        subject.data);
}

} // namespace nix::cidasks
