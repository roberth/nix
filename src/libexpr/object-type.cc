#include "nix/expr/object-type.hh"
#include "nix/util/error.hh"

namespace nix {

std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs:
        return "set";
    case nList:
        return "list";
    case nString:
        return "string";
    case nPath:
        return "path";
    case nInt:
        return "int";
    case nFloat:
        return "float";
    case nBool:
        return "bool";
    case nNull:
        return "null";
    case nFunction:
        return "lambda";
    case nExternal:
        return "external";
    case nThunk:
        return "thunk";
    case nFailed:
        return "failed";
    }
    return "unknown";
}

ObjectType stringToObjectType(const std::string & type)
{
    if (type == "set")
        return nAttrs;
    if (type == "list")
        return nList;
    if (type == "string")
        return nString;
    if (type == "path")
        return nPath;
    if (type == "int")
        return nInt;
    if (type == "float")
        return nFloat;
    if (type == "bool")
        return nBool;
    if (type == "null")
        return nNull;
    if (type == "lambda")
        return nFunction;
    if (type == "thunk")
        return nThunk;
    if (type == "external")
        return nExternal;
    if (type == "failed")
        return nFailed;
    throw Error("unknown object type: %s", type);
}

} // namespace nix
