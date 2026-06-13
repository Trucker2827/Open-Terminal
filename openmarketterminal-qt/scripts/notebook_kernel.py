"""Persistent notebook kernel for the Code editor.

A long-running process that execs each cell in a SHARED namespace, so state
(variables, imports, functions) carries across cells — a real kernel, unlike the
old "one fresh subprocess per cell" model.

Protocol (one JSON object per line):
  stdin   {"id": <int>, "code": "<source>"}        -> run a cell
          {"id": <int>, "cmd": "reset"}            -> clear the namespace (restart)
  stdout  {"id": <int>, "ok": <bool>, "stdout": "...", "stderr": "...",
           "result": "<repr of last expression, if any>",
           "error": "<short>", "traceback": ["..."]}

The cell's own print() output is captured (sys.stdout is redirected during exec)
and returned in the JSON — it never corrupts the protocol stream, which uses the
real stdout saved at startup.
"""
import ast
import io
import json
import sys
import traceback

_REAL_STDOUT = sys.stdout
_NS = {"__name__": "__main__", "__builtins__": __builtins__}


def _fresh_ns():
    _NS.clear()
    _NS["__name__"] = "__main__"
    _NS["__builtins__"] = __builtins__


def run_cell(code: str) -> dict:
    buf_out, buf_err = io.StringIO(), io.StringIO()
    result_repr, ok, err, tb = "", True, "", []
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = buf_out, buf_err
    try:
        tree = ast.parse(code, mode="exec")
        # Jupyter-style: if the last statement is a bare expression, eval it and
        # show its repr (so `df.head()` or `2 + 2` displays a value).
        last_expr = None
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            last_expr = ast.Expression(tree.body.pop().value)
        if tree.body:
            exec(compile(tree, "<cell>", "exec"), _NS)
        if last_expr is not None:
            val = eval(compile(last_expr, "<cell>", "eval"), _NS)
            if val is not None:
                try:
                    result_repr = repr(val)
                except Exception:
                    result_repr = "<unreprable object>"
    except Exception:
        ok = False
        tb = traceback.format_exc().splitlines()
        err = tb[-1] if tb else "Error"
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    return {"ok": ok, "stdout": buf_out.getvalue(), "stderr": buf_err.getvalue(),
            "result": result_repr, "error": err, "traceback": tb}


def _emit(obj):
    _REAL_STDOUT.write(json.dumps(obj) + "\n")
    _REAL_STDOUT.flush()


def main():
    _emit({"id": 0, "ready": True, "python": sys.version.split()[0]})
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        rid = req.get("id")
        if req.get("cmd") == "reset":
            _fresh_ns()
            _emit({"id": rid, "ok": True, "reset": True})
            continue
        res = run_cell(req.get("code", ""))
        res["id"] = rid
        _emit(res)


if __name__ == "__main__":
    main()
