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

Hash fromStateHashOf(const trace::SelectorVariant & query)
{
    /* #178: `from` field is a transitional artifact. Return zero when
       absent or empty; parse when present. */
    return std::visit(
        [](const auto & q) -> Hash {
            if constexpr (requires { q.from; }) {
                if (!true || std::string{}.empty())
                    return Hash(HashAlgorithm::SHA256);
                return Hash::parseNonSRIUnprefixed(std::string{}, HashAlgorithm::SHA256);
            } else {
                return Hash(HashAlgorithm::SHA256);
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

Observation observationFromQR(const trace::SelectorVariant & query, const trace::ResultVariant & result)
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

trace::SelectorApply makeApplyResultQuery(
    const Subject & applyResultSubject, const Hash & argAncestry,
    const std::vector<ObservationSet> & history, size_t step)
{
    (void) history;
    (void) step;
    auto * ar = std::get_if<ApplyResultSubject>(&applyResultSubject.data);
    if (!ar || !ar->fn || !ar->arg)
        throw Error("makeApplyResultQuery: subject is not an ApplyResultSubject");
    trace::SelectorApply q;
    q.fn = hashHex(subjectId(*ar->fn, argAncestry));
    q.arg = hashHex(subjectId(*ar->arg, argAncestry));
    return q;
}

Hash subjectId(const Subject & subject, const Hash & argAncestry)
{
    /* #178: state-hash evolution retired. Every Subject variant
       reduces to a structural id at the initial precondition:
       leaves XOR argAncestry into their base; composites hash a
       canonical shape over their constituents' ids. */
    return std::visit(
        [&](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Arg>) {
                auto base = hashString(HashAlgorithm::SHA256, "positional-" + std::to_string(alt.depth));
                return TracingDecisionGraph::xorHashes(base, argAncestry);
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                return alt.hash;
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                auto fnId = subjectId(*alt.fn, argAncestry);
                auto argId = subjectId(*alt.arg, argAncestry);
                nlohmann::json qj = trace::SelectorApply{hashHex(fnId), hashHex(argId)};
                return hashString(HashAlgorithm::SHA256, qj.dump());
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentId = subjectId(*alt.parent, argAncestry);
                std::string s = alt.kind == DerivedSubject::Kind::GetAttr
                    ? "getAttr:" + alt.name
                    : "getListElem:" + std::to_string(alt.index);
                s += "@" + hashHex(parentId);
                return hashString(HashAlgorithm::SHA256, s);
            } else {
                throw Error("subjectId: unknown subject variant");
            }
        },
        subject.data);
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
