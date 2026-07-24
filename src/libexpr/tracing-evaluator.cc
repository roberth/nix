#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/outer-object.hh"
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
    auto [v, qh] = writer.logRootSelector(trace::SelectorImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto triePos = writer.logResult(v, computeWHNFFromObject(*result), qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    /* Root scope-graph cell for the cached value. Cells now carry
       only topology (depth/parent/liveObject); state hashes are pure
       functions of the proxy's Subject under the via-Asks design. */
    obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExpr", expr);
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    auto [v, qh] = writer.logRootSelector(trace::SelectorExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto triePos = writer.logResult(v, computeWHNFFromObject(*result), qh);
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
    return obj;
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExprLazy", expr);
    ensurePreloaded();
    auto [v, qh] = writer.logRootSelector(trace::SelectorExpr{expr, basePath.path.abs()});
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
        if (auto hex = obj->getStateHashHex())
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
        if (auto hex = obj.getStateHashHex())
            return *hex;
        throw Error(
            "TracingEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnStateHashStr = getId(*fn);
    auto argStateHashStr = getId(*arg);

    tracingCacheLog("tracing: apply fnStateHash=%s argStateHash=%s", fnStateHashStr, argStateHashStr);

    /* cb-apply: record an explicit ε edge for this apply.
       createCallbackCell closes the preceding observations as one
       Asks edge (β1) and then records a synthetic single-observation
       Asks edge (ε) carrying just the apply Request — both sides
       advance their cumulative subject-id history by one for ε, so the
       apply-result's state hash is computed at a history step the walker
       can reach via the recorded chain. */
    nlohmann::json applyQ = trace::SelectorApply{fnStateHashStr, argStateHashStr};

    /* If fn is a TracingCallbackArg (= inner-supplied lambda the
       outer is now applying — the cb-higher-order case), capture the
       enclosing cell's applyId before proceeding so that observations
       recorded on the apply-result (via TracingCallbackApplyResult
       below) route to the enclosing CallbackCell's runningObsSet
       rather than to a fresh cell.

       Skip `createCallbackCell` entirely for the TracingCallbackArg-fn
       path: pushing a fresh empty cell there would produce spurious
       state that the enclosing cell already covers, since the nested
       apply is itself an observation on the enclosing cell's
       contra-arg. */
    bool fnIsTlo = dynamic_cast<TracingCallbackArg *>(fn.get_ptr().get()) != nullptr;

    /* Build the ApplyResultSubject from fn/arg constituents.

       Non-TracingCallbackArg: `getSubject()` on each with PostulatedIdempotentRead
       fallback (= satisfied by fresh-from-evalFile TracingObjects and
       literal `mk*` Objects per the variant contract).

       TracingCallbackArg-fn (= recursive cb-apply): the arg crosses the cb-apply
       boundary as `Arg{depth+1}` regardless of its
       outside-the-boundary Subject. Same convention as
       `OuterObject::queryApply` and the walker's
       `<replay-local-lambda>` primop. */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnStateHashStr, HashAlgorithm::SHA256);
    auto argSubjectHash = Hash::parseNonSRIUnprefixed(argStateHashStr, HashAlgorithm::SHA256);

    Subject fnSubj = fn->getSubject()
        ? *fn->getSubject()
        : Subject{PostulatedIdempotentRead{fnIdHash}};

    Subject argSubject;
    Hash argArgAncestryForApply{HashAlgorithm::SHA256};
    if (fnIsTlo) {
        auto callerScope = effectiveArgCell(*fn);
        int localDepth = callerScope ? callerScope->depth + 1 : 0;
        argSubject = Subject{Arg{localDepth}};
        argArgAncestryForApply = Hash{HashAlgorithm::SHA256};
    } else {
        argSubject = arg->getSubject()
            ? *arg->getSubject()
            : Subject{PostulatedIdempotentRead{argSubjectHash}};
        argArgAncestryForApply = arg->getArgAncestry();
    }

    /* Apply boundary's argAncestry combines fn's and arg's inherited scopes
       symmetrically but non-commutatively. The walker mirrors this. */
    Hash applyArgAncestry = combineArgAncestries(fn->getArgAncestry(), argArgAncestryForApply);

    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const Subject>(std::move(argSubject)),
    }};

    Hash enclosingApplyId(HashAlgorithm::SHA256);
    if (fnIsTlo) {
        if (auto enclosingId = writer.getCurrentCbApplyId())
            enclosingApplyId = *enclosingId;
        /* Nested cb-apply (fn is a TracingCallbackArg): the recursive
           apply itself is an observation on the enclosing cell's
           contra-arg. Under the obsSet CAS mechanism it's carried
           through the enclosing cell's runningObsSet — no dedicated
           fact record is needed at the writer level here. */
    } else {
        tracingCacheLog("createCallbackCell callsite=TracingEvaluator::apply fn=%s arg=%s",
                        fnStateHashStr.substr(0, 12), argStateHashStr.substr(0, 12));
        writer.createCallbackCell(applyQ);
    }

    /* Per-arg-completion option 2: apply-result state hash evolves with
       the writer's envWalk at the moment of apply. With the
       1:1 alignment restructure, writer.d1.size grows in lockstep
       with envAsksEdges; walker.envWalk grows per dispatched
       Asks edge. At sibling B's apply, walker.envWalk should
       have caught up to writer.d1.size at cold sib B apply (= all
       of sib A's envAsksEdges traversed via prior v13Walks). */
    auto & d1Walk = writer.getD1CidasksWalk();
    auto applyArgAncestryStateHash = stateHashAt(
        resultSubject, applyArgAncestry, d1Walk, d1Walk.size());
    auto applyArgAncestryStateHashHex = applyArgAncestryStateHash.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "writer apply: fn=%s arg=%s argAncestry=%s -> applyArgAncestryStateHash=%s",
            describe(*apr.fn),
            describe(*apr.arg),
            applyArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
            applyArgAncestryStateHashHex.substr(0, 16));
    }

    auto v = writer.getSink().logSelector(trace::SelectorApply{fnStateHashStr, argStateHashStr});

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
        std::shared_ptr<OuterResolver> resolver;
        Hash oldScope{HashAlgorithm::SHA256};
        ~CallScopeGuard() {
            if (resolver) setOuterResolverCallArgAncestry(*resolver, oldScope);
        }
    } guard;
    if (!fnIsTlo && !fnIsApplyResult) {
        if (auto resolver = inner->getOuterResolver()) {
            guard.resolver = resolver;
            guard.oldScope = getOuterResolverCallScope(*resolver);
            /* Sibling discrimination (cb-sibling-b): applyArgAncestryStateHash
               alone collides across siblings whose constituents are
               structurally identical at apply time. XOR in
               writer.envFactSetHash so cold's sibling A (applying at
               v13FactSet_A) and sibling B (applying at v13FactSet_B >
               v13FactSet_A) get distinct siblingScopes → distinct
               callback-arg inheritedScopes → distinct reqhashes for
               observations they emit. */
            auto siblingScope = TracingDecisionGraph::xorHashes(
                TracingDecisionGraph::xorHashes(guard.oldScope, applyArgAncestryStateHash),
                writer.getV13FactSetHash());
            setOuterResolverCallArgAncestry(*resolver, siblingScope);
        }
    }

    auto result = inner->apply(fn, arg);
    auto cell = ArgCell::make(effectiveArgCell(*fn), arg.get_ptr());

    /* For the TracingCallbackArg-fn case (cb-higher-order's recursive
       cb-apply): wrap the result in a TracingCallbackApplyResult so
       subsequent method calls (`getType`, `getInt`, etc.) route their
       observations into the enclosing CallbackCell's runningObsSet
       rather than into env main-trie Terminals. See
       tracing-callback-apply-result.hh for the recording flow. */
    if (fnIsTlo) {
        auto laro = std::make_shared<TracingCallbackApplyResult>(
            result, writer, std::move(resultSubject), applyArgAncestry, enclosingApplyId);
        laro->withArgCell(std::move(cell));
        return ref<Object>(laro);
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel; not keyed off this
        .queryHashStr = applyArgAncestryStateHashHex,
    };
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withArgCell(std::move(cell));
    obj->withApplyResultSubject(std::move(resultSubject), applyArgAncestry);
    if (auto * argAmb = dynamic_cast<OuterObject *>(arg.get_ptr().get())) {
        if (auto ctx = argAmb->getApplyContext())
            obj->withApplyContext(std::move(ctx));
    }
    return obj;
}

} // namespace nix
