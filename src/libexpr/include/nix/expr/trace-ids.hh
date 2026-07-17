#pragma once
/**
 * @file
 * Strong type wrappers for the identifiers in the tracing/caching
 * system. Prevents accidental mixing of:
 *
 * - ValueHandle: JSON trace correlation handle (TraceSink)
 * - OuterId:   a Subject-derived state hash used in payloads referring
 *                to outer values. Alias for Hash; distinguishes
 *                Subject-state-hash uses from other Hash uses at call
 *                sites. Not a registry key — outer Objects flow
 *                directly through queryFn/applyFn.
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

/** A Subject-derived state hash. Alias for Hash; marks call-site
 *  intent — this Hash is the state hash of some outer/local Subject.
 *  No registry lookup involved: outer Objects flow directly through
 *  queryFn/applyFn now. */
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
