"""Function discovery and per-function control-flow facts.

Sources of function entry points, in priority order:
  1. ELF FUNC symbols with sizes (when present).
  2. The ELF entry point and every ``jal`` target found in executable code.
  3. Code addresses stored in data (function-pointer tables) — only when they
     coincide with a symbol or call target, to avoid inventing entries.
GCC's ``foo.cold`` partitions are merged into their parent as extra chunks.
"""
from __future__ import annotations

import bisect
from dataclasses import dataclass, field

from .decoder import (BRANCH, BRANCH_LIKELY, BRANCH_LINK, CALL, CALL_REG, JUMP, JUMP_REG, Insn,
                      decode)
from .elf import Elf, Symbol


class AnalysisError(Exception):
    pass


@dataclass
class Function:
    name: str
    start: int
    chunks: list[tuple[int, int]]           # [start, end) ranges, first = entry chunk
    insns: dict[int, Insn] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    jump_table_targets: set[int] = field(default_factory=set)
    has_indirect_jump: bool = False
    callees: set[int] = field(default_factory=set)

    def contains(self, addr: int) -> bool:
        return any(s <= addr < e for s, e in self.chunks)


@dataclass
class Program:
    elf: Elf
    functions: dict[int, Function]
    text_ranges: list[tuple[int, int]]
    unknown: list[Insn]
    warnings: list[str]

    def func_at(self, addr: int) -> Function | None:
        return self.functions.get(addr)


def _text_sections(elf: Elf):
    return [s for s in elf.sections if s.is_exec and s.is_alloc and s.size]


def _sanitize(name: str) -> str:
    out = "".join(c if c.isalnum() or c == "_" else "_" for c in name)
    return out or "anon"


def analyze(elf: Elf) -> Program:
    texts = _text_sections(elf)
    if not texts:
        raise AnalysisError("no executable sections")
    text_ranges = [(s.addr, s.addr + s.size) for s in texts]

    def in_text(a: int) -> bool:
        return any(s <= a < e for s, e in text_ranges)

    # Decode every word of every executable section once.
    insns: dict[int, Insn] = {}
    for s, e in text_ranges:
        for a in range(s, e, 4):
            insns[a] = decode(a, elf.word(a))

    warnings: list[str] = []
    entries: dict[int, str] = {}
    sized: dict[int, int] = {}
    cold: list[Symbol] = []
    for sym in elf.symbols:
        if sym.type != Symbol.STT_FUNC or not in_text(sym.value):
            continue
        if ".cold" in sym.name:
            cold.append(sym)
            continue
        if sym.value not in entries or sym.bind == 1:  # prefer global names
            entries[sym.value] = sym.name
        if sym.size:
            sized[sym.value] = max(sized.get(sym.value, 0), sym.size)
    entries.setdefault(elf.entry, "_start")
    for ins in insns.values():
        if ins.op in ("jal",) and in_text(ins.target):
            entries.setdefault(ins.target, f"sub_{ins.target:08x}")
        if ins.op == "bgezal" and ins.rs == 0 and in_text(ins.target):  # bal
            entries.setdefault(ins.target, f"sub_{ins.target:08x}")

    starts = sorted(entries)
    cold_starts = sorted(c.value for c in cold)
    boundaries = sorted(set(starts) | set(cold_starts))
    functions: dict[int, Function] = {}
    for start in starts:
        if start in sized:
            end = start + sized[start]
        else:
            k = bisect.bisect_right(boundaries, start)
            end = boundaries[k] if k < len(boundaries) else next(e for s, e in text_ranges if s <= start < e)
        name = _sanitize(entries[start])
        functions[start] = Function(name=name, start=start, chunks=[(start, end)])

    # Attach cold partitions to their parent function by name.
    by_name = {f.name: f for f in functions.values()}
    for c in cold:
        parent = by_name.get(_sanitize(c.name.split(".cold")[0]))
        if parent is None:
            warnings.append(f"cold partition {c.name} has no parent; treated as function")
            functions[c.value] = Function(_sanitize(c.name), c.value, [(c.value, c.value + c.size)])
        else:
            parent.chunks.append((c.value, c.value + c.size))

    # Words in data sections that point into text: jump tables and fn-pointer tables.
    data_ptrs: set[int] = set()
    for sec in elf.sections:
        if sec.is_alloc and not sec.is_exec and sec.type != 8 and sec.size >= 4:
            for a in range(sec.addr, sec.addr + sec.size - 3, 4):
                v = elf.word(a)
                if in_text(v) and v % 4 == 0:
                    data_ptrs.add(v)
    # Immediate address materialisation (lui/addiu) also yields code pointers.
    for ins in insns.values():
        if ins.op == "lui":
            hi = ins.imm << 16
            nxt = insns.get(ins.addr + 4)
            for cand in (nxt, insns.get(ins.addr + 8)):
                if cand and cand.op in ("addiu", "ori") and cand.rs == ins.rt:
                    v = (hi + (cand.simm if cand.op == "addiu" else cand.imm)) & 0xFFFFFFFF
                    if in_text(v):
                        data_ptrs.add(v)

    unknown: list[Insn] = []
    for f in functions.values():
        for s, e in f.chunks:
            for a in range(s, e, 4):
                ins = insns[a]
                f.insns[a] = ins
                if ins.op is None:
                    unknown.append(ins)
                    continue
                if ins.flow in (BRANCH, BRANCH_LIKELY, JUMP):
                    if f.contains(ins.target):
                        f.labels.add(ins.target)
                    elif ins.target not in functions:
                        raise AnalysisError(
                            f"{ins}: branch from {f.name} to {ins.target:#010x}, which is neither "
                            "inside the function nor a known function entry")
                if ins.flow == BRANCH_LINK or ins.flow == CALL:
                    f.callees.add(ins.target)
                if ins.flow == JUMP_REG and ins.rs != 31:
                    f.has_indirect_jump = True
        if f.has_indirect_jump:
            f.jump_table_targets = {p for p in data_ptrs if f.contains(p)}
            f.labels |= f.jump_table_targets

    return Program(elf, functions, text_ranges, unknown, warnings)
