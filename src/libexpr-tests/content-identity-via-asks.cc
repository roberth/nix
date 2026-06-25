#include <gtest/gtest.h>

#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix::cidasks {

static Subject seed(int depth)
{
    return Subject{PositionalSeed{depth}};
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

TEST(CidAsks, PositionalSeedInitialIdIsDeterministic)
{
    auto a = contentIdAfter(seed(0), noScope(), {});
    auto b = contentIdAfter(seed(0), noScope(), {});
    EXPECT_EQ(a, b);
}

TEST(CidAsks, DifferentDepthsHaveDifferentInitialIds)
{
    auto a = contentIdAfter(seed(0), noScope(), {});
    auto b = contentIdAfter(seed(1), noScope(), {});
    EXPECT_NE(a, b);
}

TEST(CidAsks, DerivedSubjectIncludesParentInitial)
{
    /* Derived subjects don't have CDIs — only structural addresses
       (= producer query hashes). Same property holds: different
       names / different parents → different addresses. */
    auto x = getAttrOn(seed(0), "x");
    auto y = getAttrOn(seed(0), "y");
    auto xOn1 = getAttrOn(seed(1), "x");
    EXPECT_NE(structuralAddressAfter(x, noScope(), {}), structuralAddressAfter(y, noScope(), {}));
    EXPECT_NE(structuralAddressAfter(x, noScope(), {}), structuralAddressAfter(xOn1, noScope(), {}));
}

TEST(CidAsks, ApplyResultDistinguishesFnAndArg)
{
    auto fn0 = seed(0);
    auto fn1 = seed(1);
    auto arg = seed(2);
    EXPECT_NE(contentIdAfter(applyResult(fn0, arg), noScope(), {}), contentIdAfter(applyResult(fn1, arg), noScope(), {}));
}

/* ---- observation-driven evolution ---- */

TEST(CidAsks, ObservationOnSeedAdvancesContentId)
{
    auto s = seed(0);
    auto initial = contentIdAfter(s, noScope(), {});

    // A getInt fact whose from matches the seed's initial id.
    trace::QueryGetInt q{hex(initial)};
    trace::ResultInt r{42};
    Edge e{.observations = {observationFromQR(q, r)}};

    auto after = contentIdAfter(s, noScope(), {e});
    EXPECT_NE(initial, after);

    // The advance is exactly elementHash XORed in.
    auto fact = observationFromQR(q, r);
    auto expected = TracingDecisionGraph::xorHashes(initial, fact.elementHash);
    EXPECT_EQ(after, expected);
}

TEST(CidAsks, FactOnUnrelatedSubjectDoesNotAdvance)
{
    auto s0 = seed(0);
    auto s1 = seed(1);
    auto s1Initial = contentIdAfter(s1, noScope(), {});

    // Fact whose from matches s1, not s0.
    trace::QueryGetInt q{hex(s1Initial)};
    trace::ResultInt r{99};
    Edge e{.observations = {observationFromQR(q, r)}};

    EXPECT_EQ(contentIdAfter(s0, noScope(), {}), contentIdAfter(s0, noScope(), {e}));
    EXPECT_NE(contentIdAfter(s1, noScope(), {}), contentIdAfter(s1, noScope(), {e}));
}

TEST(CidAsks, SameShapeCollapse)
{
    // Two seeds at the same depth — same initial id (= same-shape collapse).
    // They cannot be distinguished without their own observations.
    auto a = seed(0);
    auto b = seed(0);
    EXPECT_EQ(contentIdAfter(a, noScope(), {}), contentIdAfter(b, noScope(), {}));
}

TEST(CidAsks, XorCommutativityWithinEdge)
{
    auto s = seed(0);
    auto initial = contentIdAfter(s, noScope(), {});

    trace::QueryGetInt q1{hex(initial)};
    trace::ResultInt r1{1};
    trace::QueryGetType q2{hex(initial)};
    trace::ResultType r2{"int"};

    auto f1 = observationFromQR(q1, r1);
    auto f2 = observationFromQR(q2, r2);
    Edge eAB{.observations = {f1, f2}};
    Edge eBA{.observations = {f2, f1}};

    // Within one edge, dispatch order doesn't matter.
    EXPECT_EQ(contentIdAfter(s, noScope(), {eAB}), contentIdAfter(s, noScope(), {eBA}));
}

/* ---- derived evolution: parent advances → derived advances ---- */

TEST(CidAsks, DerivedAdvancesWhenParentAdvances)
{
    auto parent = seed(0);
    auto child = getAttrOn(parent, "x");

    auto parentInitial = contentIdAfter(parent, noScope(), {});
    auto childInitial = structuralAddressAfter(child, noScope(), {});

    // A fact on the parent.
    trace::QueryGetType q{hex(parentInitial)};
    trace::ResultType r{"set"};
    Edge e{.observations = {observationFromQR(q, r)}};

    auto childAfter = structuralAddressAfter(child, noScope(), {e});
    EXPECT_NE(childInitial, childAfter);  // address changes because parent's CDI did
}

TEST(CidAsks, DerivedDoesNotAdvanceOnFactsTargetedAtItself)
{
    /* Per-arg centralization: facts about derived values get
       stamped at `from = root_cdi`, never `from = derived_address`.
       A hypothetical fact with from=derived_address therefore does
       NOT advance derived's address — only facts on the root do
       (via the root's CDI evolving), which the prior test covers. */
    auto parent = seed(0);
    auto child = getAttrOn(parent, "x");

    auto childInitial = structuralAddressAfter(child, noScope(), {});

    // A fact whose `from` matches the child's address (not the root's).
    trace::QueryGetInt q{hex(childInitial)};
    trace::ResultInt r{7};
    Edge e{.observations = {observationFromQR(q, r)}};

    EXPECT_EQ(structuralAddressAfter(child, noScope(), {e}), childInitial);
}

/* ---- inheritance: outer-scope CDIs make sibling content ids distinct ---- */

static Hash scopeFor(const std::string & q)
{
    return hashString(HashAlgorithm::SHA256, "Q:" + q);
}

TEST(CidAsks, InheritanceDistinguishesPositionalSeedsAcrossScopes)
{
    auto s = seed(0);
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(contentIdAfter(s, scopeA, {}), contentIdAfter(s, scopeB, {}));
}

TEST(CidAsks, InheritanceDistinguishesDerivedAcrossScopes)
{
    auto child = getAttrOn(seed(0), "x");
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(structuralAddressAfter(child, scopeA, {}), structuralAddressAfter(child, scopeB, {}));
}

TEST(CidAsks, InheritancePropagatesIntoDerivedQueryPayload)
{
    /* The child's content id derives from the parent's *scoped*
       content id used in the query payload. So child's id differs
       between scopes even though the structural derivation
       (getAttr "x") is identical. */
    auto parent = seed(0);
    auto child = getAttrOn(parent, "x");
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    auto parentInA = contentIdAfter(parent, scopeA, {});
    auto parentInB = contentIdAfter(parent, scopeB, {});
    EXPECT_NE(parentInA, parentInB);

    auto childInA = structuralAddressAfter(child, scopeA, {});
    auto childInB = structuralAddressAfter(child, scopeB, {});
    EXPECT_NE(childInA, childInB);
}

TEST(CidAsks, InheritanceWithEmptyScopeMatchesUnscoped)
{
    /* noScope() is the zero Hash; XORing with zero is a no-op, so
       scope=noScope() must give identical results to "no scope" usage
       in the legacy tests. */
    auto s = seed(0);
    auto child = getAttrOn(s, "x");

    EXPECT_EQ(contentIdAfter(s, noScope(), {}), contentIdAfter(s, Hash(HashAlgorithm::SHA256), {}));
    EXPECT_EQ(structuralAddressAfter(child, noScope(), {}), structuralAddressAfter(child, Hash(HashAlgorithm::SHA256), {}));
}

TEST(CidAsks, InheritanceDistinguishesApplyResultAcrossScopes)
{
    auto fn = seed(0);
    auto arg = seed(1);
    auto result = applyResult(fn, arg);
    auto scopeA = scopeFor("A");
    auto scopeB = scopeFor("B");

    EXPECT_NE(contentIdAfter(result, scopeA, {}), contentIdAfter(result, scopeB, {}));
}

TEST(CidAsks, ObservationOnScopedSeedRequiresMatchingScopedFromHash)
{
    /* Sanity check that the recording/walker symmetry under
       inheritance: a fact whose `from` matches the *scoped* content id
       contributes; one whose `from` matches the unscoped id doesn't. */
    auto s = seed(0);
    auto scope = scopeFor("Q1");

    auto scopedInitial = contentIdAfter(s, scope, {});
    auto unscopedInitial = contentIdAfter(s, noScope(), {});
    EXPECT_NE(scopedInitial, unscopedInitial);

    trace::QueryGetInt qScoped{hex(scopedInitial)};
    trace::QueryGetInt qUnscoped{hex(unscopedInitial)};
    trace::ResultInt r{1};
    Edge eScoped{.observations = {observationFromQR(qScoped, r)}};
    Edge eUnscoped{.observations = {observationFromQR(qUnscoped, r)}};

    EXPECT_NE(contentIdAfter(s, scope, {eScoped}), scopedInitial);
    EXPECT_EQ(contentIdAfter(s, scope, {eUnscoped}), scopedInitial);
}

} // namespace nix::cidasks
