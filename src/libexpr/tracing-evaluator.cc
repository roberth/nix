#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/tracing-callback-apply-result.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-callback-arg.hh"
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

namespace {

/** `TracingEvaluator` is constructed only by `prim_cache`, so every
    instance is by definition the recording layer of a `builtins.cache`
    boundary. Reaching one of its root-value methods means the cache
    layer is about to construct (or has just constructed via a replay
    miss → delegation) a fresh root value — either as a top-level
    `evalFile`/`evalExpr` miss in the replay evaluator above, or as a
    `TracingReplayObject::ensureInner` escape that targets this
    evaluator directly via its `getInner` callback.

    `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1` asserts that neither path
    is allowed — the cache replay must serve every recorded value's
    behaviour from the cache alone. */
void guardCacheRecording(std::string_view method, std::string_view target)
{
    static const bool disallow =
        getEnv("_NIX_DISALLOW_CACHE_INTERPRET_INNER").value_or("") == "1";
    if (!disallow)
        return;
    throw Error(
        "tracing eval cache: the recording layer of a `builtins.cache` "
        "boundary was asked to %s `%s` (_NIX_DISALLOW_CACHE_INTERPRET_INNER=1). "
        "A recorded value's behaviour could not be served from the cache "
        "alone — the recording is either keyed at a query the walker "
        "cannot reach, or absent entirely.",
        method, target);
}

} // namespace

ref<Object> TracingEvaluator::evalFile(const RootedPath & path, const std::string & displayPath)
{
    guardCacheRecording("evalFile", displayPath);
    ensurePreloaded();
    tracingCacheLog("tracing: evalFile %s", displayPath);
    auto [v, qh] = writer.logRootQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    /* Root scope-graph cell for the cached value. Cells now carry
       only topology (depth/parent/liveObject); scope state ids are pure
       functions of the proxy's Subject under the via-Asks design. */
    obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExpr", expr);
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExprLazy", expr);
    ensurePreloaded();
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    auto obj = TracingObject::create(result, writer, v);
    obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
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
        if (auto hex = obj->getScopeStateIdHex())
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
        if (auto hex = obj.getScopeStateIdHex())
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
       openApplyBoundary closes the preceding observations as one
       Asks edge (β1) and then records a synthetic single-observation
       Asks edge (ε) carrying just the apply Request — both sides
       advance their cumulative cidasks walk by one for ε, so the
       apply-result's argStateId is computed at a walk index the walker
       can reach via the recorded chain. */
    nlohmann::json applyQ = trace::QueryApply{fnId, argId};

    /* If fn is a TracingCallbackArg (= inner-supplied lambda the
       outer is now applying — the cb-higher-order case), record
       this apply as a depth-2 fact under the ENCLOSING cb-apply's
       chain. Per via-Asks Replay (depth-2): the lambda primop at
       warm pulls this edge by `(chainCursor, stampedReqHash)`.
       Walker-side counterpart: the lambda primop's impl advances
       the standin's chainCursor by this fact's elementHash.

       Capture the enclosing boundary's applyId BEFORE
       logAmbientApplyFact / openApplyBoundary so the apply-result
       observations recorded after `inner->apply` returns (via
       `TracingCallbackApplyResult` below) route to the same enclosing
       boundary the recursive apply Fact landed in. Their d=2
       chain order is: [recursiveApplyFact, applyResult.getType,
       applyResult.getInt, ...] — matching the walker's standin's
       primop manual-push (= one fact) followed by the synthetic's
       per-probe `advanceChainAndAppendFact` calls.

       Skip `openApplyBoundary` entirely for the TLO-fn path: it
       would push a fresh empty boundary whose synthetic d=1 fact
       `(applyReqHash, applyReqHash)` enters envFactSet at finalize
       and forces the outer walker into a `dispatchApplyLive` whose
       arg has no sidecar — a guaranteed miss that destabilises the
       outer chain. The recursive apply Fact (recorded in the
       enclosing boundary by `logAmbientApplyFact`) already covers
       the d=2 chain entry for this apply, so a separate boundary
       carries no information.

       Filtered to TLO fn specifically so we don't add d=2 facts
       for ordinary nested cb-applies (= cached-fn applied to outer
       values) — those don't go through the lambda-primop path at
       warm and would just contaminate the enclosing chain's
       AmbientResult. */
    bool fnIsTlo = dynamic_cast<TracingCallbackArg *>(fn.get_ptr().get()) != nullptr;

    /* Build the ApplyResultSubject from fn/arg constituents.

       Non-TLO: `getSubject()` on each with PostulatedIdempotentRead
       fallback (= satisfied by fresh-from-evalFile TracingObjects and
       literal `mk*` Objects per the variant contract).

       TLO-fn (= recursive cb-apply): the arg crosses the cb-apply
       boundary as `PositionalSeed{depth+1}` regardless of its
       outside-the-boundary Subject. Same convention as
       `AmbientObject::queryApply` and the walker's
       `<replay-local-lambda>` primop. */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnId, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(argId, HashAlgorithm::SHA256);

    Subject fnSubj = fn->getSubject()
        ? *fn->getSubject()
        : Subject{PostulatedIdempotentRead{fnIdHash}};

    Subject argSubj;
    Hash argArgAncestryForApply{HashAlgorithm::SHA256};
    if (fnIsTlo) {
        auto callerScope = effectiveArgCell(*fn);
        int localDepth = callerScope ? callerScope->depth + 1 : 0;
        argSubj = Subject{PositionalSeed{localDepth}};
        argArgAncestryForApply = Hash{HashAlgorithm::SHA256};
    } else {
        argSubj = arg->getSubject()
            ? *arg->getSubject()
            : Subject{PostulatedIdempotentRead{argIdHash}};
        argArgAncestryForApply = arg->getArgAncestry();
    }

    /* Apply boundary's scope combines fn's and arg's inherited scopes
       symmetrically but non-commutatively. The walker mirrors this. */
    Hash applyArgAncestry = combineArgAncestries(fn->getArgAncestry(), argArgAncestryForApply);

    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const Subject>(std::move(argSubj)),
    }};

    Hash enclosingApplyId(HashAlgorithm::SHA256);
    if (fnIsTlo) {
        if (auto enclosingId = writer.getCurrentApplyBoundaryId())
            enclosingApplyId = *enclosingId;
        /* d=2 apply Fact: Subject = resultSubject built above;
           flushAmbient stamps via the generic
           pathAndRootsFromSubject path. The QueryApply payload's
           fn/arg use Subject-derived hex so the walker (which has
           only Subjects at primop firing time) can byte-match. */
        const auto & ars = std::get<ApplyResultSubject>(resultSubject.data);
        auto fnSubjHash = scopeStateIdAfter(*ars.fn, applyArgAncestry, {});
        auto argSubjHash = scopeStateIdAfter(*ars.arg, applyArgAncestry, {});
        auto fnSubjHex = fnSubjHash.to_string(HashFormat::Base16, false);
        auto argSubjHex = argSubjHash.to_string(HashFormat::Base16, false);
        tracingCacheLog(
            "writer logAmbientApplyFact: fnSubj=%s argSubj=%s applyArgAncestry=%s fnHex=%s argHex=%s",
            describe(*ars.fn),
            describe(*ars.arg),
            applyArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
            fnSubjHex.substr(0, 12),
            argSubjHex.substr(0, 12));
        nlohmann::json applyQd2 = trace::QueryApply{fnSubjHex, argSubjHex};
        writer.logAmbientApplyFact(applyQd2, resultSubject, applyArgAncestry);
    } else {
        tracingCacheLog("openApplyBoundary callsite=TracingEvaluator::apply fn=%s arg=%s",
                        fnId.substr(0, 12), argId.substr(0, 12));
        writer.openApplyBoundary(applyQ);
    }

    /* Per-arg-completion option 2: apply-result argStateId evolves with
       the writer's envWalk at the moment of apply. With the
       1:1 alignment restructure, writer.d1.size grows in lockstep
       with envAsksEdges; walker.envWalk grows per dispatched
       Asks edge. At sibling B's apply, walker.envWalk should
       have caught up to writer.d1.size at cold sib B apply (= all
       of sib A's envAsksEdges traversed via prior v13Walks). */
    auto & d1Walk = writer.getD1CidasksWalk();
    /* Path 3: stamp SubjectEvolutionEdges via hook. */
    Hash resultSelfHash = scopeStateIdAt(
        resultSubject, Hash(HashAlgorithm::SHA256), {}, 0);
    auto applyArgAncestryStateHash = scopeStateIdAtWithHook(
        resultSubject, applyArgAncestry, d1Walk, d1Walk.size(),
        [&](const EvolutionStep & step) {
            writer.insertSubjectEvolutionEdge(
                resultSelfHash, step.curBefore,
                step.obsFromHash, step.obsElementHash,
                step.curAfter);
        });
    auto applyArgAncestryStateHashHex = applyArgAncestryStateHash.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "writer apply: fn=%s arg=%s scope=%s -> applyArgAncestryStateHash=%s",
            describe(*apr.fn),
            describe(*apr.arg),
            applyArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
            applyArgAncestryStateHashHex.substr(0, 16));
    }

    auto v = writer.getSink().logQuery(trace::QueryApply{fnId, argId});

    /* Per-invocation callArgAncestry for GENUINE cb-apply (not curried
       follow-up): sibling cb-apply invocations of the SAME cached
       primop (cb-sibling's `cached { fA }` vs `cached { fB }`) share
       the same resolver->callArgAncestry, so their inner facts stamp
       identical `from` fields → reqhash collision.

       Distinguish genuine cb-apply from curried follow-up by fn's
       Subject: genuine cb-apply's fn is a fresh cached primop (its
       Subject is Opaque/PostulatedIdempotentRead of the primop's
       identity hash). Curried follow-up's fn is an ApplyResultSubject
       (result of a previous apply). Only XOR at the former. */
    bool fnIsApplyResult = fn->getSubject()
        && std::holds_alternative<ApplyResultSubject>(fn->getSubject()->data);
    struct CallScopeGuard {
        std::shared_ptr<AmbientResolver> resolver;
        Hash oldScope{HashAlgorithm::SHA256};
        ~CallScopeGuard() {
            if (resolver) setAmbientResolverCallScope(*resolver, oldScope);
        }
    } guard;
    if (!fnIsTlo && !fnIsApplyResult) {
        if (auto resolver = inner->getAmbientResolver()) {
            guard.resolver = resolver;
            guard.oldScope = getAmbientResolverCallScope(*resolver);
            /* Sibling discrimination (cb-sibling-b): applyArgAncestryStateHash
               alone collides across siblings whose constituents are
               structurally identical at apply time. XOR in
               writer.envFactSetHash so cold's sibling A (applying at
               v13FactSet_A) and sibling B (applying at v13FactSet_B >
               v13FactSet_A) get distinct siblingScopes → distinct
               inner-ambient-object inheritedScopes → distinct
               reqhashes for observations they emit. */
            auto siblingScope = TracingDecisionGraph::xorHashes(
                TracingDecisionGraph::xorHashes(guard.oldScope, applyArgAncestryStateHash),
                writer.getV13FactSetHash());
            setAmbientResolverCallScope(*resolver, siblingScope);
        }
    }

    auto result = inner->apply(fn, arg);
    auto cell = ArgCell::make(effectiveArgCell(*fn), arg.get_ptr());

    /* For the TLO-fn case (= cb-higher-order's recursive cb-apply):
       wrap the result in a TracingCallbackApplyResult so subsequent
       method calls (`getType`, `getInt`, etc.) record d=2
       observations on the enclosing cb-apply boundary instead of
       d=1 main-trie Terminals. The walker's `<replay-local-lambda>`
       primop reads these from LocalResponseMap via the same
       per-arg-stamped reqHash; the AmbientAsks edges enable its
       synthetic's `advanceChainAndAppendFact` to keep
       `chainCursor` aligned with the cold AmbientResult. */
    if (fnIsTlo) {
        auto laro = std::make_shared<TracingCallbackApplyResult>(
            result, writer, std::move(resultSubject), applyArgAncestry, enclosingApplyId);
        laro->withArgCell(std::move(cell));
        return ref<Object>(laro);
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel; v13 doesn't key off this
        .queryHashStr = applyArgAncestryStateHashHex,
    };
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withArgCell(std::move(cell));
    obj->withApplyResultSubject(std::move(resultSubject), applyArgAncestry);
    if (auto * argAmb = dynamic_cast<AmbientObject *>(arg.get_ptr().get())) {
        if (auto ctx = argAmb->getApplyContext())
            obj->withApplyContext(std::move(ctx));
    }
    return obj;
}

} // namespace nix
