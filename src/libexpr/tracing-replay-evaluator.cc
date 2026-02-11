#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-database.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <cstdlib>

namespace nix {

bool TracingReplayEvaluator::validateEnvTo(size_t targetPos)
{
    while (cursor < targetPos && cursor < trace.size()) {
        bool valid = std::visit(
            overloaded{
                [&](const trace::Response<trace::FileReadRequest> & r) {
                    // TODO: validate file content hash matches
                    // For now, assume valid
                    debug("replay: validating file read %s", r.request.absPath);
                    return true;
                },
                [&](const trace::Response<trace::GetEnvRequest> & r) {
                    const char * current = std::getenv(r.request.name.c_str());
                    std::optional<std::string> currentVal;
                    if (current)
                        currentVal = current;

                    if (currentVal != r.response.value) {
                        debug("replay invalidated: env %s changed", r.request.name);
                        return false;
                    }
                    return true;
                },
                [](const auto &) {
                    // Non-env entries (queries, results) don't need validation
                    return true;
                }},
            trace[cursor]);

        if (!valid)
            return false;

        ++cursor;
    }
    return true;
}

TracingReplayEvaluator::TracingReplayEvaluator(ref<Evaluator> inner, TracingDatabase & db)
    : inner(inner)
    , db(db)
{
    auto tracePath = db.latestTraceFile();
    if (!tracePath) {
        debug("no previous trace to replay");
        return;
    }

    try {
        trace = db.parseTraceFile(*tracePath);
        index.emplace(trace);
    } catch (const std::exception & e) {
        warn("could not load trace file: %s", e.what());
        return;
    }

    debug("loaded trace with %d entries", trace.size());
}

bool TracingReplayEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & TracingReplayEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & TracingReplayEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

EvalState * TracingReplayEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingReplayEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    if (!invalidated && index) {
        if (auto entry = index->lookup(trace::QueryImport{displayPath})) {
            if (auto * q = std::get_if<trace::Query<trace::QueryImport>>(&trace[entry->queryIndex])) {
                // Validate environment interactions up to the result
                size_t targetPos = entry->resultIndex ? entry->resultIndex : entry->queryIndex;
                if (validateEnvTo(targetPos)) {
                    debug("replay hit: evalFile %s", displayPath);
                    return make_ref<TracingReplayObject>(getStore(), trace, *index, q->v, [this, path, displayPath]() {
                        return inner->evalFile(path, displayPath);
                    });
                }
            }
        }
    }

    debug("replay miss: evalFile %s", displayPath);
    invalidated = true;
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    if (!invalidated && index) {
        if (auto entry = index->lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
            if (auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex])) {
                // Validate environment interactions up to the result
                size_t targetPos = entry->resultIndex ? entry->resultIndex : entry->queryIndex;
                if (validateEnvTo(targetPos)) {
                    debug("replay hit: evalExpr");
                    return make_ref<TracingReplayObject>(getStore(), trace, *index, q->v, [this, expr, basePath]() {
                        return inner->evalExpr(expr, basePath);
                    });
                }
            }
        }
    }

    debug("replay miss: evalExpr");
    invalidated = true;
    return inner->evalExpr(expr, basePath);
}

} // namespace nix
