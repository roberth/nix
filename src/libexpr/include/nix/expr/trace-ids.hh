#pragma once
/**
 * @file
 * Strong type wrappers for the identifiers in the tracing/caching
 * system. Prevents accidental mixing of:
 *
 * - ValueHandle: JSON trace correlation handle (TraceSink)
 * - OuterId:   a Subject-derived state hash used in payloads referring
 *                to outer values. Alias for TracingHash; distinguishes
 *                Subject-state-hash uses from other TracingHash uses at
 *                call sites. Not a registry key — outer Objects flow
 *                directly through queryFn/applyFn.
 */

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace nix {

/**
 * Fixed 128-bit hash for the tracing/cache pipeline. Stored as a raw
 * byte array — no algorithm tag, no hashSize field. Comparisons memcmp
 * a fixed 16 bytes; hashing for unordered containers uses the first 8
 * bytes directly (SHA-256 output is uniform, so no further mixing
 * helps).
 *
 * Every tracing atom key (Request/Response/Query/Result/SetHash), every
 * cell state hash, every rst FrozenNode identity is a TracingHash.
 * Construct via `TracingHash::compute(bytes)` — that boundary is the
 * only place SHA-256 (or a future BLAKE3) leaks in. Tracing code never
 * sees nix::Hash; the tracing world and the general nix::Hash world
 * are separate.
 *
 * Wins over storing nix::Hash: 5× smaller (16 vs ~80 bytes), simpler
 * memcmp path, better cache-line packing for the hash-keyed containers
 * that dominate memory. The SHA-256 → BLAKE3 swap becomes a single-line
 * change at `compute()`.
 */
struct TracingHash
{
    static constexpr size_t size = 16;
    std::array<uint8_t, size> bytes{};

    /** Compute from bytes via SHA-256, truncated to first 16 bytes. */
    static TracingHash compute(std::string_view input);

    /** All-zero identity — the neutral element for XOR-fold. */
    static TracingHash zero() { return TracingHash{}; }

    /** Parse 32 lowercase hex characters. Throws on malformed input. */
    static TracingHash parseHex(std::string_view hex);

    /** Lowercase 32-character hex. */
    std::string toHex() const;

    /** Bytewise XOR into a fresh TracingHash. */
    TracingHash xorWith(const TracingHash & other) const
    {
        TracingHash r;
        for (size_t i = 0; i < size; ++i)
            r.bytes[i] = bytes[i] ^ other.bytes[i];
        return r;
    }

    /** Bytewise XOR in place (self ^= other). */
    void xorInPlace(const TracingHash & other)
    {
        for (size_t i = 0; i < size; ++i)
            bytes[i] ^= other.bytes[i];
    }

    bool operator==(const TracingHash &) const = default;
    auto operator<=>(const TracingHash &) const = default;
};

/**
 * CRTP-free strong id wrapper. Tag type prevents cross-assignment.
 * Not default-constructible — callers must supply a value.
 */
template<typename Tag, typename T>
struct StrongId
{
    T raw;

    explicit StrongId(T v)
        : raw(std::move(v))
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

/** A Subject-derived state hash. Alias for TracingHash; marks call-site
 *  intent — this hash is the state hash of some outer/local Subject.
 *  No registry lookup involved: outer Objects flow directly through
 *  queryFn/applyFn now. */
using OuterId = TracingHash;

/** Phantom-typed hash wrapper: distinct Phantom types produce distinct
    HashOf types that can't cross-assign. Prevents mixing hashes with
    different semantic kinds (e.g., a request hash vs a result hash).
    Reads naturally: HashOf<Request> = "the hash of a Request". */
template<typename Phantom>
using HashOf = StrongId<Phantom, TracingHash>;

} // namespace nix

// std::hash specialisation for StrongId — delegates to the wrapped type.
template<typename Tag, typename T>
struct std::hash<nix::StrongId<Tag, T>>
{
    size_t operator()(const nix::StrongId<Tag, T> & id) const noexcept
    {
        return std::hash<T>{}(id.raw);
    }
};

// std::hash specialisation for TracingHash — first 8 bytes are already
// a uniform hash (SHA-256 output), no need to mix further.
template<>
struct std::hash<nix::TracingHash>
{
    size_t operator()(const nix::TracingHash & h) const noexcept
    {
        size_t out;
        static_assert(sizeof(out) <= nix::TracingHash::size);
        std::memcpy(&out, h.bytes.data(), sizeof(out));
        return out;
    }
};
