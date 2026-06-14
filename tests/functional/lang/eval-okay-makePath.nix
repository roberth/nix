let

  inherit (builtins)
    baseNameOf
    dirOf
    isPath
    makePath
    pathExists
    readDir
    readFile
    readFileType
    readSymlink
    toString
    typeOf
    ;

  # Wraps a bare node as a root-only makePath. Used for the type-tag
  # round-trips against readFileType.
  mkOnly = node: makePath { root = node; };

  # Shared fixture: a small directory with a regular file, an empty
  # subdir, and a symlink to the regular file. Used across
  # readFile / readDir / readFileType / readSymlink / pathExists
  # checks.
  tree = makePath {
    root = {
      type = "directory";
      entries = {
        "hello.txt" = {
          type = "regular";
          contents = "Hello, world!\n";
        };
        "subdir" = {
          type = "directory";
          entries = { };
        };
        "link" = {
          type = "symlink";
          target = "hello.txt";
        };
      };
    };
  };

in

# Each `type` discriminator round-trips through readFileType.
assert readFileType (mkOnly { type = "regular"; contents = ""; }) == "regular";
assert readFileType (mkOnly { type = "directory"; entries = { }; }) == "directory";
assert readFileType (mkOnly { type = "symlink"; target = "."; }) == "symlink";
assert readFileType (mkOnly { type = "unknown"; }) == "unknown";

# readFile returns a regular node's contents.
assert readFile (tree + "/hello.txt") == "Hello, world!\n";

# readFile follows a benign in-tree symlink to its target's contents.
assert readFile (tree + "/link") == "Hello, world!\n";

# readSymlink returns the resolved target path, readable through the
# same lazy accessor.
assert readFile (readSymlink (tree + "/link")) == "Hello, world!\n";

# readFileType on the symlink itself is "symlink" (lstat, no follow).
assert readFileType (tree + "/link") == "symlink";

# readDir lists every entry with its type.
assert readDir tree == {
  "hello.txt" = "regular";
  "link" = "symlink";
  "subdir" = "directory";
};

# pathExists is true for present entries and false for absent ones.
assert pathExists (tree + "/hello.txt");
assert !pathExists (tree + "/absent");

# The returned value is a path with no basename, and is its own parent.
assert typeOf tree == "path";
assert isPath tree;
assert baseNameOf tree == "";
assert dirOf tree == tree;

# The primop does not walk its argument: reflexive equality on a
# value whose `root` would throw if forced still succeeds.
assert (let p = makePath { root = throw "argument not walked"; }; in p == p);

# Reading one entry does not force a sibling whose value would throw.
assert (
  let
    laxTree = makePath {
      root = {
        type = "directory";
        entries = {
          "good" = {
            type = "regular";
            contents = "good";
          };
          "broken" = throw "sibling forced";
        };
      };
    };
  in
  readFile (laxTree + "/good") == "good"
);

# A path-valued node mounts the referenced path at the node's
# position; reading the resulting tree matches reading the original.
assert readDir (makePath { root = ./readDir; }) == {
  bar = "regular";
  foo = "directory";
  ldir = "symlink";
  linked = "symlink";
};

# `contents` accepts either a string (literal bytes) or a path (bytes
# read through the path's own accessor on force).
assert (
  let
    via = makePath {
      root = {
        type = "directory";
        entries = {
          "from-string" = {
            type = "regular";
            contents = "literal";
          };
          "from-path" = {
            type = "regular";
            contents = ./readDir/bar;
          };
        };
      };
    };
  in
  readFile (via + "/from-string") == "literal"
  && readFile (via + "/from-path") == ""
);

# The `executable` bit reaches the materialised storepath: two files
# with identical contents but differing executable bits hash to
# distinct storepaths. Asserted as inequality so the test is
# insensitive to the exact hash (and to `storeDir`).
assert (
  let
    reg = makePath {
      root = {
        type = "regular";
        contents = "x";
      };
    };
    exe = makePath {
      root = {
        type = "regular";
        contents = "x";
        executable = true;
      };
    };
  in
  toString reg != toString exe
);

# A `makePath` tree may mount itself; laziness keeps a finite query
# productive.
assert (
  let
    cyc = makePath {
      root = {
        type = "directory";
        entries = {
          "leaf" = {
            type = "regular";
            contents = "deep";
          };
          "self" = cyc;
        };
      };
    };
  in
  readFile (cyc + "/self/self/self/leaf") == "deep"
);

# `import` walks the tree, finds default.nix, and evaluates it. The
# throw-bodied sibling is never forced.
assert
  import (makePath {
    root = {
      type = "directory";
      entries = {
        "default.nix" = {
          type = "regular";
          contents = "1 + 1";
        };
        "broken.txt" = throw "illustratively broken";
      };
    };
  }) == 2;

# After materialisation, symlink targets that would be rejected
# pre-materialisation (absolute, or `..`-escaping) resolve through
# the OS at the materialised storepath. Both subcases below point
# at another store path materialised from a sibling makePath.
assert (
  let
    target = makePath {
      root = {
        type = "regular";
        contents = "hello from target";
      };
    };
    withAbs = makePath {
      root = {
        type = "directory";
        entries = {
          "link" = {
            type = "symlink";
            target = toString target;
          };
        };
      };
    };
    withDotDot = makePath {
      root = {
        type = "directory";
        entries = {
          "link" = {
            type = "symlink";
            target = "../" + baseNameOf (toString target);
          };
        };
      };
    };
  in
  readFile (toString withAbs + "/link") == "hello from target"
  && readFile (toString withDotDot + "/link") == "hello from target"
);

"ok"
