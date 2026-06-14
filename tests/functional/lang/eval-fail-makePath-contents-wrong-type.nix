builtins.readFile (builtins.makePath {
  root = {
    type = "regular";
    contents = 42;
  };
})
