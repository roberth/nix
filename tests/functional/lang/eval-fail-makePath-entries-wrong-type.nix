builtins.readDir (builtins.makePath {
  root = {
    type = "directory";
    entries = "nope";
  };
})
