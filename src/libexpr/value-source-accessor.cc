#include "nix/expr/value-source-accessor.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/attr-set.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

ValueSourceAccessor::ValueSourceAccessor(EvalState & state, Value & root)
    : state(state)
    , root(allocRootValue(&root))
    , sEntries(state.symbols.create("entries"))
    , sContents(state.symbols.create("contents"))
    , sTarget(state.symbols.create("target"))
    , sExecutable(state.symbols.create("executable"))
{
    setPathDisplay("«makePath»");
}

void ValueSourceAccessor::anchor() {}

ValueSourceAccessor::Resolved ValueSourceAccessor::interpret(Value & v, const CanonPath & at)
{
    state.forceValue(v, noPos);
    if (v.type() == nPath)
        return Resolved{.delegate = v.path()};
    if (v.type() != nAttrs)
        state
            .error<TypeError>("node is %1%, expected an attribute set or a path", showType(v))
            .atPos(v.determinePos(noPos))
            .debugThrow();
    return Resolved{.node = &v};
}

std::string_view ValueSourceAccessor::nodeType(Value & node, const CanonPath & at)
{
    auto attr = node.attrs()->get(state.s.type);
    if (!attr)
        state.error<EvalError>("node is missing required attribute '%1%'", "type")
            .atPos(node.determinePos(noPos))
            .debugThrow();
    return state.forceStringNoCtx(
        *attr->value, attr->pos, "while evaluating the 'type' attribute of a 'builtins.makePath' node");
}

const Bindings & ValueSourceAccessor::nodeEntries(Value & node, const CanonPath & at)
{
    auto attr = node.attrs()->get(sEntries);
    if (!attr)
        state.error<EvalError>("directory node is missing required attribute '%1%'", "entries")
            .atPos(node.determinePos(noPos))
            .debugThrow();
    state.forceAttrs(
        *attr->value,
        attr->pos,
        "while evaluating the 'entries' attribute of a 'builtins.makePath' directory");
    return *attr->value->attrs();
}

ValueSourceAccessor::Resolved ValueSourceAccessor::walk(const CanonPath & path)
{
    Value * cur = *root;
    CanonPath cursor = CanonPath::root;
    PosIdx lastEntryPos = noPos;

    for (auto seg : path) {
        try {
            auto r = interpret(*cur, cursor);
            if (r.delegate) {
                auto remaining = path.removePrefix(cursor);
                return Resolved{.delegate = *r.delegate / remaining};
            }
            auto t = nodeType(*r.node, cursor);
            if (t != "directory")
                throw NotADirectory("'%1%' is not a directory", showPath(cursor));
            const auto & entries = nodeEntries(*r.node, cursor);
            auto attr = entries.get(state.symbols.create(seg));
            if (!attr)
                throw FileNotFound("'%1%' does not exist", showPath(cursor / seg));
            cur = attr->value;
            cursor.push(seg);
            lastEntryPos = attr->pos;
        } catch (Error & e) {
            if (lastEntryPos != noPos)
                e.addTrace(
                    state.positions[lastEntryPos],
                    "in 'builtins.makePath' entry '%1%'",
                    showPath(cursor));
            throw;
        }
    }

    auto r = interpret(*cur, cursor);
    r.entryPos = lastEntryPos;
    return r;
}

std::optional<SourceAccessor::Stat> ValueSourceAccessor::maybeLstat(const CanonPath & path)
{
    auto _level = state.addCallDepth(noPos);
    Resolved r;
    try {
        r = walk(path);
        if (r.delegate)
            return r.delegate->accessor->maybeLstat(r.delegate->path);

        auto t = nodeType(*r.node, path);
        if (t == "regular") {
            Stat st{.type = tRegular};
            if (auto exe = r.node->attrs()->get(sExecutable))
                st.isExecutable = state.forceBool(
                    *exe->value,
                    exe->pos,
                    "while evaluating the 'executable' attribute of a 'builtins.makePath' regular");
            return st;
        }
        if (t == "directory")
            return Stat{.type = tDirectory};
        if (t == "symlink")
            return Stat{.type = tSymlink};
        if (t == "unknown")
            return Stat{.type = tUnknown};

        auto typeAttr = r.node->attrs()->get(state.s.type);
        state
            .error<EvalError>(
                "node has unrecognised type '%1%'; expected one of '%2%', '%3%', '%4%', '%5%'",
                t,
                "regular",
                "directory",
                "symlink",
                "unknown")
            .atPos(typeAttr ? typeAttr->pos : noPos)
            .debugThrow();
    } catch (FileNotFound &) {
        return std::nullopt;
    } catch (Error & e) {
        e.addTrace(
            state.positions[r.entryPos],
            "while accessing '%1%' through 'builtins.makePath'",
            showPath(path));
        throw;
    }
}

SourceAccessor::DirEntries ValueSourceAccessor::readDirectory(const CanonPath & path)
{
    auto _level = state.addCallDepth(noPos);
    Resolved r;
    try {
        r = walk(path);
        if (r.delegate)
            return r.delegate->accessor->readDirectory(r.delegate->path);

        auto t = nodeType(*r.node, path);
        if (t != "directory")
            throw NotADirectory("not a directory");

        const auto & entries = nodeEntries(*r.node, path);
        DirEntries result;
        for (const auto & attr : entries) {
            std::string_view name = state.symbols[attr.name];
            if (name.empty() || name == "." || name == ".."
                || name.find('/') != std::string_view::npos)
                state
                    .error<EvalError>("invalid entry name '%1%'", name)
                    .atPos(attr.pos)
                    .debugThrow();
            result.emplace(std::string{name}, std::nullopt);
        }
        return result;
    } catch (Error & e) {
        e.addTrace(
            state.positions[r.entryPos],
            "while accessing '%1%' through 'builtins.makePath'",
            showPath(path));
        throw;
    }
}

std::string ValueSourceAccessor::readLink(const CanonPath & path)
{
    auto _level = state.addCallDepth(noPos);
    Resolved r;
    try {
        r = walk(path);
        if (r.delegate)
            return r.delegate->accessor->readLink(r.delegate->path);

        auto t = nodeType(*r.node, path);
        if (t != "symlink")
            throw NotASymlink("not a symlink");

        auto attr = r.node->attrs()->get(sTarget);
        if (!attr)
            state
                .error<EvalError>("symlink node is missing required attribute '%1%'", "target")
                .atPos(r.node->determinePos(noPos))
                .debugThrow();
        return std::string{state.forceStringNoCtx(
            *attr->value,
            attr->pos,
            "while evaluating the 'target' attribute of a 'builtins.makePath' symlink")};
    } catch (Error & e) {
        e.addTrace(
            state.positions[r.entryPos],
            "while accessing '%1%' through 'builtins.makePath'",
            showPath(path));
        throw;
    }
}

void ValueSourceAccessor::readFile(
    const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    auto _level = state.addCallDepth(noPos);
    Resolved r;
    try {
        r = walk(path);
        if (r.delegate) {
            r.delegate->accessor->readFile(r.delegate->path, sink, sizeCallback);
            return;
        }

        auto t = nodeType(*r.node, path);
        if (t != "regular")
            throw NotARegularFile("not a regular file");

        auto attr = r.node->attrs()->get(sContents);
        if (!attr)
            state
                .error<EvalError>("regular node is missing required attribute '%1%'", "contents")
                .atPos(r.node->determinePos(noPos))
                .debugThrow();

        state.forceValue(*attr->value, attr->pos);
        auto & cv = *attr->value;

        if (cv.type() == nString) {
            auto sv = cv.string_view();
            sizeCallback(sv.size());
            sink(sv);
            return;
        }
        if (cv.type() == nPath) {
            try {
                cv.path().readFile(sink, sizeCallback);
            } catch (Error & e) {
                e.addTrace(
                    state.positions[attr->pos],
                    "while reading the path referenced by the 'contents' attribute");
                throw;
            }
            return;
        }

        state
            .error<TypeError>(
                "'contents' attribute of a 'builtins.makePath' regular is %1%, expected a string or a path",
                showType(cv))
            .atPos(attr->pos)
            .debugThrow();
    } catch (Error & e) {
        e.addTrace(
            state.positions[r.entryPos],
            "while accessing '%1%' through 'builtins.makePath'",
            showPath(path));
        throw;
    }
}

} // namespace nix
