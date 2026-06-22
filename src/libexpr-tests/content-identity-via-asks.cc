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

/* ---- structural-id sanity ---- */

TEST(CidAsks, PositionalSeedInitialIdIsDeterministic)
{
    auto a = contentIdAfter(seed(0), {});
    auto b = contentIdAfter(seed(0), {});
    EXPECT_EQ(a, b);
}

TEST(CidAsks, DifferentDepthsHaveDifferentInitialIds)
{
    auto a = contentIdAfter(seed(0), {});
    auto b = contentIdAfter(seed(1), {});
    EXPECT_NE(a, b);
}

TEST(CidAsks, DerivedSubjectIncludesParentInitial)
{
    auto x = getAttrOn(seed(0), "x");
    auto y = getAttrOn(seed(0), "y");
    auto xOn1 = getAttrOn(seed(1), "x");
    EXPECT_NE(contentIdAfter(x, {}), contentIdAfter(y, {}));
    EXPECT_NE(contentIdAfter(x, {}), contentIdAfter(xOn1, {}));
}

TEST(CidAsks, ApplyResultDistinguishesFnAndArg)
{
    auto fn0 = seed(0);
    auto fn1 = seed(1);
    auto arg = seed(2);
    EXPECT_NE(contentIdAfter(applyResult(fn0, arg), {}), contentIdAfter(applyResult(fn1, arg), {}));
}

/* ---- observation-driven evolution ---- */

TEST(CidAsks, ObservationOnSeedAdvancesContentId)
{
    auto s = seed(0);
    auto initial = contentIdAfter(s, {});

    // A getInt fact whose from matches the seed's initial id.
    trace::QueryGetInt q{hex(initial)};
    trace::ResultInt r{42};
    Edge e{.facts = {factFromQR(q, r)}};

    auto after = contentIdAfter(s, {e});
    EXPECT_NE(initial, after);

    // The advance is exactly elementHash XORed in.
    auto fact = factFromQR(q, r);
    auto expected = TracingDecisionGraph::xorHashes(initial, fact.elementHash);
    EXPECT_EQ(after, expected);
}

TEST(CidAsks, FactOnUnrelatedSubjectDoesNotAdvance)
{
    auto s0 = seed(0);
    auto s1 = seed(1);
    auto s1Initial = contentIdAfter(s1, {});

    // Fact whose from matches s1, not s0.
    trace::QueryGetInt q{hex(s1Initial)};
    trace::ResultInt r{99};
    Edge e{.facts = {factFromQR(q, r)}};

    EXPECT_EQ(contentIdAfter(s0, {}), contentIdAfter(s0, {e}));
    EXPECT_NE(contentIdAfter(s1, {}), contentIdAfter(s1, {e}));
}

TEST(CidAsks, SameShapeCollapse)
{
    // Two seeds at the same depth — same initial id (= same-shape collapse).
    // They cannot be distinguished without their own observations.
    auto a = seed(0);
    auto b = seed(0);
    EXPECT_EQ(contentIdAfter(a, {}), contentIdAfter(b, {}));
}

TEST(CidAsks, XorCommutativityWithinEdge)
{
    auto s = seed(0);
    auto initial = contentIdAfter(s, {});

    trace::QueryGetInt q1{hex(initial)};
    trace::ResultInt r1{1};
    trace::QueryGetType q2{hex(initial)};
    trace::ResultType r2{"int"};

    auto f1 = factFromQR(q1, r1);
    auto f2 = factFromQR(q2, r2);
    Edge eAB{.facts = {f1, f2}};
    Edge eBA{.facts = {f2, f1}};

    // Within one edge, dispatch order doesn't matter.
    EXPECT_EQ(contentIdAfter(s, {eAB}), contentIdAfter(s, {eBA}));
}

/* ---- derived evolution: parent advances → derived advances ---- */

TEST(CidAsks, DerivedAdvancesWhenParentAdvances)
{
    auto parent = seed(0);
    auto child = getAttrOn(parent, "x");

    auto parentInitial = contentIdAfter(parent, {});
    auto childInitial = contentIdAfter(child, {});

    // A fact on the parent.
    trace::QueryGetType q{hex(parentInitial)};
    trace::ResultType r{"set"};
    Edge e{.facts = {factFromQR(q, r)}};

    auto childAfter = contentIdAfter(child, {e});
    EXPECT_NE(childInitial, childAfter);  // child's id changed because parent's did
}

TEST(CidAsks, DerivedAlsoAdvancesOnOwnObservations)
{
    auto parent = seed(0);
    auto child = getAttrOn(parent, "x");

    auto childInitial = contentIdAfter(child, {});

    // A fact directly on the child.
    trace::QueryGetInt q{hex(childInitial)};
    trace::ResultInt r{7};
    Edge e{.facts = {factFromQR(q, r)}};

    EXPECT_NE(contentIdAfter(child, {e}), childInitial);
}

} // namespace nix::cidasks
