#pragma once
///@file

#include <sys/types.h>
#include <string>

#include "nix/util/configuration.hh"

namespace nix {
// Forward declarations
struct EvalSettings;

} // namespace nix

namespace nix::flake {

struct Settings : public Config
{
    Settings();

    void configureEvalSettings(nix::EvalSettings & evalSettings) const;

    Setting<bool> useRegistries{
        this,
        true,
        "use-registries",
        "Whether to use flake registries to resolve flake references.",
        {},
        true,
        Xp::Flakes};

    Setting<bool> acceptFlakeConfig{
        this,
        false,
        "accept-flake-config",
        "Whether to accept Nix configuration settings from a flake without prompting.",
        {},
        true,
        Xp::Flakes};

    Setting<std::string> commitLockFileSummary{
        this,
        "",
        "commit-lock-file-summary",
        R"(
          The commit summary to use when committing changed flake lock files. If
          empty, the summary is generated based on the action performed.
        )",
        {"commit-lockfile-summary"},
        true,
        Xp::Flakes};

    Setting<bool> defaultCopyToStore{
        this,
        true,
        "flake-default-copy-to-store",
        R"(
          Fallback default for `inputs.<name>.copyToStore` (and
          `inputs.self.copyToStore`) when the flake being loaded
          neither declares them explicitly nor carries the
          `/.nix-flake-lazy-paths-supported` marker file at its
          root. When `true` (the default), such flakes'
          `outPath` on each input is a store-path string;
          structural primops (`dirOf`, `baseNameOf`, anything
          routing through `lib.path.deconstructPath`) walk through
          the storepath the way they did before lazy-paths — the
          shape existing flakes rely on.

          Priority (finest to coarsest):
          `inputs.<name>.copyToStore` > `inputs.self.copyToStore` >
          marker file at flake root > this setting.
        )",
        {},
        true,
        Xp::Flakes};
};

} // namespace nix::flake
