#pragma once

#include "nix/fetchers/fetchers.hh"
#include "nix/util/fun.hh"

namespace nix::fetchers {

enum class UseRegistries : int;
struct Settings;

struct InputCache
{
    /**
     * `accessor` is a thunk so consumers that only need the locked
     * input or extra attrs (e.g. lockfile rendering, sourceInfo
     * metadata) don't pay for the fetcher's accessor materialisation.
     * Fire the thunk (`accessor()`) when actually reading through
     * the source. Passing the thunk directly to APIs that accept a
     * `fun<ref<SourceAccessor>()>` (e.g. `MountedSourceAccessor::mount`)
     * keeps the deferral end-to-end.
     */
    struct CachedResult
    {
        fun<ref<SourceAccessor>()> accessor;
        Input resolvedInput;
        Input lockedInput;
        Attrs extraAttrs;
    };

    CachedResult
    getAccessor(const Settings & settings, Store & store, const Input & originalInput, UseRegistries useRegistries);

    struct CachedInput
    {
        Input lockedInput;
        fun<ref<SourceAccessor>()> accessor;
        Attrs extraAttrs;
    };

    virtual std::optional<CachedInput> lookup(const Input & originalInput) const = 0;

    virtual void upsert(Input key, CachedInput cachedInput) = 0;

    virtual void clear() = 0;

    static ref<InputCache> create();

    virtual ~InputCache() = default;
};

} // namespace nix::fetchers
