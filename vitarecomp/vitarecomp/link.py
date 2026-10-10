"""Plano de link do runtime próprio: em que base cada módulo do Vita mora e onde fica cada
variável importada. As importações de variável remendam MOVW/MOVT no código, então a tradução
de cada módulo precisa conhecer a base dos módulos que exportam essas variáveis.

Plano (JSON):
  {"modules": [{"name": "gow1", "elf": "elf/gow1.elf", "base": "0x81000000"}, ...],
   "hle_var_base": "0x80F00000"}
"""
from __future__ import annotations

import json
from pathlib import Path

from . import module as modmod
from . import nids


NID_PROCESS_PARAM = 0x70FBA1E7   # export <main> do executável: sceProcessParam
NID_SCE_LIBC_PARAM = 0xDF084DFA  # importada pela libc (biblioteca SceLibcParam)


def _int(v) -> int:
    return int(v, 0) if isinstance(v, str) else int(v)


def load_plan(path: Path) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    root = path.parent
    for m in plan["modules"]:
        m["elf_path"] = str((root / m["elf"]).resolve())
        m["base"] = _int(m["base"])
    plan["hle_var_base"] = _int(plan.get("hle_var_base", "0x80F00000"))
    return plan


def resolve(plan: dict) -> dict:
    """Carrega todos os módulos nas bases do plano e decide o endereço de cada variável importada."""
    db = nids.load_db()
    mods = {}
    exports: dict[tuple[int, int], tuple[int, str]] = {}
    by_nid: dict[int, tuple[int, str]] = {}
    for m in plan["modules"]:
        mod = modmod.load(Path(m["elf_path"]).read_bytes(), m["name"], load_base=m["base"])
        mods[m["name"]] = mod
        for e in mod.exports:
            if e.library and e.library != "<main>":
                exports[(e.library_nid, e.nid)] = (e.addr, m["name"])
                by_nid.setdefault(e.nid, (e.addr, m["name"]))
    # Variáveis que o sistema fornece a partir do executável principal (o 1º do plano):
    # SceLibcParam é a própria estrutura sce_libc_param apontada pelo sceProcessParam.
    main = mods[plan["modules"][0]["name"]]
    special: dict[int, tuple[int, str]] = {}
    pp = next((e.addr for e in main.exports if e.nid == NID_PROCESS_PARAM), None)
    if pp is not None and main.read(pp + 4, 4) == b"PSP2":
        libc_param = main.u32(pp + 0x2C)
        if libc_param:
            special[NID_SCE_LIBC_PARAM] = (libc_param, f"{plan['modules'][0]['name']}:sce_libc_param")
    var_map: dict[str, dict] = {}
    next_slot = plan["hle_var_base"]
    for name, mod in mods.items():
        for imp in mod.imports:
            if imp.kind != "var":
                continue
            key = f"{imp.library_nid:#010x}:{imp.nid:#010x}"
            if key in var_map:
                continue
            hit = exports.get((imp.library_nid, imp.nid)) or by_nid.get(imp.nid) or special.get(imp.nid)
            if hit:
                var_map[key] = dict(addr=hit[0], source=hit[1])
            else:  # nenhum módulo carregado exporta: o runtime reserva a variável
                var_map[key] = dict(addr=next_slot, source="hle")
                next_slot += 16
            var_map[key]["name"] = nids.name_of(imp.library_nid, imp.nid, db)
            var_map[key]["library"] = imp.library
    return dict(modules=[{k: m[k] for k in ("name", "elf_path", "base")} for m in plan["modules"]],
                hle_var_base=plan["hle_var_base"], vars=var_map)


def var_resolver(link: dict):
    table = {k: v["addr"] for k, v in link["vars"].items()}

    def res(lib_nid: int, nid: int) -> int:
        return table[f"{lib_nid:#010x}:{nid:#010x}"]
    return res


def module_entry(link: dict, name: str) -> dict:
    name = name.strip()   # nomes vindos do shell no Windows podem trazer 
    for m in link["modules"]:
        if m["name"] == name:
            return m
    raise KeyError(f"módulo {name!r} não está no plano")
