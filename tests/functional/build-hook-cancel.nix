with import ./config.nix;

{
  # Goes to the build hook, which never answers.
  remote = mkDerivation {
    name = "build-hook-cancel-remote";
    buildCommand = "echo remote > $out";
  };

  # Builds locally, without asking the hook, and fails once the hook
  # runs, so that the failure drops the goal that waits for the hook.
  fail = mkDerivation {
    name = "build-hook-cancel-fail";
    preferLocalBuild = true;
    hookStarted = builtins.getEnv "HOOK_STARTED";
    buildCommand = ''
      while [[ ! -e "$hookStarted" ]]; do
        sleep 0.1
      done
      exit 1
    '';
  };
}
