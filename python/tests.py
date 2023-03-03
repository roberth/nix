import nix
import unittest


class TestPythonNix(unittest.TestCase):
    def test_dict(self):
        val = dict(a=1)
        self.assertEqual(nix.eval("a", vars=dict(a=val)), val)

    def test_string(self):
        self.assertEqual(nix.eval("a", vars=dict(a="foo")), "foo")

    def test_bool(self):
        self.assertEqual(nix.eval("a", vars=dict(a=True)), True)

    def test_none(self):
        self.assertEqual(nix.eval("a", vars=dict(a=None)), None)

    def test_ifd(self):
        expression = """
        builtins.readFile (derivation {
          name = "test";
          args = [ "-c" "printf \\"%s\\" test > $out" ];
          builder = "/bin/sh";
          system = builtins.currentSystem;
        })
        """
        self.assertEqual(nix.eval(expression, vars=dict()), "test")

    def test_throw(self):
        errorString = "hello hi there\ntest"
        try:
            nix.eval("throw str", vars=dict(str=errorString))
        except nix.ThrownNixError as e:
            self.assertEqual(e.args[0], errorString)

    def test_syntax_error(self):
        try:
            nix.eval("{")
        except nix.ThrownNixError as e:
            self.assertTrue(False)
        except nix.NixError as e:
            self.assertTrue(True)

    def test_GIL_case(self):
        try:
            nix.eval("{ a = throw \"nope\"; }")
        except nix.NixError as e:
            self.assertEqual(e.args[0], "nope")

    def test_infinity(self):
        try:
            nix.eval("let x = { inherit x; }; in x")
        except nix.NixError as e:
            self.assertTrue(True)

    def test_null_expression(self):
        # Null characters should be allowed in expressions, even if they aren't
        # very useful really, though at least null's should be supported in
        # strings in the future https://github.com/NixOS/nix/issues/1307)
        self.assertEqual(nix.eval("\"ab\x00cd\""), "ab")

    def test_throw_null(self):
        try:
            nix.eval("throw \"hello\x00there\"")
        except nix.ThrownNixError as e:
            self.assertEqual(e.args[0], "hello")

    def test_booleans(self):
        self.assertIs(nix.eval("assert a == true; a", vars=dict(a=True)), True)
        self.assertIs(nix.eval("assert a == false; a", vars=dict(a=False)), False)

if __name__ == '__main__':
    unittest.main()
