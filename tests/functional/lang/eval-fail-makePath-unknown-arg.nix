# `builtins.makePath` accepts no attributes besides `root`. `name` in
# particular is rejected: paths produced by `makePath` have no name
# (a name is a storepath concern; set one via `builtins.path { path
# = ...; name = ...; }` at the materialisation site).
builtins.makePath {
  root = {
    type = "regular";
    contents = "";
  };
  name = "tempting-but-rejected";
}
