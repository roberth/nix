#include "nix/expr/subject-id.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"  // for jsonToCborString
#include "nix/util/error.hh"

#include <cstring>
#include <nlohmann/json.hpp>

namespace nix {

static std::string hashHex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

Hash fromStateHashOf(const trace::QueryVariant & query)
{
    return std::visit(
        [](const auto & q) -> Hash {
            if constexpr (requires { q.from; }) {
                if (!q.from.isStateHash())
                    throw Error("fromStateHashOf: query.from is not a StateHashLeaf");
                return Hash::parseNonSRIUnprefixed(q.from.stateHash(), HashAlgorithm::SHA256);
            } else {
                throw Error("fromStateHashOf: query type has no `from` field");
            }
        },
        query);
}

/* Subject leaf equality — used by the path builder to dedupe roots
   (= two derivation chains rooted at the same Arg or
   PostulatedIdempotentRead share one entry in fromStateHashes). Only meaningful
   for leaf forms; the builder only compares leaves. */
static bool sameLeaf(const Subject & a, const Subject & b)
{
    if (auto * ap = std::get_if<Arg>(&a.data)) {
        if (auto * bp = std::get_if<Arg>(&b.data))
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
       subjects (Arg, PostulatedIdempotentRead) deduplicate
       against previously-collected roots so shared cb_args (= fn
       and arg derived from the same outer arg) collapse to one
       fromStateHashes entry. */
    struct Builder
    {
        std::vector<Subject> roots;

        size_t findOrInsert(const Subject & leaf)
        {
            /* Leaves-only invariant: pathAndRootsFromSubject's public
               contract guarantees `roots` contains only Arg or
               PostulatedIdempotentRead. Enforce here so a future
               builder change that accidentally adds a Derived or
               ApplyResult to roots fires clearly rather than
               trapping in a caller's strict stateHashAt. */
            assert((std::holds_alternative<Arg>(leaf.data)
                    || std::holds_alternative<PostulatedIdempotentRead>(leaf.data))
                   && "pathAndRootsFromSubject: root must be a leaf variant");
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
                    if constexpr (std::is_same_v<T, Arg>
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
        .fromHash = fromStateHashOf(query),
        .elementHash = TracingDecisionGraph::xorFactIntoHash(Hash(HashAlgorithm::SHA256), reqHash, respHash),
    };
}

trace::QueryApply makeApplyResultQuery(
    const Subject & applyResultSubject, const Hash & argAncestry,
    const std::vector<ObservationSet> & history, size_t step)
{
    if (!std::holds_alternative<ApplyResultSubject>(applyResultSubject.data))
        throw Error("makeApplyResultQuery: subject is not an ApplyResultSubject");

    auto par = pathAndRootsFromSubject(applyResultSubject);
    if (par.path.steps.size() != 1
        || par.path.steps[0].kind != trace::PathStep::Kind::Apply
        || !par.path.steps[0].fnPath
        || !par.path.steps[0].argPath)
        throw Error("makeApplyResultQuery: unexpected path shape");
    const auto & applyStep = par.path.steps[0];

    trace::QueryApply q;
    q.fromStateHashes.reserve(par.roots.size());
    for (auto & root : par.roots) {
        auto cid = stateHashAt(root, argAncestry, history, step);
        q.fromStateHashes.emplace_back(hashHex(cid));
    }
    q.fnPath = *applyStep.fnPath;
    q.argPath = *applyStep.argPath;
    q.fnRootIndex = applyStep.fnRootIndex;
    q.argRootIndex = applyStep.argRootIndex;
    return q;
}

Hash stateHashAtStamping(
    const Subject & subject,
    const Hash & argAncestry,
    const std::vector<ObservationSet> & history,
    size_t step,
    const std::function<void(const EvolutionStep &)> & hook)
{
    /* Mirrors stateHashAt's fold logic, emitting one
       EvolutionStep per matched observation. Subject-evolution walker will
       navigate via a table stamped from these emissions.

       Within a single history edge, all observations are matched
       against the edge-entry state hash (not the accumulated
       one). This means multiple observations in the same edge
       fold into the SAME curBefore; the emitted `curAfter` is
       curBefore XOR obs.elem (per-observation), not the
       edge-cumulative fold. Subject-evolution walker will replicate this
       edge-scoped semantics — cur updates at edge boundaries,
       not per-observation. */
    Hash result = stateHashAt(subject, argAncestry, history, step);
    Hash argSubjectHash = stateHashAt(subject, Hash(HashAlgorithm::SHA256), {}, 0);
    Hash selfFactFold = Hash(HashAlgorithm::SHA256);
    for (size_t k = 0; k < step && k < history.size(); ++k) {
        Hash myScopeStateIdAtK = TracingDecisionGraph::xorHashes(
            stateHashAt(subject, argAncestry, history, k), Hash(HashAlgorithm::SHA256));
        /* Above is `stateHashAt(subject, argAncestry, history, k)` —
           subject's state at edge k's precondition. */
        for (auto & obs : history[k].observations) {
            if (obs.fromHash == myScopeStateIdAtK) {
                Hash curAfter = TracingDecisionGraph::xorHashes(myScopeStateIdAtK, obs.elementHash);
                if (hook)
                    hook(EvolutionStep{myScopeStateIdAtK, obs.fromHash, obs.elementHash, curAfter});
                selfFactFold = TracingDecisionGraph::xorHashes(selfFactFold, obs.elementHash);
            }
        }
    }
    (void) argSubjectHash;  /* Reserved for stamping the subject's Merkle key. */
    return result;
}

Hash stateHashAt(const Subject & subject, const Hash & argAncestry, const std::vector<ObservationSet> & history, size_t step)
{
    /* Compute subject's state hash at the precondition of the
       `step`-th edge by replaying the first `step` edges'
       effects on the subject's running state hash.

       Inheritance: `argAncestry` is the XOR of outer-argAncestry state hashes (chiefly
       the cached call's state hash(Q) at the cb-apply). Passing
       zero gives the pure structural id. Leaf subjects
       (Arg, PostulatedIdempotentRead) XOR `argAncestry` into their
       base hash. Composite subjects (DerivedSubject,
       ApplyResultSubject) propagate `argAncestry` recursively through
       their constituents' state hashes; the structural derivation
       at this level uses those scoped constituents' values in its
       query payload, so inheritance ripples through naturally
       without a second XOR at this level.

       For positional seeds, only direct observations matter:
       facts in earlier edges whose `from` equals the arg's
       precondition state hash at that edge contribute to its
       evolution. */
    return std::visit(
        [&](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;

            /* The subject's id at edge step `k`, BEFORE this subject's
               selfFactFold gets XOR'd in. Naming note: not pure
               De-Bruijn-style "structural" for all variants. For
               Arg / PostulatedIdempotentRead it IS k-invariant
               pure position. For ApplyResultSubject it depends on `k`
               because it composes the constituents' *fully evolved*
               state hashes (= constituents' stateHashAt at the
               same k) into a SHA-sealed shape — so the apply's id
               varies with k via constituent evolution even before
               this subject's selfFactFold contributes. */
            auto subjectIdAt = [&](size_t k) -> Hash {
                if constexpr (std::is_same_v<T, Arg>) {
                    auto base = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
                    return TracingDecisionGraph::xorHashes(base, argAncestry);
                } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                    /* Derived subjects have no state hash — only an address
                       (= producer query hash). Callers that need an
                       address for any subject use `stateHashAtSubject`;
                       reaching this branch via `stateHashAt` means a
                       caller passed a derived subject where the design
                       requires an argument-level subject. */
                    nix::unreachable();
                } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                    /* Apply-result composes its constituents' state hashes.
                       Constituents may be Derived → route through
                       stateHashAtSubject (which dispatches Derived to
                       the producer-query-hash path). */
                    auto fnAtK = stateHashAtSubject(*alt.fn, argAncestry, history, k);
                    auto argAtK = stateHashAtSubject(*alt.arg, argAncestry, history, k);
                    nlohmann::json qj = trace::QueryApply{hashHex(fnAtK), hashHex(argAtK)};
                    return hashString(HashAlgorithm::SHA256, qj.dump());
                } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                    /* X is treated as argAncestry-saturated. Callers pass
                       hashes that already encode the relevant argAncestry
                       — `OuterObject::getStateHash()` returns
                       stateHashAfterSubject with argAncestry
                       baked in; ReplayCallbackArg's localId is
                       stateHashAfter(Arg{D}, callArgAncestry, {})
                       which is also argAncestry-saturated. Re-XORing argAncestry
                       here would either double-XOR (= argAncestry-saturated
                       inputs) or under-XOR (= un-scoped inputs); treating
                       PostulatedIdempotentRead as a pre-computed state hash
                       atom (return alt.hash unchanged) avoids both. */
                    return alt.hash;
                } else {
                    throw Error("stateHashAt: unknown subject variant");
                }
            };

            /* Walk the chain and accumulate the XOR-fold of env-layer fact
               element hashes from observations that point at THIS
               subject. An observation in `history[k]` points at this
               subject iff `obs.fromHash` (= the recorder-stamped
               subject pointer carried in the `from` field of the
               query that produced the fact) equals this subject's
               running state hash at step k. The result is the
               contribution to state hash that comes from env-layer facts
               about self. */
            Hash selfFactFold = Hash(HashAlgorithm::SHA256);
            std::string foldTrace;
            for (size_t k = 0; k < step && k < history.size(); ++k) {
                Hash myScopeStateIdAtK = TracingDecisionGraph::xorHashes(subjectIdAt(k), selfFactFold);
                for (auto & obs : history[k].observations) {
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

            auto result = TracingDecisionGraph::xorHashes(subjectIdAt(step), selfFactFold);
            tracingCacheLog(
                "stateHashAt: subject=%s argAncestry=%s history.size=%zu step=%zu\n"
                "  subjectIdAt(step)=%s selfFactFold=%s result=%s%s",
                describe(subject),
                hashHex(argAncestry).substr(0, 12),
                history.size(), step,
                hashHex(subjectIdAt(step)).substr(0, 12),
                hashHex(selfFactFold).substr(0, 12),
                hashHex(result).substr(0, 12),
                foldTrace);
            return result;
        },
        subject.data);
}

Hash stateHashAfter(const Subject & subject, const Hash & argAncestry, const std::vector<ObservationSet> & history)
{
    return stateHashAt(subject, argAncestry, history, history.size());
}

Hash stateHashConverged(const Subject & subject, const Hash & argAncestry, const std::vector<ObservationSet> & history)
{
    /* Flatten history into deduped observation pool keyed by
       (fromHash, elementHash). Order within `history` is discarded —
       the greedy partition below only reads `fromHash` for state-
       match and XOR-folds `elementHash`, both order-insensitive. */
    std::vector<Observation> flat;
    std::set<std::pair<Hash, Hash>> seen;
    for (auto & edge : history)
        for (auto & obs : edge.observations) {
            auto key = std::make_pair(obs.fromHash, obs.elementHash);
            if (seen.insert(key).second) flat.push_back(obs);
        }
    /* Greedy state-match partition: at each round, pull every obs
       whose fromHash == subject's current state into a synthetic
       edge; append; recompute the subject's state; repeat until no
       obs matches. `stateHashAt(subj, argAncestry, hypWalk, hypWalk.size())`
       XOR-folds each edge's matching obs into the running state,
       so state advances one round per iteration.

       Termination: each non-break round consumes at least one obs
       from `flat` (partition is non-empty when we don't break), so
       `flat.size()` strictly decreases. Bounded by initial pool
       size without an explicit numeric cap. */
    std::vector<ObservationSet> hypWalk;
    while (!flat.empty()) {
        auto currentId = stateHashAt(subject, argAncestry, hypWalk, hypWalk.size());
        ObservationSet partition;
        std::vector<Observation> stillRemaining;
        for (auto & obs : flat) {
            if (obs.fromHash == currentId) partition.observations.push_back(obs);
            else stillRemaining.push_back(obs);
        }
        if (partition.observations.empty()) break;
        hypWalk.push_back(std::move(partition));
        flat = std::move(stillRemaining);
    }
    return stateHashAt(subject, argAncestry, hypWalk, hypWalk.size());
}

Hash producerQueryHashAt(
    const DerivedSubject & derived,
    const Hash & argAncestry,
    const std::vector<ObservationSet> & history,
    size_t step)
{
    auto [pathToParent, parentRoots] = pathAndRootsFromSubject(*derived.parent);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(parentRoots.size());
    for (auto & root : parentRoots) {
        auto rootStateHash = stateHashAt(root, argAncestry, history, step);
        fromStateHashes.emplace_back(hashHex(rootStateHash));
    }
    auto fromLeaf = fromStateHashes.empty() ? trace::QueryLeaf("") : fromStateHashes[0];
    nlohmann::json qj;
    if (derived.kind == DerivedSubject::Kind::GetAttr) {
        trace::QueryGetAttr q{derived.name, fromLeaf};
        q.path = pathToParent;
        q.fromStateHashes = fromStateHashes;
        qj = q;
    } else {
        trace::QueryGetListElem q{fromLeaf, derived.index};
        q.path = pathToParent;
        q.fromStateHashes = fromStateHashes;
        qj = q;
    }
    return hashString(HashAlgorithm::SHA256, qj.dump());
}

Hash producerQueryHashAfter(
    const DerivedSubject & derived, const Hash & argAncestry, const std::vector<ObservationSet> & history)
{
    return producerQueryHashAt(derived, argAncestry, history, history.size());
}

Hash stateHashAtSubject(
    const Subject & subject, const Hash & argAncestry, const std::vector<ObservationSet> & history, size_t step)
{
    /* Polymorphic dispatch: DerivedSubjects have no own state hash,
       so we key them by their producer query's hash instead. Every
       other variant delegates to the strict `stateHashAt`. */
    if (auto * derived = std::get_if<DerivedSubject>(&subject.data))
        return producerQueryHashAt(*derived, argAncestry, history, step);
    return stateHashAt(subject, argAncestry, history, step);
}

Hash stateHashAfterSubject(const Subject & subject, const Hash & argAncestry, const std::vector<ObservationSet> & history)
{
    return stateHashAtSubject(subject, argAncestry, history, history.size());
}

std::string describe(const Subject & subject)
{
    return std::visit(
        [](const auto & alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Arg>) {
                return "arg(" + std::to_string(alt.depth) + ")";
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

} // namespace nix
