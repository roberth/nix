builtins.readSymlink (builtins.makePath {
  root = {
    type = "symlink";
    target = 42;
  };
})
