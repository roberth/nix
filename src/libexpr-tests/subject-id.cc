#include <gtest/gtest.h>

#include "nix/expr/subject-id.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix::cidasks {

static Subject argAt(int depth)
{
    return Subject{Arg{depth}};
}

static Subject getAttrOn(const Subject & parent, std::string name)
{
    return Subject{
        DerivedSubject{
            .parent = std::make_shared<const Subject>(parent),
            .kind = DerivedSubject::Kind::GetAttr,
            .name = std::move(name),
        }};
}

static Subject applyResult(const Subject & fn, const Subject & arg)
{
    return Subject{
        ApplyResultSubject{
            .fn = std::make_shared<const Subject>(fn),
            .arg = std::make_shared<const Subject>(arg),
        }};
}

static std::string hex(const Hash & h)
{
    return h.to_string(HashFormat::Base16, false);
}

/* Empty scope = no inheritance from outer scopes; equivalent to the
   pre-inheritance behavior. Used for the legacy tests that exercise
   structural semantics independent of scope. */
static Hash noScope()
{
    return Hash(HashAlgorithm::SHA256);
}

/* ---- structural-id sanity ---- */

TEST(CidAsks, ArgInitialSubjectHashIsDeterministic)
{
    auto a = stateHashAfter(argAt(0), noScope(), {});
    auto b = stateHashAfter(argAt(0), noScope(), {});
    EXPECT_EQ(a, b);
}

TEST(CidAsks, DifferentDepthsHaveDifferentInitialIds)
{
    auto a = stateHashAfter(argAt(0), noScope(), {});
    auto b = stateHashAfter(argAt(1), noScope(), {});
    EXPECT_NE(a, b);
}

TEST(CidAsks, DerivedSubjectIncludesParentInitial)
{
    /* Derived subjects don't have state hashes — only structural addresses
       (= producer query hashes). Same property holds: different
       names / different parents → different addresses. */
    auto x = getAttrOn(argAt(0), "x");
    auto y = getAttrOn(argAt(0), "y");
    auto xOn1 = getAttrOn(argAt(1), "x");
    EXPECT_NE(stateHashAfterSubject(x, noScope(), {}), stateHashAfterSubject(y, noScope(), {}));
    EXPECT_NE(stateHashAfterSubject(x, noScope(), {}), stateHashAfterSubject(xOn1, noScope(), {}));
}

TEST(CidAsks, ApplyResultDistinguishesFnAndArg)
{
    auto fn0 = argAt(0);
    auto fn1 = argAt(1);
    auto arg = argAt(2);
    EXPECT_NE(stateHashAfter(applyResult(fn0, arg), noScope(), {}), stateHashAfter(applyResult(fn1, arg), noScope(), {}));
}

/* ---- observation-driven evolution ---- */

TEST(CidAsks, ObservationOnSeedAdvancesContentId)
{
    auto s = argAt(0);
    auto initial = stateHashAfter(s, noScope(), {});

    // A getInt fact whose from matches the arg's initial id.
    trace::SelectorGetWHNF q{hex(initial)};
    trace::ResultWHNF r{"int", trace::WHNFInt{42}};
    ObservationSet e{.observations = {observationFromQR(q, r)}};

    auto after = stateHashAfter(s, noScope(), {e});
    EXPECT_NE(initial, after);

    // The advance is exactly elementHash XORed in.
    auto fact = observationFromQR(q, r);
    auto expected = TracingDecisionGraph::xorHashes(initial, fact.elementHash);
    EXPECT_EQ(after, expected);
}

TEST(CidAsks, FactOnUnrelatedSubjectDoesNotAdvance)
{
    auto s0 = argAt(0);
    auto s1 = argAt(1);
    auto s1Initial = stateHashAfter(s1, noScope(), {});

    // Fact whose from matches s1, not s0.
    trace::SelectorGetWHNF q{hex(s1Initial)};
    trace::ResultWHNF r{"int", trace::WHNFInt{99}};
    ObservationSet e{.observations = {observationFromQR(q, r)}};

    EXPECT_EQ(stateHashAfter(s0, noScope(), {}), stateHashAfter(s0, noScope(), {e}));
    EXPECT_NE(stateHashAfter(s1, noScope(), {}), stateHashAfter(s1, noScope(), {e}));
}

TEST(CidAsks, SameShapeCollapse)
{
    // Two seeds at the same depth — same initial id (= same-shape collapse).
    // They cannot be distinguished without their own observations.
    auto a = argAt(0);
    auto b = argAt(0);
    EXPECT_EQ(stateHashAfter(a, noScope(), {}), stateHashAfter(b, noScope(), {}));
}

TEST(CidAsks, XorCommutativityWithinEdge)
{
    auto s = argAt(0);
    auto initial = stateHashAfter(s, noScope(), {});

    trace::SelectorGetWHNF q1{hex(initial)};
    trace::ResultWHNF r1{"int", trace::WHNFInt{1}};
    trace::SelectorGetAttr q2{"foo", hex(initial)};
    trace::ResultWHNF r2{"int", trace::WHNFInt{2}};

    auto f1 = observationFromQR(q1, r1);
    auto f2 = observationFromQR(q2, r2);
    ObservationSet eAB{.observations = {f1, f2}};
    ObservationSet eBA{.observations = {f2, f1}};

    // Within one edge, dispatch order doesn't matter.
    EXPECT_EQ(stateHashAfter(s, noScope(), {eAB}), stateHashAfter(s, noScope(), {eBA}));
}

/* ---- derived evolution: parent advances → derived advances ---- */

TEST(CidAsks, DerivedAdvancesWhenParentAdvances)
{
    auto parent = argAt(0);
    auto child = getAttrOn(parent, "x");

    auto parentInitial = stateHashAfter(parent, noScope(), {});
    auto childInitial = stateHashAfterSubject(child, noScope(), {});

    // A fact on the parent.
    trace::SelectorGetWHNF q{hex(parentInitial)};
    trace::ResultWHNF r{"set", trace::WHNFAttrs{{"x"}}};
    ObservationSet e{.observations = {observationFromQR(q, r)}};

    auto childAfter = stateHashAfterSubject(child, noScope(), {e});
    EXPECT_NE(childInitial, childAfter);  // address changes because parent's state hash did
}

TEST(CidAsks, DerivedDoesNotAdvanceOnFactsTargetedAtItself)
{
    /* Per-arg centralization: facts about derived values get
       stamped at `from = root_cdi`, never `from = derived_address`.
       A hypothetical fact with from=derived_address therefore does
       NOT advance derived's address — only facts on the root do
       (via the root's state hash evolving), which the prior test covers. */
    auto parent = argAt(0);
    auto child = getAttrOn(parent, "x");

    auto childInitial = stateHashAfterSubject(child, noScope(), {});

    // A fact whose `from` matches the child's address (not the root's).
    trace::SelectorGetWHNF q{hex(childInitial)};
    trace::ResultWHNF r{"int", trace::WHNFInt{7}};
    ObservationSet e{.observations = {observationFromQR(q, r)}};

    EXPECT_EQ(stateHashAfterSubject(child, noScope(), {e}), childInitial);
}

/* ---- inheritance: outer-scope state hashes make sibling content ids distinct ---- */

static Hash scopeFor(const std::string & q)
{
    return hashString(HashAlgorithm::SHA256, "Q:" + q);
}

TEST(CidAsks, InheritanceDistinguishesArgsAcrossArgAncestries)
{
    auto s = argAt(0);
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(stateHashAfter(s, scopeA, {}), stateHashAfter(s, scopeB, {}));
}

TEST(CidAsks, InheritanceDistinguishesDerivedAcrossScopes)
{
    auto child = getAttrOn(argAt(0), "x");
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(stateHashAfterSubject(child, scopeA, {}), stateHashAfterSubject(child, scopeB, {}));
}

TEST(CidAsks, InheritancePropagatesIntoDerivedQueryPayload)
{
    /* The child's content id derives from the parent's *scoped*
       content id used in the query payload. So child's id differs
       between scopes even though the structural derivation
       (getAttr "x") is identical. */
    auto parent = argAt(0);
    auto child = getAttrOn(parent, "x");
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    auto parentInA = stateHashAfter(parent, scopeA, {});
    auto parentInB = stateHashAfter(parent, scopeB, {});
    EXPECT_NE(parentInA, parentInB);

    auto childInA = stateHashAfterSubject(child, scopeA, {});
    auto childInB = stateHashAfterSubject(child, scopeB, {});
    EXPECT_NE(childInA, childInB);
}

TEST(CidAsks, InheritanceWithEmptyScopeMatchesUnscoped)
{
    /* noScope() is the zero Hash; XORing with zero is a no-op, so
       scope=noScope() must give identical results to "no scope" usage
       in the legacy tests. */
    auto s = argAt(0);
    auto child = getAttrOn(s, "x");

    EXPECT_EQ(stateHashAfter(s, noScope(), {}), stateHashAfter(s, Hash(HashAlgorithm::SHA256), {}));
    EXPECT_EQ(stateHashAfterSubject(child, noScope(), {}), stateHashAfterSubject(child, Hash(HashAlgorithm::SHA256), {}));
}

TEST(CidAsks, InheritanceDistinguishesApplyResultAcrossScopes)
{
    auto fn = argAt(0);
    auto arg = argAt(1);
    auto result = applyResult(fn, arg);
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(stateHashAfter(result, scopeA, {}), stateHashAfter(result, scopeB, {}));
}

TEST(CidAsks, ObservationOnScopedSeedRequiresMatchingScopedFromHash)
{
    /* Sanity check that the recording/walker symmetry under
       inheritance: a fact whose `from` matches the *scoped* content id
       contributes; one whose `from` matches the unscoped id doesn't. */
    auto s = argAt(0);
    auto scope = scopeFor("Q1");

    auto scopedInitial = stateHashAfter(s, scope, {});
    auto unscopedInitial = stateHashAfter(s, noScope(), {});
    EXPECT_NE(scopedInitial, unscopedInitial);

    trace::SelectorGetWHNF qScoped{hex(scopedInitial)};
    trace::SelectorGetWHNF qUnscoped{hex(unscopedInitial)};
    trace::ResultWHNF r{"int", trace::WHNFInt{1}};
    ObservationSet eScoped{.observations = {observationFromQR(qScoped, r)}};
    ObservationSet eUnscoped{.observations = {observationFromQR(qUnscoped, r)}};

    EXPECT_NE(stateHashAfter(s, scope, {eScoped}), scopedInitial);
    EXPECT_EQ(stateHashAfter(s, scope, {eUnscoped}), scopedInitial);
}

} // namespace nix::cidasks
