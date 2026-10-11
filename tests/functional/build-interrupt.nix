with import ./config.nix;

mkDerivation {
  name = "build-interrupt";
  pidFile = builtins.getEnv "BUILDER_PID_FILE";
  # When this file exists, the builder finishes at once instead of
  # waiting to be killed.
  doneFile = builtins.getEnv "BUILDER_DONE_FILE";
  buildCommand = ''
    if [[ -n "$doneFile" && -e "$doneFile" ]]; then
      echo done > $out
      exit 0
    fi
    echo $$ > "$pidFile"
    sleep 1000
  '';
}
