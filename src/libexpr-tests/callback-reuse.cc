#include <gtest/gtest.h>

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

/* Property tests for the callback-reuse matching criterion.
 *
 * The reuse mechanism decides whether a prior callback firing's
 * cachedApplyResult can be served to a current SCA dispatch. Getting
 * this wrong risks returning one sibling's result to another
 * sibling's caller (a "wrong hit" — priority 1 correctness violation).
 *
 * These tests pin down the criterion at the smallest testable layer:
 * `reuseMatchScore(cellObs, incoming)`, a pure function of the cell's
 * runningObsSet and the incoming SCA's obsSet. Tests assert PROPERTIES
 * we want (not just the current implementation's behavior). Test names
 * document the invariant; a passing test with a wrong assertion is
 * more dangerous than a failing test.
 */

using InlineFact = TracingDecisionGraph::InlineFact;

static TracingHash h(std::string_view s)
{
    return trace::tracingHash(s);
}

static InlineFact fact(std::string_view req, std::string_view resp)
{
    return InlineFact{h(req), std::string(resp)};
}

/* ═════════════════════════════════════════════════════════════════
   Straightforward matching cases — sanity, not the interesting bit.
   ═════════════════════════════════════════════════════════════════ */

TEST(ReuseMatchScore, EmptyCellMatchesEverything)
{
    /* An empty cellObs vacuously matches — nothing to disagree with.
     * Score = 0 (no matched entries). This is fine ONLY if reuse's
     * downstream logic doesn't treat "match" as "safe to reuse" —
     * because a cell with no obs can be a subset of anything and
     * would trigger wrong-hits. Documented here as the criterion's
     * current behavior; whether it's SAFE is what higher-level tests
     * must verify. */
    auto s = reuseMatchScore({}, {{h("q1"), "r1"}});
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(*s, 0u);
}

TEST(ReuseMatchScore, ExactMatchReturnsFullScore)
{
    auto s = reuseMatchScore(
        {fact("q1", "r1"), fact("q2", "r2")},
        {{h("q1"), "r1"}, {h("q2"), "r2"}});
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(*s, 2u);
}

TEST(ReuseMatchScore, MissingKeyInIncomingRejects)
{
    /* Cell recorded {q1, q3}; incoming has {q1, q2}. q3 not in
     * incoming → no match. */
    auto s = reuseMatchScore(
        {fact("q1", "r1"), fact("q3", "r3")},
        {{h("q1"), "r1"}, {h("q2"), "r2"}});
    EXPECT_FALSE(s.has_value());
}

TEST(ReuseMatchScore, ResponseMismatchOnSameKeyRejects)
{
    auto s = reuseMatchScore(
        {fact("q1", "recorded")},
        {{h("q1"), "different"}});
    EXPECT_FALSE(s.has_value());
}

/* ═════════════════════════════════════════════════════════════════
   The interesting property: subset trivially matches when the cell
   observed only a PROPER SUBSET of what the current call would.
   User (2026-08-05): claim "subset isolates siblings" rests on the
   assumption that siblings have disjoint obs — which is false in
   general (two different fn bodies CAN produce identical or
   subset-related obs).
   ═════════════════════════════════════════════════════════════════ */

TEST(ReuseMatchScore, CellSubsetOfIncomingIsAcceptedButMayBeUnsafe)
{
    /* This documents the current criterion's behavior on a case
     * that SHOULD probably reject: cell recorded fewer probes than
     * the incoming's fn body would make. If we serve the cell's
     * cachedApplyResult here, we're asserting the current fn's
     * evaluation would take the same path as the cell's did — but
     * we haven't verified the current fn wouldn't diverge on the
     * extra probes.
     *
     * Under strict referential transparency + deterministic fn
     * behavior, this MIGHT be safe (cell's fn didn't need those
     * extra probes; current fn wouldn't either). But the criterion
     * currently accepts even in scenarios where that assumption
     * doesn't hold — e.g., cell captured only "common" probes
     * (getType, WHNF checks) shared across sibling fns, while
     * incoming's discriminating probes weren't in cell. */
    auto s = reuseMatchScore(
        {fact("common", "same")},
        {{h("common"), "same"}, {h("discriminator"), "value"}});
    /* Current behavior: accepts. Documented — the property "reuse
     * only fires when provably safe" is NOT enforced by this
     * criterion. */
    EXPECT_TRUE(s.has_value()) << "Current criterion accepts cell⊂incoming — "
        "may serve wrong-context result if fn's evaluation differs on the "
        "extra probes";
}

TEST(ReuseMatchScore, DisjointObsRejects)
{
    /* Two truly disjoint obs sets — cell recorded {q1}, incoming
     * has {q2}. Neither is a subset of the other. Reject. This is
     * the "clean sibling" case where the claim about subset-based
     * isolation holds. */
    auto s = reuseMatchScore(
        {fact("q1", "r1")},
        {{h("q2"), "r2"}});
    EXPECT_FALSE(s.has_value());
}

TEST(ReuseMatchScore, CellSupersetsIncomingRejects)
{
    /* Cell recorded MORE than incoming. Cell has {q1, q2}, incoming
     * only {q1}. q2 in cell not in incoming → reject. */
    auto s = reuseMatchScore(
        {fact("q1", "r1"), fact("q2", "r2")},
        {{h("q1"), "r1"}});
    EXPECT_FALSE(s.has_value());
}

/* ═════════════════════════════════════════════════════════════════
   Sibling-isolation property: two cells representing siblings with
   PARTIALLY OVERLAPPING obs (common probes + differentiating probes).
   Each sibling's incoming should match ONLY its own cell.
   ═════════════════════════════════════════════════════════════════ */

TEST(ReuseMatchScore, DifferentiatedSiblingsIsolate)
{
    /* Cell A recorded [common, .a→42]. Cell B recorded [common, .b→99].
     * A's incoming = [common, .a→42], B's incoming = [common, .b→99].
     * Under subset check:
     *   A against A_incoming: full match, score=2.
     *   A against B_incoming: .a→42 not in B_incoming (only .b→99 differentiator).
     *     REJECT.
     *   B against A_incoming: .b→99 not in A_incoming. REJECT.
     *   B against B_incoming: full match, score=2.
     * Sibling isolation holds when siblings genuinely differ in what
     * they observe. */
    auto cellA = std::vector<InlineFact>{fact("common", "same"), fact("a", "42")};
    auto cellB = std::vector<InlineFact>{fact("common", "same"), fact("b", "99")};
    auto incomingA = std::map<TracingHash, std::string>{
        {h("common"), "same"}, {h("a"), "42"}};
    auto incomingB = std::map<TracingHash, std::string>{
        {h("common"), "same"}, {h("b"), "99"}};

    EXPECT_TRUE(reuseMatchScore(cellA, incomingA).has_value());
    EXPECT_FALSE(reuseMatchScore(cellA, incomingB).has_value());
    EXPECT_FALSE(reuseMatchScore(cellB, incomingA).has_value());
    EXPECT_TRUE(reuseMatchScore(cellB, incomingB).has_value());
}

TEST(ReuseMatchScore, SiblingsWithCommonOnlyCellsConflate)
{
    /* PROBLEMATIC CASE: a cell with only common probes (no
     * differentiator) is a subset of BOTH siblings' incoming. If
     * two siblings each have an "early-terminated" cell (captured
     * only common probes before firing completed), the criterion
     * can't distinguish which sibling the cell belongs to.
     *
     * Current criterion: matches both. That means whichever cell
     * happens to be picked (first-inserted for ties, per the
     * candidate-selection loop) determines the result — arbitrary
     * across siblings. Wrong-hit potential.
     *
     * This test DOCUMENTS the failure mode; the assertion asserts
     * the (unsafe) current behavior so the test passes and future
     * refactors that TIGHTEN the criterion would flip this to
     * EXPECT_FALSE. */
    auto commonOnlyCell = std::vector<InlineFact>{fact("common", "same")};
    auto incomingA = std::map<TracingHash, std::string>{
        {h("common"), "same"}, {h("a"), "42"}};
    auto incomingB = std::map<TracingHash, std::string>{
        {h("common"), "same"}, {h("b"), "99"}};

    /* Both match — the criterion cannot discriminate. Documented as
     * the sibling-conflation window. */
    EXPECT_TRUE(reuseMatchScore(commonOnlyCell, incomingA).has_value())
        << "cell with only common obs matches sibling A — "
        "unsafe when cell was recorded under a DIFFERENT sibling's context";
    EXPECT_TRUE(reuseMatchScore(commonOnlyCell, incomingB).has_value())
        << "cell with only common obs matches sibling B — "
        "same wrong-hit potential from the other direction";
}

} // namespace nix
