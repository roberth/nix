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

Hash subjectId(const Subject & subject)
{
    /* #178: state-hash evolution retired. Every Subject variant
       reduces to a structural id at the initial precondition,
       aligned with the equivalent Selector's content hash (#186). */
    return std::visit(
        [](const auto & alt) -> Hash {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Arg>) {
                return TracingDecisionGraph::computeSelectorHash(trace::SelectorArg{alt.depth});
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                return alt.hash;
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                /* #181: arg dropped from SelectorApply payload; discrimination
                   flows through cur, not through subject id. */
                auto fnId = subjectId(*alt.fn);
                return TracingDecisionGraph::computeSelectorHash(
                    trace::SelectorApply{hashHex(fnId)});
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentId = subjectId(*alt.parent);
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

trace::SelectorVariant subjectAsSelector(const Subject & subject)
{
    return std::visit(
        [](const auto & alt) -> trace::SelectorVariant {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Arg>) {
                return trace::SelectorArg{alt.depth};
            } else if constexpr (std::is_same_v<T, PostulatedIdempotentRead>) {
                /* No corresponding Selector — represent as GetWHNF{from=hash hex}.
                   Pre-Selector identity carrier for base-scope values. */
                return trace::SelectorGetWHNF{alt.hash.to_string(HashFormat::Base16, false)};
            } else if constexpr (std::is_same_v<T, ApplyResultSubject>) {
                auto fnId = subjectId(*alt.fn);
                return trace::SelectorApply{fnId.to_string(HashFormat::Base16, false)};
            } else if constexpr (std::is_same_v<T, DerivedSubject>) {
                auto parentId = subjectId(*alt.parent);
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
