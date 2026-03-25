#include "nix/expr/trace-types.hh"

namespace nix::trace {

// ---------------------------------------------------------------------------
// Environment request/response serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const FileReadRequest & r)
{
    j = nlohmann::json{{"absPath", r.absPath}};
}

void from_json(const nlohmann::json & j, FileReadRequest & r)
{
    j.at("absPath").get_to(r.absPath);
}

void to_json(nlohmann::json & j, const FileReadResponse & r)
{
    j = nlohmann::json{{"contentHash", r.contentHash.to_string(HashFormat::SRI, true)}};
}

void from_json(const nlohmann::json & j, FileReadResponse & r)
{
    r.contentHash = Hash::parseSRI(j.at("contentHash").get<std::string>());
}

void to_json(nlohmann::json & j, const GetEnvRequest & r)
{
    j = nlohmann::json{{"name", r.name}};
}

void from_json(const nlohmann::json & j, GetEnvRequest & r)
{
    j.at("name").get_to(r.name);
}

void to_json(nlohmann::json & j, const GetEnvResponse & r)
{
    if (r.value)
        j = nlohmann::json{{"value", *r.value}};
    else
        j = nlohmann::json{{"value", nullptr}};
}

void from_json(const nlohmann::json & j, GetEnvResponse & r)
{
    auto & v = j.at("value");
    if (v.is_null())
        r.value = std::nullopt;
    else
        r.value = v.get<std::string>();
}

// ---------------------------------------------------------------------------
// Result payload serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const ResultType & r)
{
    j = nlohmann::json{{"type", r.type}};
}

void from_json(const nlohmann::json & j, ResultType & r)
{
    j.at("type").get_to(r.type);
}

void to_json(nlohmann::json & j, const ResultMaybeType & r)
{
    if (r.type)
        j = nlohmann::json{{"type", *r.type}};
    else
        j = nlohmann::json{{"type", nullptr}};
}

void from_json(const nlohmann::json & j, ResultMaybeType & r)
{
    auto & v = j.at("type");
    if (v.is_null())
        r.type = std::nullopt;
    else
        r.type = v.get<std::string>();
}

void to_json(nlohmann::json & j, const ResultString & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultString & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultStringWithContext & r)
{
    j = nlohmann::json{{"value", r.value}, {"context", r.context}};
}

void from_json(const nlohmann::json & j, ResultStringWithContext & r)
{
    j.at("value").get_to(r.value);
    j.at("context").get_to(r.context);
}

void to_json(nlohmann::json & j, const ResultInt & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultInt & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultFloat & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultFloat & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultBool & r)
{
    j = nlohmann::json{{"value", r.value}};
}

void from_json(const nlohmann::json & j, ResultBool & r)
{
    j.at("value").get_to(r.value);
}

void to_json(nlohmann::json & j, const ResultPath & r)
{
    j = nlohmann::json{{"path", r.path}};
}

void from_json(const nlohmann::json & j, ResultPath & r)
{
    j.at("path").get_to(r.path);
}

void to_json(nlohmann::json & j, const ResultListOfStrings & r)
{
    j = nlohmann::json{{"values", r.values}};
}

void from_json(const nlohmann::json & j, ResultListOfStrings & r)
{
    j.at("values").get_to(r.values);
}

void to_json(nlohmann::json & j, const ResultListSize & r)
{
    j = nlohmann::json{{"size", r.size}};
}

void from_json(const nlohmann::json & j, ResultListSize & r)
{
    j.at("size").get_to(r.size);
}

// ---------------------------------------------------------------------------
// Query payload serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json & j, const QueryExpr & q)
{
    j = nlohmann::json{{"query", "expr"}, {"params", {{"expr", q.expr}, {"baseDir", q.baseDir}}}};
}

void from_json(const nlohmann::json & j, QueryExpr & q)
{
    j.at("params").at("expr").get_to(q.expr);
    j.at("params").at("baseDir").get_to(q.baseDir);
}

void to_json(nlohmann::json & j, const QueryImport & q)
{
    j = nlohmann::json{{"query", "import"}, {"params", {{"path", q.path}}}};
}

void from_json(const nlohmann::json & j, QueryImport & q)
{
    j.at("params").at("path").get_to(q.path);
}

void to_json(nlohmann::json & j, const QueryGetAttr & q)
{
    j = nlohmann::json{{"query", "getAttr"}, {"params", {{"name", q.name}, {"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetAttr & q)
{
    j.at("params").at("name").get_to(q.name);
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetString & q)
{
    j = nlohmann::json{{"query", "getString"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetString & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetStringWithContext & q)
{
    j = nlohmann::json{{"query", "getStringWithContext"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetStringWithContext & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetAttrNames & q)
{
    j = nlohmann::json{{"query", "getAttrNames"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetAttrNames & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetType & q)
{
    j = nlohmann::json{{"query", "getType"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetType & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetBool & q)
{
    j = nlohmann::json{{"query", "getBool"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetBool & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetInt & q)
{
    j = nlohmann::json{{"query", "getInt"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetInt & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetFloat & q)
{
    j = nlohmann::json{{"query", "getFloat"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetFloat & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetListOfStrings & q)
{
    j = nlohmann::json{{"query", "getListOfStrings"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetListOfStrings & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetListSize & q)
{
    j = nlohmann::json{{"query", "getListSize"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetListSize & q)
{
    j.at("params").at("from").get_to(q.from);
}

void to_json(nlohmann::json & j, const QueryGetListElem & q)
{
    j = nlohmann::json{{"query", "getListElem"}, {"params", {{"from", q.from}, {"index", q.index}}}};
}

void from_json(const nlohmann::json & j, QueryGetListElem & q)
{
    j.at("params").at("from").get_to(q.from);
    j.at("params").at("index").get_to(q.index);
}

void to_json(nlohmann::json & j, const QueryGetPath & q)
{
    j = nlohmann::json{{"query", "getPath"}, {"params", {{"from", q.from}}}};
}

void from_json(const nlohmann::json & j, QueryGetPath & q)
{
    j.at("params").at("from").get_to(q.from);
}

} // namespace nix::trace
