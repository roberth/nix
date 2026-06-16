#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/primops.hh"

/*
 * builtins.cache: stripped during the v12 -> v13 migration. The v12
 * trie that this used as a persistence backend is gone; the v13
 * decision graph doesn't yet model the d=2 ambient layer that
 * `builtins.cache`'s explicit-scope semantics need. Left as a stub
 * that errors loudly until d=2 support lands in v13.
 */

namespace nix {

static void prim_cache(EvalState & state, const PosIdx pos, Value ** /*args*/, Value & /*v*/)
{
    state.error<EvalError>("builtins.cache: not implemented in v13 (d=2 ambient layer pending)")
        .atPos(pos)
        .debugThrow();
}

static RegisterPrimOp primop_cache({
    .name = "__cache",
    .args = {"attrs"},
    .doc = R"(
      Disabled during the v12 → v13 eval-cache migration. Throws on call.
    )",
    .impl = prim_cache,
    .experimentalFeature = Xp::TracingEvalCache,
});

} // namespace nix
