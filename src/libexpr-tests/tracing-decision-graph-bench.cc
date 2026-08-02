#include <benchmark/benchmark.h>

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"

namespace nix {
namespace {

/* A fresh on-disk SQLite-backed graph per benchmark to isolate
   storage-cost measurements from process-wide state. */
struct GraphFixture
{
    std::filesystem::path tempDir;
    std::unique_ptr<TracingDecisionGraph> g;

    GraphFixture()
        : tempDir(createTempDir())
        , g(std::make_unique<TracingDecisionGraph>(tempDir / "index.sqlite"))
    {
    }

    ~GraphFixture()
    {
        g.reset();
        std::filesystem::remove_all(tempDir);
    }
};

static Hash sha(std::string_view s)
{
    return trace::tracingHash(s);
}

/* Build a fresh FactSet with `nFacts` distinct (Request, Response)
   pairs derived from a arg string. */
static TracingDecisionGraph::SetHash makeFactSet(
    TracingDecisionGraph & g, std::string_view arg, size_t nFacts)
{
    std::vector<TracingDecisionGraph::Fact> facts;
    facts.reserve(nFacts);
    for (size_t i = 0; i < nFacts; ++i) {
        auto req = sha(fmt("%s-req-%d", arg, i));
        auto resp = sha(fmt("%s-resp-%d", arg, i));
        facts.push_back({req, resp});
    }
    return g.insertFactSet(std::move(facts));
}

} // namespace

/* ─────────────────────────────────────────────────────────────────────
   Storage layer: set extension cost
   ───────────────────────────────────────────────────────────────────── */

static void BM_InsertFactSet(benchmark::State & state)
{
    const auto nFacts = static_cast<size_t>(state.range(0));
    GraphFixture fx;

    for (auto _ : state) {
        std::vector<TracingDecisionGraph::Fact> facts;
        facts.reserve(nFacts);
        for (size_t i = 0; i < nFacts; ++i) {
            /* Vary the arg each iteration to avoid measuring INSERT-OR-IGNORE
               dedup of a single set; we want to measure inserting *novel*
               sets of the given size. */
            auto req = sha(fmt("bench-%d-req-%d", state.iterations(), i));
            auto resp = sha(fmt("bench-%d-resp-%d", state.iterations(), i));
            facts.push_back({req, resp});
        }
        auto h = fx.g->insertFactSet(std::move(facts));
        benchmark::DoNotOptimize(h);
    }

    state.SetItemsProcessed(state.iterations() * nFacts);
}
BENCHMARK(BM_InsertFactSet)->Arg(10)->Arg(100)->Arg(1000);

/* ─────────────────────────────────────────────────────────────────────
   record(): integrating a recording of varying depth
   ───────────────────────────────────────────────────────────────────── */

static void BM_RecordFreshFactSet(benchmark::State & state)
{
    const auto nFacts = static_cast<size_t>(state.range(0));
    GraphFixture fx;

    /* Pre-allocate one Q so we measure record's work, not Query insert. */
    auto q = sha("bench-Q");

    /* Each iteration uses a fresh arg → distinct FactSet → record
       inserts |FactSet| new Asks edges plus Terminals. */
    size_t iter = 0;
    for (auto _ : state) {
        auto fs = makeFactSet(*fx.g, fmt("rec-bench-%d", iter), nFacts);
        auto result = sha(fmt("result-%d", iter));
        fx.g->record(q, fs, result);
        ++iter;
    }

    state.SetItemsProcessed(state.iterations() * nFacts);
}
BENCHMARK(BM_RecordFreshFactSet)->Arg(10)->Arg(100);

/* ─────────────────────────────────────────────────────────────────────
   walk(): replay cost on a single recorded path
   ───────────────────────────────────────────────────────────────────── */

static void BM_WalkExistingRecording(benchmark::State & state)
{
    const auto nFacts = static_cast<size_t>(state.range(0));
    GraphFixture fx;

    auto q = sha("walk-bench-Q");
    auto fs = makeFactSet(*fx.g, "walk-bench", nFacts);
    auto result = sha("walk-bench-R");
    fx.g->record(q, fs, result);

    /* Pre-materialise the (request, response) lookup table. Use
       std::map because nix::Hash has operator<=> but isn't default-
       constructible (so unordered_map's value-type machinery
       doesn't work). */
    std::map<Hash, Hash> table;
    for (size_t i = 0; i < nFacts; ++i) {
        auto req = sha(fmt("walk-bench-req-%d", i));
        auto resp = sha(fmt("walk-bench-resp-%d", i));
        table.emplace(req, resp);
    }

    for (auto _ : state) {
        auto hit = fx.g->walk(q, [&](const Hash & req) {
            return table.at(req);
        });
        benchmark::DoNotOptimize(hit);
    }

    state.SetItemsProcessed(state.iterations() * nFacts);
}
BENCHMARK(BM_WalkExistingRecording)->Arg(10)->Arg(100);

} // namespace nix
