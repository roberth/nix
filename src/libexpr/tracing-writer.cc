#include "nix/expr/tracing-writer.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix {

/* Walk a JSON tree and substitute string values that match a key in
   `sub` with the corresponding value. Used by flushPendingAmbient to
   replace placeholder hexes in `from` / `fn` / `arg` / `applyResultId`
   fields with their settled intrinsic / new-apply_qH hexes. */
static void substituteHexes(nlohmann::json & j, const std::map<std::string, std::string> & sub)
{
    if (j.is_object()) {
        for (auto & [_, val] : j.items()) {
            if (val.is_string()) {
                auto s = val.get<std::string>();
                auto it = sub.find(s);
                if (it != sub.end())
                    val = it->second;
            } else {
                substituteHexes(val, sub);
            }
        }
    } else if (j.is_array()) {
        for (auto & item : j)
            substituteHexes(item, sub);
    }
}

/* Record a Pass-1 or Pass-3 old→new substitution into the persistent
   map. Mappings are content-defined, so a placeholder colliding on
   `oldHex` across cycles SHOULD resolve to the same `newHex` (the
   computation is deterministic in the placeholder + observation
   history). A collision-with-different-value indicates a real bug:
   two distinct logical placeholders hash to the same value (likely
   because an apply Q's `arg` placeholder is the initial empty-cell
   contentId, which is the same hash for unrelated sibling cb
   invocations — see #63). Insert_or_assign keeps the latest, but the
   prior fact's pool entry now points at an orphan key.
   Logged at error level (via tracingCacheLog with
   _NIX_TRACING_CACHE_LOGGING=1) rather than thrown, because some
   existing test scenarios depend on the silent overwrite. Once #63 is
   fixed this should escalate to an exception. */
void TracingWriter::recordPreFlushSubstitution(
    const std::string & oldHex, const std::string & newHex, const char * passLabel)
{
    auto it = preFlushSubstitutions.find(oldHex);
    if (it != preFlushSubstitutions.end() && it->second != newHex) {
        tracingCacheStats().preFlushSubstitutionCollisions++;
        tracingCacheLog(
            "TracingWriter::%s: pre-flush-substitution collision: "
            "%s already mapped to %s, overwriting with %s (#63)",
            passLabel, oldHex, it->second, newHex);
    }
    preFlushSubstitutions.insert_or_assign(oldHex, newHex);
}

void TracingWriter::flushPendingAmbient()
{
    if (!decisionGraph)
        return;

    /* Descendant Locals (TracingLocalObjects produced via
       maybeGetAttr / getListElem on a parent Local) are
       content-addressed by their chain hash rooted in their parent's
       intrinsic, NOT by their own observation intrinsic — that's
       what replay's ReplayLocalObject.maybeGetAttr computes when
       navigating from a parent ReplayLocalObject. Recording must
       match by emitting descendant facts with from=chain_settled,
       so drop descendant placeholders from the intrinsic map before
       building `sub`. The cascade below then fills sub with their
       chain hashes via substituteHexes on derivationTemplate.

       Top-level Locals (callback-arg seeds from AmbientResolver::apply)
       are NOT in delayedContentDefinedIdentities — they stay in
       placeholderToIntrinsic and keep using intrinsic substitution
       for the apply Request's `arg` field, which is what enables
       same-shape collapse for callback args across unrelated calls
       (§2 of the design). Descendant collapse via intrinsic is
       deferred (would require an Asks-shaped index from descendant
       chain hash to descendant intrinsic; see the Open issues
       section of the design doc). */
    for (auto & dl : delayedContentDefinedIdentities)
        placeholderToIntrinsic.erase(dl.placeholderHex);

    /* Substitution map starts with: (a) local-placeholder → intrinsic
       from this cycle, and (b) persistent old→new mappings carried
       over from prior flush cycles (apply Q substitutions and chain
       child mappings — see #49). Persistence is what lets a fact
       referencing an apply_qH placeholder land at the substituted
       hash even when the fact is logged in a later flush cycle than
       the apply Q request itself. */
    std::map<std::string, std::string> sub = preFlushSubstitutions;
    for (auto & [placeholderHex, intrinsic] : placeholderToIntrinsic) {
        sub.insert_or_assign(placeholderHex, intrinsic.to_string(HashFormat::Base16, false));
    }

    /* Pass 1: process pending QueryApply Requests. Substituting the
       `arg` (and possibly `fn`) field shifts the payload's queryHash;
       record old→new so the apply-result placeholder used in
       downstream Ambient cascade entries and in facts' `from` fields
       resolves to the substituted apply_qH. Runs before the cascade
       walk because Ambient chain children's derivationTemplate has
       the placeholder apply_qH as `from`. */
    for (auto & req : pendingRequests) {
        if (req.keyPlaceholder)
            continue;
        auto oldHash = hashString(HashAlgorithm::SHA256, req.payload.dump());
        substituteHexes(req.payload, sub);
        auto newHash = hashString(HashAlgorithm::SHA256, req.payload.dump());
        if (oldHash != newHash) {
            auto oldHex = oldHash.to_string(HashFormat::Base16, false);
            auto newHex = newHash.to_string(HashFormat::Base16, false);
            sub.emplace(oldHex, newHex);
            /* Persist across flush cycles: a fact whose `from` is this
               request's old hash may be deferred to a later cycle (e.g.,
               an observation on an apply result that's logged after the
               apply Q is flushed). Without persistence the later cycle's
               sub starts empty for this mapping, leaves the fact's
               `from` unsubstituted, and replay's resolveAmbientId can't
               find the producer in the pool — falling back to a frozen
               ReplayLocalObject that serves stale recorded responses
               (#49). */
            recordPreFlushSubstitution(oldHex, newHex, "Pass1");
        }
        decisionGraph->insertRequest(newHash, jsonToCborString(req.payload));
    }

    /* Pass 2: process sidecar Requests. Their payload references the
       apply_qH (now substituted via pass 1's map entries) and their
       insertion key is the local's intrinsic hash (the substituted
       form of their keyPlaceholder). */
    for (auto & req : pendingRequests) {
        if (!req.keyPlaceholder)
            continue;
        substituteHexes(req.payload, sub);
        auto it = sub.find(*req.keyPlaceholder);
        const std::string & keyHex = (it != sub.end()) ? it->second : *req.keyPlaceholder;
        auto key = Hash::parseNonSRIUnprefixed(keyHex, HashAlgorithm::SHA256);
        decisionGraph->insertRequest(key, jsonToCborString(req.payload));
    }

    /* Cascade delayed content-defined identities: each child's
       settled hash is qH(derivation_query) with parent's placeholder
       hex in `from` substituted to parent's settled hash. Process in
       registration order — parents register before their children
       so each entry's parent is already in `sub` by the time we get
       here, including chains of derived-from-derived. Runs after
       pass 1 so Ambient children whose parent's settled hash is an
       apply_qH (provided by pass 1's old→new sub entry) substitute
       correctly. */
    for (auto & dl : delayedContentDefinedIdentities) {
        auto tmpl = dl.derivationTemplate;
        substituteHexes(tmpl, sub);
        auto finalHash = hashString(HashAlgorithm::SHA256, tmpl.dump());
        sub.emplace(dl.placeholderHex, finalHash.to_string(HashFormat::Base16, false));
    }

    /* Pass 3: process pending ambient facts. Substitute placeholders
       in the query JSON (local placeholders, old apply_qHs, and
       Ambient chain children's placeholders), then do what
       logAmbientInteraction used to do synchronously: compute
       reqHash + respHash, fold into v13FactSet, populate the
       Requests/Responses pools and the incremental writer state.

       Each fact's queryHash may change as a side effect of `from`
       substitution. Subsequent facts whose `from` is this fact's
       OLD queryHash (which is how a chain navigation step references
       its parent's identity) must substitute to the NEW queryHash
       too — otherwise the chain id replay computes can't reach the
       producer fact in the pool, and resolveAmbientId falls back to
       a frozen ReplayLocalObject. Register old→new in sub as we go;
       since pendingFacts is in observation order (parent observed
       before child), each child sees its parent's substitution by
       the time we process it. */
    for (auto & fact : pendingFacts) {
        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, fact.query);
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, fact.result);

        auto oldQueryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
        substituteHexes(queryJson, sub);

        /* computeQueryHash on typed Query objects round-trips through
           JSON serialisation, so hashing the substituted dump matches
           what we'd get if we deserialised into the typed variant and
           ran computeQueryHash. Going through JSON keeps this code
           agnostic of the QueryVariant alternatives. */
        auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
        if (oldQueryHash != queryHash) {
            auto oldHex = oldQueryHash.to_string(HashFormat::Base16, false);
            auto newHex = queryHash.to_string(HashFormat::Base16, false);
            sub.emplace(oldHex, newHex);
            /* Same reason as Pass 1: a downstream fact whose `from` is
               this fact's old query hash may flush later. */
            recordPreFlushSubstitution(oldHex, newHex, "Pass3");
        }
        auto responsePayload = jsonToCborString(resultJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        decisionGraph->insertResponse(queryHash, responsePayload);

        if (seenRequests.insert(queryHash).second) {
            v13FactSet.push_back({queryHash, responseHash});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, queryHash, responseHash);
            responseFor.emplace(queryHash, responseHash);
            allRequestsTrie.insert(queryHash);
        }
    }

    pendingFacts.clear();
    pendingRequests.clear();
    placeholderToIntrinsic.clear();
    delayedContentDefinedIdentities.clear();
}

} // namespace nix
