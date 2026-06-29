#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/lambda-apply-result-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/expr/trace-file.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/environment.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/sync.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/expr/object-type.hh"

namespace nix {

TracingEvaluator::TracingEvaluator(TracingWriter & writer, ref<Evaluator> inner, TracingDatabase * db)
    : writer(writer)
    , inner(inner)
    , db(db)
{
}

void TracingEvaluator::ensurePreloaded()
{
    if (preloaded)
        return;
    preloaded = true;

    if (!db)
        return;

    auto & evalState = inner->getEvalState();

    auto latestTrace = db->latestTraceFile();
    if (!latestTrace)
        return;

    auto filePaths = db->getTracedFilePaths(*latestTrace);
    if (filePaths.empty())
        return;

    // Get the tracing source accessor from the environment
    auto accessor = evalState.environment->fsRoot();
    auto tracingAccessor = dynamic_cast<TracingSourceAccessor *>(&*accessor);
    if (!tracingAccessor)
        return;

    // Read files in parallel (I/O bound)
    struct PreloadedFile
    {
        CanonPath path;
        SpeculativeReadResult result;
    };

    Sync<std::vector<PreloadedFile>> preloadedFiles;

    ThreadPool pool;
    for (const auto & pathStr : filePaths) {
        pool.enqueue([&, pathStr]() {
            try {
                auto canonPath = CanonPath(pathStr);
                auto result = tracingAccessor->readSpeculatively(canonPath);
                preloadedFiles.lock()->push_back(
                    PreloadedFile{
                        .path = std::move(canonPath),
                        .result = std::move(result),
                    });
            } catch (...) {
                // Ignore read errors during preload
            }
        });
    }

    try {
        pool.process();
    } catch (...) {
        // Ignore pool errors during preload
    }

    // Parse sequentially (EvalState parsing is not thread-safe)
    for (auto & file : *preloadedFiles.lock()) {
        try {
            auto sourcePath = SourcePath{accessor, file.path};
            /* lazy-paths: parseExprFromString takes RootedPath. Preloaded
               .nix files come through the (system) rootFS accessor, so
               root them at `rootFSRoot`. */
            auto basePath = RootedPath{evalState.rootFSRoot, sourcePath.parent().path};
            auto expr = evalState.parseExprFromString(std::move(file.result.contents), basePath);
            evalState.insertPreloadedParsedFile(sourcePath, expr, std::move(file.result.emitTrace));
        } catch (...) {
            // Ignore parse errors during preload
        }
    }
}

bool TracingEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & TracingEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & TracingEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

EvalState & TracingEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingEvaluator::evalFile(const RootedPath & path, const std::string & displayPath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalFile %s", displayPath);
    auto [v, qh] = writer.logRootQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    /* Root scope-graph cell for the cached value. Cells now carry
       only topology (depth/parent/liveObject); content ids are pure
       functions of the proxy's Subject under the via-Asks design. */
    obj->withScope(ArgScopeCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withScope(ArgScopeCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    ensurePreloaded();
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    auto obj = TracingObject::create(result, writer, v);
    obj->withScope(ArgScopeCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::mkString(const std::string & s)
{
    auto result = inner->mkString(s);
    // Deterministic identity from content — no trie entry needed.
    auto hash = hashString(HashAlgorithm::SHA256, "mkString:" + s);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkInt(NixInt i)
{
    auto result = inner->mkInt(i);
    auto hash = hashString(HashAlgorithm::SHA256, "mkInt:" + std::to_string(i.value));
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkBool(bool b)
{
    auto result = inner->mkBool(b);
    auto hash = hashString(HashAlgorithm::SHA256, b ? "mkBool:true" : "mkBool:false");
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkPath(const RootedPath & path)
{
    auto result = inner->mkPath(path);
    /* Identity from the SourceRoot's unpinnedId + canon path. The
       unpinnedId strips revision-output attrs from the URL, so the
       same logical source at two different revs produces the same
       identity — exactly the property the trie needs to replay across
       upgrades of an input. SourceRoots without an unpinnedId (e.g.
       internal-helper accessors) fall back to a per-instance address;
       those are typically process-scoped and don't need cross-run
       replay anyway. */
    std::string content = "mkPath:";
    if (path.root->unpinnedId)
        content += *path.root->unpinnedId;
    else
        content += fmt("addr:%p", (void *) &*path.root);
    content += ":" + path.path.abs();
    auto hash = hashString(HashAlgorithm::SHA256, content);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::getInternalPrimOp(const std::string & name)
{
    auto result = inner->getInternalPrimOp(name);
    auto hash = hashString(HashAlgorithm::SHA256, "internalPrimOp:" + name);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    auto result = inner->mkAttrs(attrs);
    // Deterministic identity from attr names + child identities.
    std::string content = "mkAttrs:";
    for (auto & [name, obj] : attrs) {
        content += name + "=";
        if (auto hex = obj->getCdiHex())
            content += *hex;
        content += ",";
    }
    auto hash = hashString(HashAlgorithm::SHA256, content);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    /* fn and arg must be cache-boundary proxies whose identity is
       content-defined. No counter fallback — see the parallel
       comment in TracingReplayEvaluator::apply. */
    auto getId = [](Object & obj) -> std::string {
        if (auto hex = obj.getCdiHex())
            return *hex;
        throw Error(
            "TracingEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    tracingCacheLog("tracing: apply fnId=%s argId=%s", fnId, argId);

    /* cb-apply boundary: record an explicit ε edge for this apply.
       markApplyBoundary closes the preceding observations as one
       Asks edge (β1) and then records a synthetic single-observation
       Asks edge (ε) carrying just the apply Request — both sides
       advance their cumulative cidasks walk by one for ε, so the
       apply-result's CDI is computed at a walk index the walker
       can reach via the recorded chain. */
    nlohmann::json applyQ = trace::QueryApply{fnId, argId};

    /* If fn is a TracingLocalObject (= inner-supplied lambda the
       outer is now applying — the cb-higher-order case), record
       this apply as a depth-2 fact under the ENCLOSING cb-apply's
       chain. Per via-Asks Replay (depth-2): the lambda primop at
       warm pulls this edge by `(chainCursor, stampedReqHash)`.
       Walker-side counterpart: the lambda primop's impl advances
       the standin's chainCursor by this fact's elementHash.

       Capture the enclosing boundary's applyId BEFORE
       logDepth2ApplyFact / markApplyBoundary so the apply-result
       observations recorded after `inner->apply` returns (via
       `LambdaApplyResultObject` below) route to the same enclosing
       boundary the recursive apply Fact landed in. Their d=2
       chain order is: [recursiveApplyFact, applyResult.getType,
       applyResult.getInt, ...] — matching the walker's standin's
       primop manual-push (= one fact) followed by the synthetic's
       per-probe `advanceChainAndAppendFact` calls.

       Skip `markApplyBoundary` entirely for the TLO-fn path: it
       would push a fresh empty boundary whose synthetic d=1 fact
       `(applyReqHash, applyReqHash)` enters v13FactSet at finalize
       and forces the outer walker into a `dispatchApplyLive` whose
       arg has no sidecar — a guaranteed miss that destabilises the
       outer chain. The recursive apply Fact (recorded in the
       enclosing boundary by `logDepth2ApplyFact`) already covers
       the d=2 chain entry for this apply, so a separate boundary
       carries no information.

       Filtered to TLO fn specifically so we don't add d=2 facts
       for ordinary nested cb-applies (= cached-fn applied to outer
       values) — those don't go through the lambda-primop path at
       warm and would just contaminate the enclosing chain's
       AmbientResult. */
    bool fnIsTlo = dynamic_cast<TracingLocalObject *>(fn.get_ptr().get()) != nullptr;
    Hash enclosingApplyId(HashAlgorithm::SHA256);
    if (fnIsTlo) {
        if (auto enclosingId = writer.getCurrentApplyBoundaryId())
            enclosingApplyId = *enclosingId;
        auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQ.dump());
        writer.logDepth2ApplyFact(applyQ, applyReqHash);
    } else {
        writer.markApplyBoundary(applyQ);
    }

    /* Build the ApplyResultSubject from fn/arg constituents via
       polymorphic `getSubject()` — works for AmbientObject (PositionalSeed
       cb-arg or DerivedSubject child), TracingObject /
       TracingReplayObject when they're themselves apply results
       (ApplyResultSubject surfaced from applyResultSubject), and
       TracingLocalObject (PositionalSeed local). Fall back to
       OpaqueContentSubject only when getSubject() is null — that
       narrows the fallback to atoms whose CDI is fully determined
       at construction (e.g. fresh TracingObject from evalFile, an
       InterpreterObject wrapping a concrete value) and not subject
       to observation-driven evolution, matching the per-use rule
       in `tracing-eval-cache-per-arg-completion.md`. */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnId, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(argId, HashAlgorithm::SHA256);

    cidasks::Subject fnSubj = fn->getSubject()
        ? *fn->getSubject()
        : cidasks::Subject{cidasks::OpaqueContentSubject{fnIdHash}};

    cidasks::Subject argSubj = arg->getSubject()
        ? *arg->getSubject()
        : cidasks::Subject{cidasks::OpaqueContentSubject{argIdHash}};
    Hash applyScope = arg->getInheritedScope();

    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubj)),
    }};

    /* Per-arg-completion option 2: apply-result CDI evolves with
       the writer's d1CidasksWalk at the moment of apply. With the
       1:1 alignment restructure, writer.d1.size grows in lockstep
       with perQAsksEdges; walker.cidasksWalk grows per dispatched
       Asks edge. At sibling B's apply, walker.cidasksWalk should
       have caught up to writer.d1.size at cold sib B apply (= all
       of sib A's perQAsksEdges traversed via prior v13Walks). */
    auto & d1Walk = writer.getD1CidasksWalk();
    auto applyScopeStateId = cidasks::scopeStateIdAt(resultSubject, applyScope, d1Walk, d1Walk.size());
    auto applyScopeStateIdHex = applyScopeStateId.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<cidasks::ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "writer apply: fn=%s arg=%s scope=%s -> applyScopeStateId=%s",
            cidasks::describe(*apr.fn),
            cidasks::describe(*apr.arg),
            applyScope.to_string(HashFormat::Base16, false).substr(0, 12),
            applyScopeStateIdHex.substr(0, 16));
    }

    auto v = writer.getSink().logQuery(trace::QueryApply{fnId, argId});
    auto result = inner->apply(fn, arg);
    auto cell = ArgScopeCell::make(effectiveArgScope(*fn), arg.get_ptr());

    /* For the TLO-fn case (= cb-higher-order's recursive cb-apply):
       wrap the result in a LambdaApplyResultObject so subsequent
       method calls (`getType`, `getInt`, etc.) record d=2
       observations on the enclosing cb-apply boundary instead of
       d=1 main-trie Terminals. The walker's `<replay-local-lambda>`
       primop reads these from LocalResponseMap via the same
       per-arg-stamped reqHash; the AmbientAsks edges enable its
       synthetic's `advanceChainAndAppendFact` to keep
       `chainCursor` aligned with the cold AmbientResult. */
    if (fnIsTlo) {
        auto laro = std::make_shared<LambdaApplyResultObject>(
            result, writer, std::move(resultSubject), applyScope, enclosingApplyId);
        laro->withScope(std::move(cell));
        return ref<Object>(laro);
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel; v13 doesn't key off this
        .queryHashStr = applyScopeStateIdHex,
    };
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withScope(std::move(cell));
    obj->withApplyResultSubject(std::move(resultSubject), applyScope);
    if (auto * argAmb = dynamic_cast<AmbientObject *>(arg.get_ptr().get())) {
        if (auto ctx = argAmb->getApplyContext())
            obj->withApplyContext(std::move(ctx));
    }
    return obj;
}

} // namespace nix
