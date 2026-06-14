# Entry names are file names, not paths. A `/` in the name is invalid
# and should be rejected when the directory is enumerated.
builtins.readDir (builtins.makePath {
  root = {
    type = "directory";
    entries = {
      "a/b" = {
        type = "regular";
        contents = "";
      };
    };
  };
})
