#pragma once
/**
 * @file
 * Strong type wrappers for the various integer identifiers in the
 * tracing/caching system. Prevents accidental mixing of:
 *
 * - ValueHandle:   JSON trace correlation handle (TraceSink)
 * - VirtualRootId: identity for untraced values in QueryApply
 * - AmbientId:     AmbientResolver registry handle
 */

#include <compare>
#include <cstdint>
#include <functional>

namespace nix {

/**
 * CRTP-free strong id wrapper. Tag type prevents cross-assignment.
 */
template<typename Tag, typename T>
struct StrongId
{
    T raw;

    explicit StrongId(T v) : raw(v) {}

    T value() const { return raw; }

    auto operator<=>(const StrongId &) const = default;
    bool operator==(const StrongId &) const = default;
};

struct ValueHandleTag {};
struct VirtualRootIdTag {};
struct AmbientIdTag {};

/** JSON trace correlation handle (links Query and Result entries). */
using ValueHandle = StrongId<ValueHandleTag, uint64_t>;

/** Identity for values without trie provenance (e.g. {} literals). */
using VirtualRootId = StrongId<VirtualRootIdTag, uint64_t>;

/** AmbientResolver registry handle for outer/local values. */
using AmbientId = StrongId<AmbientIdTag, int>;

} // namespace nix

// std::hash specializations for use in unordered containers
template<typename Tag, typename T>
struct std::hash<nix::StrongId<Tag, T>>
{
    size_t operator()(const nix::StrongId<Tag, T> & id) const noexcept
    {
        return std::hash<T>{}(id.raw);
    }
};
