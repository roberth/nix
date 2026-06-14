# "." is a special path component (current directory). It is not a
# valid entry name and should be rejected when the directory is
# enumerated.
builtins.readDir (builtins.makePath {
  root = {
    type = "directory";
    entries = {
      "." = {
        type = "regular";
        contents = "";
      };
    };
  };
})
