# Materialisation (here via `toString`) writes a NAR for the tree.
# NAR can only represent regular files, directories, and symlinks; a
# tree containing an `"unknown"`-typed node fails the NAR dump.
toString (builtins.makePath {
  root = {
    type = "directory";
    entries = {
      "socket" = {
        type = "unknown";
      };
    };
  };
})
