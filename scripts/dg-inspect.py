#!/usr/bin/env python3
"""
Decode the tracing-eval-cache decision-graph.sqlite into readable form.

Payload CBOR shapes (mirrors src/libexpr/tracing-decision-graph.cc +
serde headers):

  Requests table:
    ObservationSet is `array of {q: hex-hash, p: bytes (nested WHNF CBOR)}`.
    Requests are FileReadRequest / GetEnvRequest / OuterValueRequest,
    each an NLOHMANN_JSON_DEFINE-decorated struct: map with tag=…
    field + payload fields at same level.

  Selectors table:
    payload = CBOR of the Selector node — map with `tag` field and
    variant-specific fields. tag ∈ {"apply", "callbackApply",
    "getAttr", "getListElem", "getFunctionInfo", "arg", "import",
    "expr"}.

  Results table:
    payload = CBOR of the ResultWHNF — map with `type` field.

  ObservationSet table:
    payload = CBOR array of `{q: hex-hash-string, p: CBOR-bytes}`.

Usage:
    dg-inspect.py <db.sqlite> [table [hex-prefix]]

  With no args: summary counts + a few samples per table.
  With table: dump all rows in that table (or matching prefix if given).
"""

import sys
import sqlite3
import cbor2
import json

def hexh(b): return b.hex()

def try_cbor(b):
    try:
        return cbor2.loads(b)
    except Exception as e:
        return f"<cbor decode failed: {e}>"

def render_whnf(w):
    """Compact rendering of a ResultWHNF payload."""
    if not isinstance(w, dict):
        return repr(w)
    t = w.get("type", "?")
    if t == "set":
        names = w.get("names", [])
        return f"set{{{', '.join(names) if len(names) <= 8 else ', '.join(names[:8]) + f', … +{len(names)-8} more'}}}"
    if t == "list":
        return f"list[{w.get('size', '?')}]"
    if t == "string":
        v = w.get("value", "")
        ctx = w.get("context", [])
        vs = v if len(v) <= 60 else v[:60] + "…"
        return f'string({vs!r}{" +ctx="+str(len(ctx)) if ctx else ""})'
    if t == "bool":
        return f"bool({w.get('value')})"
    if t == "int":
        return f"int({w.get('value')})"
    if t == "float":
        return f"float({w.get('value')})"
    if t == "null":
        return "null"
    if t == "path":
        return f"path({w.get('path', '?')!r})"
    if t == "lambda":
        return "lambda"
    if t == "derivation":
        return f"deriv({w.get('drvPath', '?')})"
    return f"{t}({', '.join(f'{k}={v!r}' for k,v in w.items() if k != 'type')})"

def render_request(r):
    if not isinstance(r, dict):
        return repr(r)
    t = r.get("tag")
    if t == "outerValue":
        q = r.get("query", "?")
        return f"outer q={q[:12]}…"
    if t == "envFile":
        return f"envFile {r.get('absPath', '?')}"
    if t == "envVar":
        return f"envVar {r.get('name', '?')}"
    return f"{t}({r})"

def render_selector(s):
    if not isinstance(s, dict):
        return repr(s)
    t = s.get("tag")
    if t == "apply":
        return f"Apply(parent={s.get('parent','?')[:12]}…)"
    if t == "callbackApply":
        return f"CBApply(fn={s.get('parent','?')[:12]}…, obs={s.get('argObsSet','?')[:12]}…)"
    if t == "getAttr":
        return f"GetAttr({s.get('name','?')!r}, parent={s.get('parent','?')[:12]}…)"
    if t == "getListElem":
        return f"GetListElem(#{s.get('index','?')}, parent={s.get('parent','?')[:12]}…)"
    if t == "getFunctionInfo":
        return f"GetFnInfo(parent={s.get('parent','?')[:12]}…)"
    if t == "arg":
        return f"Arg(depth={s.get('depth','?')})"
    if t == "import":
        return f"Import({s.get('path','?')})"
    if t == "expr":
        return f"Expr({s.get('expr','?')})"
    return f"{t}({s})"

def render_obs(payload_bytes):
    """A single obs entry's `p` field: nested CBOR → WHNF."""
    inner = try_cbor(payload_bytes)
    return render_whnf(inner)

def inspect(db_path, table_filter=None, hex_prefix=None):
    con = sqlite3.connect(db_path)
    cur = con.cursor()

    tables = {
        "Selectors":       ("selectorHash", "payload", "selector"),
        "Requests":        ("requestHash",  "payload", "request"),
        "Results":         ("resultHash",   "payload", "result"),
        "ObservationSet":  ("setHash",      "payload", "obsset"),
        "RequestSetNodes": ("nodeHash",     "payload", "reqset"),
    }

    def dump_row(name, h, payload):
        hh = hexh(h)
        prefix = f"[{name} {hh[:12]}] "
        if name == "ObservationSet":
            arr = try_cbor(payload)
            if isinstance(arr, list):
                print(f"{prefix}{len(arr)} obs")
                for entry in arr:
                    if isinstance(entry, dict) and 'q' in entry and 'p' in entry:
                        q = entry['q']
                        p = entry['p']
                        pv = render_obs(p)
                        # look up the reqHash to give a query description
                        try:
                            qbin = bytes.fromhex(q)
                        except Exception:
                            qbin = None
                        req_desc = ""
                        if qbin is not None:
                            row = cur.execute(
                                "SELECT payload FROM Requests WHERE requestHash = ?",
                                (qbin,)).fetchone()
                            if row:
                                req_desc = f" [req={render_request(try_cbor(row[0]))}]"
                        print(f"   • q={q[:12]}…{req_desc}\n     → {pv}")
            else:
                print(f"{prefix}<not a list: {arr}>")
        elif name == "Selectors":
            s = try_cbor(payload)
            print(f"{prefix}{render_selector(s)}")
        elif name == "Requests":
            r = try_cbor(payload)
            print(f"{prefix}{render_request(r)}")
        elif name == "Results":
            r = try_cbor(payload)
            print(f"{prefix}{render_whnf(r)}")
        else:
            print(f"{prefix}{try_cbor(payload)}")

    if table_filter and table_filter not in tables:
        print(f"Unknown table {table_filter!r}. Choices: {list(tables)}")
        return 1

    for name, (hcol, pcol, _) in tables.items():
        if table_filter and name != table_filter:
            continue
        q = f"SELECT {hcol}, {pcol} FROM {name}"
        params = ()
        if hex_prefix:
            q += f" WHERE hex({hcol}) LIKE ?"
            params = (hex_prefix.upper() + "%",)
        rows = list(cur.execute(q, params))
        if not table_filter:
            print(f"\n=== {name}: {len(rows)} rows ===")
            for h, payload in rows[:5]:
                dump_row(name, h, payload)
            if len(rows) > 5:
                print(f"   … +{len(rows)-5} more")
        else:
            print(f"=== {name}: {len(rows)} matching rows ===")
            for h, payload in rows:
                dump_row(name, h, payload)
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__.strip())
        sys.exit(0)
    db = sys.argv[1]
    tbl = sys.argv[2] if len(sys.argv) > 2 else None
    hp  = sys.argv[3] if len(sys.argv) > 3 else None
    sys.exit(inspect(db, tbl, hp))
