#pragma once
/**
 * @file
 * TracingSourceAccessor - SourceAccessor wrapper that records file access traces.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/ref.hh"

#include <nlohmann/json.hpp>

#include <functional>

namespace nix {

/**
 * SourceAccessor wrapper that traces file read operations.
 *
 * Wraps another SourceAccessor and logs all file reads with content hashes.
 */
class TracingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> inner;
    std::function<void(const nlohmann::json &)> logFn;

public:
    TracingSourceAccessor(ref<SourceAccessor> inner, std::function<void(const nlohmann::json &)> logFn);

    std::string readFile(const CanonPath & path) override;

    void readFile(
        const CanonPath & path, Sink & sink, std::function<void(uint64_t)> sizeCallback = [](uint64_t) {}) override;

    bool pathExists(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    std::string showPath(const CanonPath & path) override;

    AllowListSourceAccessor * asAllowListSourceAccessor() override;
};

} // namespace nix
