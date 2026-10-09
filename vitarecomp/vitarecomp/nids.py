"""NID name database (vita-headers db/*.yml) and Vita3K HLE coverage map.

HLE status for a function NID, derived from the Vita3K source tree:
  implemented   EXPORT(...) body without STUBBED/UNIMPLEMENTED markers
  stubbed       body contains STUBBED(...)  (returns a fake success)
  unimplemented body returns UNIMPLEMENTED() or the NID has no EXPORT body
  unknown       NID not present in Vita3K's nids.inc at all
"""
from __future__ import annotations

import re
from functools import lru_cache
from pathlib import Path

import yaml

DEFAULT_HEADERS = Path("/home/user/deps/vita-headers/db")
DEFAULT_VITA3K = Path("/home/user/deps/vita3k-preqt/vita3k")


@lru_cache(maxsize=None)
def load_db(db_dir: str = str(DEFAULT_HEADERS)) -> dict:
    """Return {"libs": {lib_nid: lib_name}, "funcs": {(lib_nid, nid): name}, "vars": {...}}."""
    libs: dict[int, str] = {}
    funcs: dict[tuple[int, int], str] = {}
    vars_: dict[tuple[int, int], str] = {}
    by_nid: dict[int, str] = {}
    for yml in sorted(Path(db_dir).glob("**/*.yml")):
        doc = yaml.safe_load(yml.read_text()) or {}
        for _mod_name, mod in (doc.get("modules") or {}).items():
            for lib_name, lib in (mod.get("libraries") or {}).items():
                lnid = lib.get("nid")
                if lnid is None:
                    continue
                libs[lnid] = lib_name
                for fname, fnid in (lib.get("functions") or {}).items():
                    funcs[(lnid, fnid)] = fname
                    by_nid.setdefault(fnid, fname)
                for vname, vnid in (lib.get("variables") or {}).items():
                    vars_[(lnid, vnid)] = vname
                    by_nid.setdefault(vnid, vname)
    return {"libs": libs, "funcs": funcs, "vars": vars_, "by_nid": by_nid}


def name_of(lib_nid: int, nid: int, db: dict | None = None) -> str | None:
    db = db or load_db()
    return db["funcs"].get((lib_nid, nid)) or db["vars"].get((lib_nid, nid)) or db["by_nid"].get(nid)


@lru_cache(maxsize=None)
def vita3k_hle(root: str = str(DEFAULT_VITA3K)) -> dict:
    """Map NID -> (name, status) from Vita3K sources."""
    rootp = Path(root)
    nid_names: dict[int, str] = {}
    for m in re.finditer(r"^\s*NID\((\w+),\s*(0x[0-9A-Fa-f]+)\)", (rootp / "nids/include/nids/nids.inc").read_text(),
                         re.M):  # anchored: commented-out "//NID(...)" lines must not count
        nid_names.setdefault(int(m[2], 16), m[1])
    status: dict[str, str] = {}
    export_re = re.compile(r"^EXPORT\([^,]+,\s*(\w+)")
    for src in (rootp / "modules").glob("**/*.cpp"):
        lines = src.read_text(errors="replace").splitlines()
        i = 0
        while i < len(lines):
            m = export_re.match(lines[i])
            if not m:
                i += 1
                continue
            name, depth, body = m[1], 0, []
            j = i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                body.append(lines[j])
                if depth <= 0 and "{" in "".join(body):
                    break
                j += 1
            text = "\n".join(body)
            if "UNIMPLEMENTED()" in text:
                status[name] = "unimplemented"
            elif "STUBBED(" in text:
                status[name] = "stubbed"
            else:
                status[name] = "implemented"
            i = j + 1
    return {nid: (name, status.get(name, "unimplemented")) for nid, name in nid_names.items()}


def hle_status(nid: int, root: str = str(DEFAULT_VITA3K)) -> tuple[str | None, str]:
    entry = vita3k_hle(root).get(nid)
    return entry if entry else (None, "unknown")
