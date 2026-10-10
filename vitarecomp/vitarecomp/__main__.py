"""vitarecomp CLI.

  python -m vitarecomp inspect <eboot.bin|module.elf> [-o report.json]
  python -m vitarecomp keys-import <Vita3K>/vita3k/packages/src/sce_utils.cpp
  python -m vitarecomp decrypt <app.self> --rif work.bin -o app.elf

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


def recomp(path: Path | None, out: Path, linux: bool, trace: bool = False, base: int | None = None,
           link_path: Path | None = None, module_name: str | None = None) -> int:
    from .arm import analysis, generate, image
    link = None
    if link_path:
        from . import link as linkmod
        link = json.loads(link_path.read_text(encoding="utf-8"))
        entry = linkmod.module_entry(link, module_name)
        path, base = Path(entry["elf_path"]), entry["base"]
    data = path.read_bytes()
    if linux:
        img = image.from_linux_elf(data)
    else:
        try:
            mod = modmod.load(data, path.name, load_base=base)
        except modmod.ModuleError as e:
            print(f"vitarecomp: error: {e}", file=sys.stderr)
            return 1
        db = nids.load_db()
        for imp in mod.imports:
            imp.name = nids.name_of(imp.library_nid, imp.nid, db)
        for exp in mod.exports:
            exp.name = nids.name_of(exp.library_nid, exp.nid, db)
        if link:
            from . import link as linkmod
            bound = modmod.apply_var_imports(mod, linkmod.var_resolver(link))
            print(f"vitarecomp: {len(bound)} variáveis importadas ligadas "
                  f"({sum(b['sites'] for b in bound)} locais remendados)")
        img = image.from_vita_module(mod)
    prog = analysis.analyze(img)
    m = generate.generate(prog, out, trace)
    print(f"vitarecomp: {m['functions']} functions ({m['thumb_functions']} Thumb), "
          f"{m['instructions']} instructions -> {out}")
    if m["untranslated"]:
        print(f"vitarecomp: {len(m['untranslated'])} instruction(s) not translated yet (runtime trap):",
              file=sys.stderr)
        for e in m["untranslated"][:15]:
            print("   ", e, file=sys.stderr)
    return 0


def keys_import(src: Path, out: Path | None) -> int:
    from . import selfdec
    keys = selfdec.import_keys(src)
    dst = out or selfdec.default_keys_path()
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(json.dumps(keys, indent=1) + "\n")
    print(f"{len(keys)} chaves -> {dst}")
    return 0


def decrypt(path: Path, rif: Path | None, out: Path) -> int:
    import hashlib
    from . import selfdec
    data = path.read_bytes()
    klic = selfdec.klic_from_rif(rif.read_bytes()) if rif else None
    try:
        elf, h = selfdec.decrypt_to_elf(data, klic)
    except selfdec.SelfError as e:
        print(f"vitarecomp: error: {e}", file=sys.stderr)
        return 1
    out.write_bytes(elf)
    print(f"{path.name}: self_type={h['self_type']} key_rev={h['key_rev']} sys_version={h['sys_version']:#x} "
          f"auth_id={h['auth_id']:#x} npdrm_type={h['npdrm_type']} content_id={h['content_id']}")
    print(f"  ELF {len(elf)} bytes sha256={hashlib.sha256(elf).hexdigest()} -> {out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="vitarecomp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("inspect", help="load a Vita module and report its structure")
    p.add_argument("path", type=Path)
    p.add_argument("-o", "--out", type=Path)
    p = sub.add_parser("link", help="resolver o plano de link (bases e variáveis importadas)")
    p.add_argument("plan", type=Path)
    p.add_argument("-o", "--out", type=Path, required=True)
    p = sub.add_parser("recomp", help="recompile ARM code to C")
    p.add_argument("path", type=Path, nargs="?")
    p.add_argument("--link", type=Path, help="link.json de `vitarecomp link` (dá base e ELF do módulo)")
    p.add_argument("--module", help="nome do módulo no link.json")
    p.add_argument("-o", "--out", type=Path, required=True)
    p.add_argument("--linux", action="store_true", help="input is a Linux ARM ELF (test guests)")
    p.add_argument("--trace-insn", action="store_true", help="log full CPU state before every instruction")
    p.add_argument("--base", type=lambda v: int(v, 0), help="relocar o módulo para esta base (ET_SCE_RELEXEC)")
    p = sub.add_parser("keys-import", help="importar as chaves de SELF da fonte do Vita3K")
    p.add_argument("src", type=Path)
    p.add_argument("-o", "--out", type=Path)
    p = sub.add_parser("decrypt", help="decifrar um SELF (NPDRM com --rif) para ELF")
    p.add_argument("path", type=Path)
    p.add_argument("--rif", type=Path)
    p.add_argument("-o", "--out", type=Path, required=True)
    args = ap.parse_args(argv)
    if args.cmd == "link":
        from . import link as linkmod
        res = linkmod.resolve(linkmod.load_plan(args.plan))
        args.out.write_text(json.dumps(res, indent=1) + "\n", encoding="utf-8")
        hle = [v for v in res["vars"].values() if v["source"] == "hle"]
        # Para o runtime: variáveis que ele mesmo precisa criar (endereço, biblioteca, nome).
        args.out.with_suffix(".hlevars.txt").write_text(
            "".join(f"{v['addr']:#010x} {v['library']} {v['name'] or '?'}\n" for v in hle), encoding="utf-8")
        print(f"link: {len(res['modules'])} módulos, {len(res['vars'])} variáveis importadas "
              f"({len(hle)} sem exportador: reservadas pelo runtime) -> {args.out}")
        for v in hle:
            print(f"   hle {v['addr']:#010x} {v['library']}:{v['name']}")
        return 0
    if args.cmd == "keys-import":
        return keys_import(args.src, args.out)
    if args.cmd == "decrypt":
        return decrypt(args.path, args.rif, args.out)
    if args.cmd == "recomp":
        return recomp(args.path, args.out, args.linux, args.trace_insn, args.base, args.link, args.module)
    if args.cmd == "inspect":
        return inspect(args.path, args.out)
    return 2


if __name__ == "__main__":
    sys.exit(main())
