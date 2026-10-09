"""vitarecomp CLI.

  python -m vitarecomp inspect <eboot.bin|module.elf> [-o report.json]

Prints a module summary: segments, entry points, relocation statistics,
imports grouped by library with names and Vita3K HLE status, and exports.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

from . import module as modmod
from . import nids


def inspect(path: Path, out: Path | None) -> int:
    try:
        mod = modmod.load(path.read_bytes(), path.name)
    except modmod.ModuleError as e:
        print(f"vitarecomp: error: {e}", file=sys.stderr)
        return 1
    db = nids.load_db()
    for imp in mod.imports:
        imp.name = nids.name_of(imp.library_nid, imp.nid, db)
    for exp in mod.exports:
        exp.name = nids.name_of(exp.library_nid, exp.nid, db)

    print(f"module {mod.name!r}  type={'RELEXEC' if mod.elf_type == modmod.ET_SCE_RELEXEC else 'EXEC'}  "
          f"module_info@{mod.info_addr:#010x}")
    for s in mod.segments:
        print(f"  seg{s.index}: {s.vaddr:#010x} memsz={s.memsz:#x} filesz={s.filesz:#x} "
              f"{'R' if s.flags & 4 else '-'}{'W' if s.flags & 2 else '-'}{'X' if s.flags & 1 else '-'}")
    if mod.module_start:
        print(f"  module_start={mod.module_start:#010x} ({'Thumb' if mod.module_start & 1 else 'ARM'})")
    reloc_hist = Counter(r.name for r in mod.relocs)
    print(f"  relocations applied: {len(mod.relocs)}  {dict(reloc_hist.most_common())}")

    by_lib: dict[str, list] = defaultdict(list)
    hle_counts: Counter = Counter()
    rows = []
    for imp in mod.imports:
        hname, status = nids.hle_status(imp.nid)
        if imp.kind != "func":
            status = "var"
        hle_counts[status] += 1
        by_lib[imp.library].append((imp, status))
        rows.append({"library": imp.library, "library_nid": f"{imp.library_nid:#010x}",
                     "nid": f"{imp.nid:#010x}", "name": imp.name or hname, "kind": imp.kind,
                     "stub": f"{imp.stub_addr:#010x}", "vita3k": status})
    print(f"  imports: {len(mod.imports)} in {len(by_lib)} libraries; Vita3K HLE: {dict(hle_counts)}")
    for lib, items in sorted(by_lib.items()):
        st = Counter(s for _, s in items)
        print(f"    {lib:<28} {len(items):>4}  {dict(st)}")
    for exp in mod.exports:
        print(f"  export {exp.library or '<main>'}:{exp.name or hex(exp.nid)} -> {exp.addr:#010x} ({exp.kind})")

    if out:
        report = {
            "module": mod.name,
            "segments": [{"index": s.index, "vaddr": f"{s.vaddr:#010x}", "memsz": s.memsz,
                          "filesz": s.filesz, "flags": s.flags} for s in mod.segments],
            "module_start": mod.module_start and f"{mod.module_start:#010x}",
            "module_stop": mod.module_stop and f"{mod.module_stop:#010x}",
            "relocations": dict(reloc_hist),
            "imports": rows,
            "exports": [{"library": e.library, "nid": f"{e.nid:#010x}", "name": e.name,
                         "addr": f"{e.addr:#010x}", "kind": e.kind} for e in mod.exports],
            "vita3k_hle_summary": dict(hle_counts),
        }
        out.write_text(json.dumps(report, indent=2) + "\n")
        print(f"report: {out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="vitarecomp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("inspect", help="load a Vita module and report its structure")
    p.add_argument("path", type=Path)
    p.add_argument("-o", "--out", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "inspect":
        return inspect(args.path, args.out)
    return 2


if __name__ == "__main__":
    sys.exit(main())
