#pragma once
/**
 * @file
 * Strong type wrappers for the identifiers in the tracing/caching
 * system. Prevents accidental mixing of:
 *
 * - ValueHandle: JSON trace correlation handle (TraceSink)
 * - OuterId:   OuterResolver registry handle, a content Hash.
 *                Arg roots use the empty-set hash (their
 *                state hash at apply time, since no
 *                observations have happened yet); derived values
 *                use the producer query's queryHash.
 */

#include "nix/util/hash.hh"

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
    T raw{};

    StrongId() = default;

    explicit StrongId(T v)
        : raw(v)
    {
    }

    T value() const
    {
        return raw;
    }

    auto operator<=>(const StrongId &) const = default;
    bool operator==(const StrongId &) const = default;
};

struct ValueHandleTag
{};

/** JSON trace correlation handle (links Query and Result entries). */
using ValueHandle = StrongId<ValueHandleTag, uint64_t>;

/** OuterResolver registry handle for outer/local values.
 *
 *  A content Hash. Arg roots are `hashString("arg:N")` /
 *  `hashString("local:N")` for an interpreter-side counter N;
 *  derived values are the producer query's `queryHash`. Both go
 *  through the same map. */
using OuterId = Hash;

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
