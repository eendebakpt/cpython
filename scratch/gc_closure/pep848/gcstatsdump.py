"""Installed into the benchmark venv via a .pth file.  At interpreter exit,
append gc.get_stats() of this process to the file named by GC_STATS_OUT,
as one JSON line, so that the per-generation GC time of every pyperf
worker can be summed per benchmark."""
import atexit
import os

_out = os.environ.get("GC_STATS_OUT")


def _dump():
    import gc
    import json
    import sys
    try:
        stats = gc.get_stats()
    except Exception:
        return
    rec = {
        "argv0": os.path.basename(os.path.dirname(sys.argv[0])) or sys.argv[0],
        "worker": "--worker" in sys.argv,
        "pid": os.getpid(),
        "classic": os.environ.get("PYTHON_GC_CLASSIC", "0"),
        "impl": gc.get_implementation() if hasattr(gc, "get_implementation") else "?",
        "stats": stats,
    }
    with open(_out, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")


if _out:
    atexit.register(_dump)
