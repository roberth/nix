#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/util/environment-variables.hh"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace nix {

namespace {

struct Registry
{
    std::mutex mtx;
    std::unordered_map<Hash, nlohmann::json> entries;
};

Registry & registry()
{
    static Registry r;
    return r;
}

bool checkEnabled()
{
    return getEnv("NIX_CACHE_PROVENANCE_FILE").has_value();
}

/* Cached at first use. Not user-toggleable mid-run: the env var
   drives it, and re-checking every call is wasteful. */
bool cachedEnabled()
{
    static bool e = checkEnabled();
    return e;
}

void dumpNow()
{
    if (!cachedEnabled())
        return;
    auto path = getEnv("NIX_CACHE_PROVENANCE_FILE");
    if (!path)
        return;
    /* Append rather than overwrite: a nix invocation is one line
       per hash it registered; concurrent / repeated invocations
       each add their own lines, and the caller can sort/dedup at
       analysis time. */
    std::ofstream out(*path, std::ios::app);
    if (!out)
        return;
    std::lock_guard<std::mutex> lk(registry().mtx);
    for (auto & [h, entry] : registry().entries) {
        nlohmann::json row = entry;
        row["hash"] = h.to_string(HashFormat::Base16, false);
        out << row.dump() << "\n";
    }
}

/* Arm std::atexit exactly once, called from the first
   recordProvenance. Force registry construction *before*
   registering the atexit callback so it happens in the
   right sequence per [basic.start.term]/1: the atexit
   callback is guaranteed to run before the destructor of any
   object whose construction preceded the atexit call.
   If we called std::atexit first and let the registry
   initialize later, the registry would destruct *before*
   the atexit fires, and dumpNow would see an empty (or
   post-dtor) map. */
void armAtExitOnce()
{
    static bool armed = false;
    if (armed)
        return;
    armed = true;
    /* Touch the registry to force its function-static
       construction sequence before std::atexit. */
    (void) registry();
    std::atexit([]() { dumpNow(); });
}

} // namespace

bool provenanceEnabled()
{
    return cachedEnabled();
}

void recordProvenance(const Hash & h, std::string_view kind, nlohmann::json details)
{
    if (!cachedEnabled())
        return;
    armAtExitOnce();
    nlohmann::json entry;
    entry["kind"] = std::string(kind);
    entry["details"] = std::move(details);
    std::lock_guard<std::mutex> lk(registry().mtx);
    /* First registration wins. Under content-addressing, identical
       hashes come from identical inputs, so the first entry is the
       canonical record. A later call with the SAME hash but
       different details would indicate a hash collision or a bug in
       one of the sites — silently overwriting would hide it. */
    registry().entries.emplace(h, std::move(entry));
}

std::optional<nlohmann::json> lookupProvenance(const Hash & h)
{
    std::lock_guard<std::mutex> lk(registry().mtx);
    auto it = registry().entries.find(h);
    if (it == registry().entries.end())
        return std::nullopt;
    return it->second;
}

std::string describeHash(const Hash & h)
{
    auto prefix = h.to_string(HashFormat::Base16, false).substr(0, 12);
    if (auto p = lookupProvenance(h))
        return prefix + "[" + p->at("kind").get<std::string>() + "]";
    return prefix;
}

} // namespace nix
