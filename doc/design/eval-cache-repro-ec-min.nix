# Paired standalone reproducer for the eval-cache warm-recursion bug.
#
# Companion to nixpkgs branch `eval-cache-minimization-repro`. Fires
# cold AND warm with `infinite recursion` at
# nixpkgs/lib/modules.nix:372:15 (evaluating the `options` module
# argument for a deeply-nested import) when run against nixpkgs
# unmodified from that branch.
#
# Run:
#   NIX_TRACING_CACHE_DIR=/tmp/repro-cache \
#     nix --extra-experimental-features tracing-eval-cache eval \
#     --impure --raw --show-trace -f doc/design/eval-cache-repro-ec-min.nix
#
# Reduction status: 34 lines. Cannot be reduced further without
# losing the trigger. Verified reductions that DO drop the trigger:
#   - `baseModules = []` in the evalConfig call → both cold and warm
#     succeed. Something in nixos/modules/module-list.nix (transitive
#     includes) is required to fire the recursion.
#   - Replacing `evalConfig` with plain `lib.evalModules` (with
#     `class = "nixos"` and pkgs threaded via `_module.args.pkgs`
#     to match) → both cold and warm succeed.
#   - Replacing `pkgs.ghostunnel.services.default` with an inline
#     `builtins.cache { expr = ...; }` returning an equivalent
#     empty module → both cold and warm succeed.
#
# So the trigger is jointly:
#   1. `evalConfig` with real `baseModules` (not [])
#   2. A cache-bridged navigation to a module-fn value
#      (`pkgs.ghostunnel.services.default` reached via `cachedNixpkgs`)
#   3. A nested `lib.evalModules` using that module fn under
#      `submoduleWith`, forced via `lib.optionAttrSetToDocList`
#
# Neither ingredient alone triggers it in isolation.
let
  lib = import /home/sandbox/nixpkgs/lib;
  cachedNixpkgs = lib.cache { import = /home/sandbox/nixpkgs; };
  evalConfig = import /home/sandbox/nixpkgs/nixos/lib/eval-config.nix;
  cfg = evalConfig {
    system = "x86_64-linux";
    pkgs = cachedNixpkgs { config.allowUnfreePredicate = _: true; overlays = []; };
    modules = [
      # Minimum stubs to satisfy evalConfig sanity.
      { fileSystems."/" = { device = "/dev/vda1"; fsType = "ext4"; };
        boot.loader.grub.enable = false;
        system.stateVersion = "24.05"; }
      # The trigger: an inner evalModules using pkgs.ghostunnel via submoduleWith.
      ({ pkgs, lib, ... }: {
        environment.systemPackages = [(
          let
            eval = lib.evalModules {
              modules = [{
                options."<svc>" = lib.mkOption {
                  type = lib.types.submoduleWith {
                    modules = [ pkgs.ghostunnel.services.default ];
                  };
                };
              }];
            };
          in pkgs.runCommand "probe" {
            options = builtins.unsafeDiscardStringContext (builtins.toJSON (
              lib.optionAttrSetToDocList eval.options
            ));
            passAsFile = [ "options" ];
          } "cp $optionsPath $out"
        )];
      })
    ];
  };
in cfg.config.system.build.toplevel.drvPath
