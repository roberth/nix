#pragma once

#include "source-accessor.hh"
#include "fun.hh"

namespace nix {

struct MountedSourceAccessor : SourceAccessor
{
    /**
     * Register a mount whose accessor is produced on demand.
     *
     * The thunk is invoked at most once per mount point — on the first
     * `getMount` (or `resolve`-driven access) that hits this point.
     * Subsequent accesses share the cached result.
     *
     * Callers whose thunk does real work (e.g. a fetcher walk) do not
     * need to wrap with `memo()` themselves — the mount table memoises
     * internally. Callers that already have a `ref<SourceAccessor>` in
     * hand can pass a trivial identity lambda; there's no work to
     * memoise so no overhead is added.
     */
    virtual void mount(CanonPath mountPoint, fun<ref<SourceAccessor>()> accessor) = 0;

    /**
     * Return the accessor mounted on `mountPoint`, or `nullptr` if
     * there is no such mount point. Fires the underlying thunk on
     * first call.
     */
    virtual std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) = 0;

    ~MountedSourceAccessor() override;
};

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, fun<ref<SourceAccessor>()>> mounts);

} // namespace nix
