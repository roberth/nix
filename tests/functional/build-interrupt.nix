with import ./config.nix;

mkDerivation {
  name = "build-interrupt";
  pidFile = builtins.getEnv "BUILDER_PID_FILE";
  buildCommand = ''
    echo $$ > "$pidFile"
    sleep 1000
  '';
}
