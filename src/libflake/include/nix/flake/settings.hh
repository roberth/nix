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
        false,
        "flake-default-copy-to-store",
        R"(
          The default value for `inputs.<name>.copyToStore` (and
          `inputs.self.copyToStore`) when a flake doesn't set it
          explicitly. When `true`, every flake input's `flake.nix` is
          imported as a store-path string rather than as a path
          Value, so structural primops (`dirOf`, `baseNameOf`,
          anything routing through `lib.path.deconstructPath`) walk
          through the storepath the way they did before lazy-paths.

          Provided so users of flakes that target older Nix versions
          (which don't recognise the `copyToStore` attribute) can opt
          into the eager shape without modifying those flakes. The
          per-input `copyToStore` setting on a flake input always
          wins over this default.
        )",
        {},
        true,
        Xp::Flakes};
};

} // namespace nix::flake
