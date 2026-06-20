#include "nix/expr/tracing-writer.hh"
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

void TracingWriter::flushPendingAmbient()
{
    if (!decisionGraph)
        return;

    /* Substitution map starts with local-placeholder → intrinsic. */
    std::map<std::string, std::string> sub;
    for (auto & [placeholderHex, intrinsic] : placeholderToIntrinsic) {
        sub.emplace(placeholderHex, intrinsic.to_string(HashFormat::Base16, false));
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
            sub.emplace(
                oldHash.to_string(HashFormat::Base16, false),
                newHash.to_string(HashFormat::Base16, false));
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
       Requests/Responses pools and the incremental writer state. */
    for (auto & fact : pendingFacts) {
        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, fact.query);
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, fact.result);

        substituteHexes(queryJson, sub);

        /* computeQueryHash on typed Query objects round-trips through
           JSON serialisation, so hashing the substituted dump matches
           what we'd get if we deserialised into the typed variant and
           ran computeQueryHash. Going through JSON keeps this code
           agnostic of the QueryVariant alternatives. */
        auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
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
            if (currentFrame_)
                currentFrame_->factSet.push_back({queryHash, responseHash});
        }
    }

    pendingFacts.clear();
    pendingRequests.clear();
    placeholderToIntrinsic.clear();
    delayedContentDefinedIdentities.clear();
}

} // namespace nix
