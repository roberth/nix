{
  type = "derivation";
  name = "bad-meta-type-1.0";
  outPath = builtins.toFile "out" "";
  meta = "not-an-attrset";
}
