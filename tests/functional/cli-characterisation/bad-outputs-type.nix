{
  type = "derivation";
  name = "bad-outputs-type-1.0";
  outPath = builtins.toFile "out" "";
  outputs = "out"; # should be a list
}
