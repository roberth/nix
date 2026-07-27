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
                /* #186: Arg's identity is now computeSelectorHash of the
                   corresponding SelectorArg — the algebraic Selector-content
                   hash, not the ad-hoc "positional-D" string. argAncestry
                   would have been XOR'd in the old scheme; under stable Q
                   hashes it retires (per #178). */
                (void) argAncestry;
                return TracingDecisionGraph::computeSelectorHash(trace::SelectorArg{alt.depth});
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                return alt.hash;
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                /* #186: identity aligned with SelectorApply's own
                   content hash. Arg is observed by value (per #181's
                   SelectorApply shape) — discrimination flows through
                   cur, not through subject id. */
                auto fnId = subjectId(*alt.fn, argAncestry);
                return TracingDecisionGraph::computeSelectorHash(
                    trace::SelectorApply{hashHex(fnId)});
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                /* #186: identity aligned with SelectorGetAttr /
                   SelectorGetListElem's own content hash. */
                auto parentId = subjectId(*alt.parent, argAncestry);
                if (alt.kind == DerivedSubject::Kind::GetAttr) {
                    return TracingDecisionGraph::computeSelectorHash(
                        trace::SelectorGetAttr{alt.name, hashHex(parentId)});
                } else {
                    return TracingDecisionGraph::computeSelectorHash(
                        trace::SelectorGetListElem{hashHex(parentId), alt.index});
                }
            } else {
                throw Error("subjectId: unknown subject variant");
            }
        },
        subject.data);
}

trace::SelectorVariant subjectAsSelector(const Subject & subject, const Hash & argAncestry)
{
    return std::visit(
        [&](const auto & alt) -> trace::SelectorVariant {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Arg>) {
                return trace::SelectorArg{alt.depth};
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                /* No corresponding Selector — represent as GetWHNF{from=hash hex}.
                   This is the "pre-Selector" identity carrier for base-scope
                   values (fresh evalFile/evalExpr, primop args). */
                return trace::SelectorGetWHNF{alt.hash.to_string(HashFormat::Base16, false)};
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                auto fnId = subjectId(*alt.fn, argAncestry);
                return trace::SelectorApply{fnId.to_string(HashFormat::Base16, false)};
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentId = subjectId(*alt.parent, argAncestry);
                auto parentHex = parentId.to_string(HashFormat::Base16, false);
                if (alt.kind == DerivedSubject::Kind::GetAttr) {
                    return trace::SelectorGetAttr{alt.name, parentHex};
                } else {
                    return trace::SelectorGetListElem{parentHex, alt.index};
                }
            } else {
                throw Error("subjectAsSelector: unknown subject variant");
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
