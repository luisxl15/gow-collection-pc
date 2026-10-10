"""Function discovery for ARMv7 (ARM + Thumb-2) by recursive descent.

Entry points: ELF entry, symbols/exports, BL/BLX targets, code pointers (data
words or SCE relocations). Each function is explored from its entry following
intra-procedural control flow; a branch to another function's entry is a tail
call. Discovery iterates to a fixpoint because new BL targets appear while
exploring. Literal pools are never decoded because only reachable code is.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import capstone
from capstone import arm as A

from .image import GuestImage

COND_AL = A.ARM_CC_AL
_FLAG_SETTING = {"mov", "mvn", "add", "adc", "sub", "sbc", "rsb", "rsc", "and", "orr", "orn", "eor", "bic",
                 "lsl", "lsr", "asr", "ror", "rrx", "mul", "mla", "umull", "smull", "umlal", "smlal"}
REG_NAMES = {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}


def reg_num(insn, reg_id: int) -> int | None:
    name = insn.reg_name(reg_id)
    if name in REG_NAMES:
        return REG_NAMES[name]
    if name and name[0] == "r" and name[1:].isdigit():
        return int(name[1:])
    return None


@dataclass
class Insn:
    addr: int
    size: int
    thumb: bool
    cs: object            # capstone CsInsn
    name: str             # capstone insn_name()
    raw: bytes
    uf: bool = False      # sets APSR flags (from the encoding's S bit where available)

    @property
    def next(self) -> int:
        return self.addr + self.size


@dataclass
class Function:
    entry: int            # without Thumb bit
    thumb: bool
    name: str
    insns: dict[int, Insn] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    tables: dict[int, list[int]] = field(default_factory=dict)  # tbb/tbh addr -> targets
    unknown: list[int] = field(default_factory=list)
    indirect_jumps: list[int] = field(default_factory=list)
    swept: bool = False

    @property
    def key(self) -> int:
        return self.entry | (1 if self.thumb else 0)


class Decoder:
    def __init__(self, img: GuestImage):
        self.img = img
        self.md = {}
        for thumb in (False, True):
            md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
            md.detail = True
            self.md[thumb] = md
        self.cache: dict[tuple[int, bool], Insn | None] = {}

    def at(self, addr: int, thumb: bool) -> Insn | None:
        key = (addr, thumb)
        if key in self.cache:
            return self.cache[key]
        ins = None
        try:
            raw = self.img.read(addr, 4)
        except ValueError:
            raw = b""
        for c in self.md[thumb].disasm(raw, addr, 1):
            ins = self._wrap(c, thumb)
        self.cache[key] = ins
        if ins is not None and ins.name == "it":
            # IT state is sequential: decode the whole block in one call so capstone
            # applies the right condition (and no-flag-setting rule) to each instruction.
            count = len(ins.cs.mnemonic.split(".")[0]) - 1
            block = self.img.read(addr, 2 + 4 * count)
            for c in list(self.md[thumb].disasm(block, addr, count + 1))[1:]:
                self.cache[(c.address, thumb)] = self._wrap(c, thumb)
        return ins

    @staticmethod
    def _wrap(c, thumb: bool) -> Insn:
        name = c.insn_name() or c.mnemonic
        if name.endswith("s") and name[:-1] in _FLAG_SETTING:  # capstone reports e.g. "movs"
            name = name[:-1]
        raw = bytes(c.bytes)
        return Insn(c.address, c.size, thumb, c, name, raw, _s_bit(name, raw, thumb, c.update_flags))


_DP = {"mov", "mvn", "add", "adc", "sub", "sbc", "rsb", "rsc", "and", "orr", "orn", "eor", "bic",
       "lsl", "lsr", "asr", "ror", "rrx"}
_MUL = {"mul", "mla", "umull", "smull", "umlal", "smlal"}
_TEST = {"cmp", "cmn", "tst", "teq"}


def _s_bit(name: str, raw: bytes, thumb: bool, cs_uf: bool) -> bool:
    """Whether the instruction writes APSR.NZCV. capstone 5 misreports some
    32-bit encodings (e.g. ADC.W without S), so read the S bit directly."""
    if name in _TEST:
        return True
    if name not in _DP and name not in _MUL:
        return cs_uf
    if thumb:
        if len(raw) == 2:
            return cs_uf  # 16-bit: sets flags outside IT, not inside (capstone tracks IT)
        hw1 = raw[0] | raw[1] << 8
        hw2 = raw[2] | raw[3] << 8
        if (hw1 & 0xFA00) == 0xF000 and not hw2 & 0x8000:   # data processing, modified immediate
            return bool(hw1 & 0x10)
        if (hw1 & 0xFE00) == 0xEA00:                          # data processing, shifted register
            return bool(hw1 & 0x10)
        if (hw1 & 0xFF80) == 0xFA00 and (hw2 & 0xF0F0) == 0xF000:  # register-controlled shifts
            return bool(hw1 & 0x10)
        if (hw1 & 0xFF80) in (0xFB00, 0xFB80):                # multiply / long multiply: never set flags
            return False
        return cs_uf
    word = int.from_bytes(raw, "little")
    return bool(word >> 20 & 1)


def _is_return(i: Insn) -> bool:
    n, ops = i.name, i.cs.operands
    if n == "bx" and ops and reg_num(i.cs, ops[0].reg) == 14:
        return True
    if n in ("pop", "ldm") and any(o.type == A.ARM_OP_REG and reg_num(i.cs, o.reg) == 15 for o in ops):
        return True
    if n == "ldr" and ops and ops[0].type == A.ARM_OP_REG and reg_num(i.cs, ops[0].reg) == 15:
        mem = ops[1]
        return mem.type == A.ARM_OP_MEM and reg_num(i.cs, mem.mem.base) == 13 and i.cs.writeback
    if n == "mov" and len(ops) == 2 and ops[0].type == A.ARM_OP_REG and reg_num(i.cs, ops[0].reg) == 15 \
            and ops[1].type == A.ARM_OP_REG and reg_num(i.cs, ops[1].reg) == 14:
        return True
    return False


def writes_pc(i: Insn) -> bool:
    ops = i.cs.operands
    if i.name in ("pop", "ldm", "ldmdb", "ldmib", "ldmda"):
        return any(o.type == A.ARM_OP_REG and reg_num(i.cs, o.reg) == 15 for o in ops)
    if i.name in ("b", "bx", "bl", "blx", "cbz", "cbnz", "tbb", "tbh", "cmp", "cmn", "tst", "teq",
                  "str", "strb", "strh", "strd", "push", "stm", "stmdb", "it"):
        return False
    return bool(ops) and ops[0].type == A.ARM_OP_REG and reg_num(i.cs, ops[0].reg) == 15


def _prev_fall(i: Insn, by_addr: dict[int, Insn]) -> Insn | None:
    """Instrução imediatamente anterior que CAI em `i` (não é desvio incondicional nem retorno)."""
    for size in (2, 4):
        p = by_addr.get(i.addr - size)
        if p is not None and p.next == i.addr:
            if p.name == "b" and p.cs.cc == COND_AL and not _in_it(p) or _is_return(p):
                return None
            return p
    return None


def _const_reg(reg: int, at: Insn, by_addr: dict[int, Insn]) -> int | None:
    """Valor constante de `reg` imediatamente antes de `at`, se montado logo antes por
    movw/movs/mov #lo (+ movt #hi) no mesmo trecho contíguo."""
    lo = hi = None
    cur = at
    for _ in range(6):
        cur = _prev_fall(cur, by_addr)
        if cur is None:
            return None
        ops = cur.cs.operands
        if not ops or ops[0].type != A.ARM_OP_REG or reg_num(cur.cs, ops[0].reg) != reg:
            continue
        if cur.name == "movt" and hi is None and ops[-1].type == A.ARM_OP_IMM:
            hi = ops[-1].imm & 0xFFFF
            continue
        if cur.name in ("movw", "mov") and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
            v = ops[1].imm & 0xFFFFFFFF
            return ((hi << 16) | (v & 0xFFFF)) if hi is not None else v
        return None   # outra escrita em reg: não é constante
    return None


def _affine_src(i: Insn, by_addr: dict[int, Insn]) -> tuple[int, int] | None:
    """(rm, k) quando `i` faz rd = rm - k com k constante: mov rd, rm; adds/subs rd, rm, #k;
    adds rd, rm, rK com rK constante montado logo antes (movw/movt)."""
    ops = i.cs.operands
    if i.name == "mov" and len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ops[1].shift.type == 0:
        return reg_num(i.cs, ops[1].reg), 0
    if i.name in ("add", "sub") and len(ops) == 3 and ops[1].type == A.ARM_OP_REG:
        if ops[2].type == A.ARM_OP_IMM:
            k = ops[2].imm & 0xFFFFFFFF
        elif ops[2].type == A.ARM_OP_REG and ops[2].shift.type == 0:
            k = _const_reg(reg_num(i.cs, ops[2].reg), i, by_addr)
            if k is None:
                return None
        else:
            return None
        return reg_num(i.cs, ops[1].reg), (k if i.name == "sub" else -k) & 0xFFFFFFFF
    return None


def _writes_reg(i: Insn, r: int) -> bool:
    ops = i.cs.operands
    if i.name in ("cmp", "cmn", "tst", "teq", "str", "strb", "strh", "strd", "push", "stm", "stmdb",
                  "b", "bl", "blx", "bx", "cbz", "cbnz", "it", "vstr", "vldr", "vstmia", "vpush"):
        return False
    return bool(ops) and ops[0].type == A.ARM_OP_REG and reg_num(i.cs, ops[0].reg) == r


# Desvio NÃO tomado que garante idx <= N (hi/gt) ou idx < N (hs/ge) após `cmp idx, N`;
# e desvio TOMADO que garante o mesmo (ls/le e lo/lt).
_GUARD_FALL = {A.ARM_CC_HI: 1, A.ARM_CC_GT: 1, A.ARM_CC_HS: 0, A.ARM_CC_GE: 0}
_GUARD_TAKEN = {A.ARM_CC_LS: 1, A.ARM_CC_LE: 1, A.ARM_CC_LO: 0, A.ARM_CC_LT: 0}


def _cmp_bound(c: Insn, reg: int, delta: int, extra: int, by_addr: dict[int, Insn]) -> int | None:
    ops = c.cs.operands
    if c.name != "cmp" or len(ops) != 2 or ops[0].type != A.ARM_OP_REG or reg_num(c.cs, ops[0].reg) != reg:
        return None
    if ops[1].type == A.ARM_OP_IMM:
        n = ops[1].imm & 0xFFFFFFFF
    elif ops[1].type == A.ARM_OP_REG and ops[1].shift.type == 0:
        n = _const_reg(reg_num(c.cs, ops[1].reg), c, by_addr)
        if n is None:
            return None
    else:
        return None
    count = ((n - delta) & 0xFFFFFFFF) + extra
    return count if 0 < count <= 4096 else None


def _switch_bound(before: list[Insn], idx_reg: int, all_insns: dict[int, Insn] | None = None) -> int | None:
    """Prova um limite superior para o índice de um tbb/tbh, andando para trás pelo caminho que
    chega nele. Aceita duas formas de guarda:
      cmp idx, N ; b{hi,gt,hs,ge} default ; ... ; tbb      (cai no tbb quando idx <= N)
      cmp idx, N ; b{ls,le,lo,lt} bloco   ...  bloco: tbb  (salta para o tbb quando idx <= N)
    N pode ser imediato ou constante montada por movw/movt. Acompanha cópias e deslocamentos
    constantes do índice (`subs r0,r1,#0xf0`, `adds r1,r0,rK`). Sem guarda provada devolve None:
    um limite pequeno demais cortaria casos reais, e o chamador recorre ao layout da tabela."""
    by_addr = {i.addr: i for i in before}
    if all_insns:
        by_addr.update(all_insns)
    reg, delta = idx_reg, 0          # idx = reg - delta (mod 2^32)
    cur = before[-1]
    after = None
    for _ in range(32):
        prev = _prev_fall(cur, by_addr)
        if prev is None:
            # Início de bloco: procure o único desvio condicional de guarda que salta para cá.
            jumps = [x for x in by_addr.values() if x.name == "b" and x.cs.operands
                     and x.cs.operands[-1].type == A.ARM_OP_IMM
                     and x.cs.operands[-1].imm & 0xFFFFFFFF == cur.addr and x.addr != cur.addr]
            if len(jumps) != 1:
                return None
            j = jumps[0]
            if j.cs.cc == COND_AL:
                after, cur = j, j
                continue
            if j.cs.cc not in _GUARD_TAKEN:
                return None
            c = _prev_fall(j, by_addr)
            return _cmp_bound(c, reg, delta, _GUARD_TAKEN[j.cs.cc], by_addr) if c else None
        i = prev
        if i.name == "cmp":
            if after is not None and after.name == "b" and after.cs.cc in _GUARD_FALL:
                n = _cmp_bound(i, reg, delta, _GUARD_FALL[after.cs.cc], by_addr)
                if n is not None:
                    return n
            # cmp que não é guarda de limite superior: segue procurando atrás dele
        elif _writes_reg(i, reg):
            src = _affine_src(i, by_addr)
            if src is None:
                return None   # índice recalculado antes da guarda: o limite não vale
            reg, delta = src[0], (delta + src[1]) & 0xFFFFFFFF
        after, cur = i, i
    return None


def _table_by_layout(img: GuestImage, base: int, n: str, limit: int = 1024) -> list[int]:
    """Tabela de tbb/tbh sem guarda: o compilador a põe logo após a instrução, e o código dos
    casos começa depois dela, então a tabela acaba onde começa o menor alvo."""
    size = 1 if n == "tbb" else 2
    targets: list[int] = []
    lowest = None
    k = 0
    while k < limit and (lowest is None or base + k * size < lowest):
        off = img.u8(base + k) if n == "tbb" else img.u16(base + 2 * k)
        t = base + 2 * off
        # Alvo dentro da própria tabela = byte de preenchimento (tbb com nº ímpar de casos) ou fim.
        if not img.is_code(t | 1) or t < base + (k + 1) * size:
            break
        targets.append(t)
        lowest = t if lowest is None else min(lowest, t)
        k += 1
    return targets


@dataclass
class Program:
    img: GuestImage
    functions: dict[int, Function]   # key: entry | thumb
    warnings: list[str]


def analyze(img: GuestImage, max_funcs: int = 200000) -> Program:
    """Duas passadas: a primeira descobre todas as entradas (alvos de BL, ponteiros); a segunda
    explora cada função já sabendo onde começam as outras, para que o fluxo sequencial pare na
    fronteira em vez de absorver a função seguinte. Ambas compartilham o cache do decodificador."""
    dec = Decoder(img)
    first = _analyze_pass(img, max_funcs, dec, None)
    seed = {k: f.name for k, f in first.functions.items()}
    return _analyze_pass(img, max_funcs, dec, seed)


def _analyze_pass(img: GuestImage, max_funcs: int, dec: "Decoder", seed: dict[int, str] | None) -> Program:
    entries: dict[int, str] = dict(seed or {})

    def add_entry(a: int, name: str | None = None):
        if not img.is_code(a) or (a & ~1) in img.imports:
            return
        if a not in entries:
            entries[a] = name or f"sub_{a & ~1:08x}{'_t' if a & 1 else '_a'}"
        elif name and entries[a].startswith("sub_"):
            entries[a] = name

    add_entry(img.entry, "_start")
    for a, n in img.symbols.items():
        add_entry(a, n)
    for a in img.code_ptrs:
        add_entry(a)

    functions: dict[int, Function] = {}
    warnings: list[str] = []
    pending = sorted(entries)
    while pending:
        key = pending.pop()
        if key in functions:
            continue
        if len(functions) >= max_funcs:
            warnings.append("function limit reached")
            break
        thumb = bool(key & 1)
        f = Function(key & ~1, thumb, _cname(entries[key]))
        functions[key] = f
        work = [f.entry]
        order: list[Insn] = []
        while work:
            a = work.pop()
            while a not in f.insns:
                if a != f.entry and (a | (1 if thumb else 0)) in entries:
                    # Fluxo sequencial chegou à entrada de outra função (p. ex. depois de um bl para
                    # função que não retorna): é fronteira, não continuação. Sem isto cada função
                    # absorvia todas as seguintes (duplicação quadrática no gow2).
                    break
                if not img.is_code(a | (1 if thumb else 0)):
                    warnings.append(f"{f.name}: flow leaves code at {a:#x}")
                    break
                i = dec.at(a, thumb)
                if i is None:
                    f.unknown.append(a)
                    break
                f.insns[a] = i
                order.append(i)
                n, cc = i.name, i.cs.cc
                ops = i.cs.operands
                if n in ("bl", "blx") and ops and ops[0].type == A.ARM_OP_IMM:
                    tgt = ops[0].imm & 0xFFFFFFFF
                    tthumb = thumb if n == "bl" else not thumb
                    tkey = tgt | (1 if tthumb else 0)
                    if (tgt & ~1) not in img.imports:
                        add_entry(tkey)
                        if tkey in entries and tkey not in functions:
                            pending.append(tkey)
                        elif tkey not in entries:
                            warnings.append(f"{f.name}: {n} at {i.addr:#x} targets non-code {tgt:#x}")
                    a = i.next
                    continue
                if n in ("b", "cbz", "cbnz"):
                    tgt = ops[-1].imm & 0xFFFFFFFF
                    tkey = tgt | (1 if thumb else 0)
                    if tkey in entries and tgt != f.entry:
                        pass  # tail call / branch to another function
                    else:
                        f.labels.add(tgt)
                        work.append(tgt)
                    if n == "b" and cc == COND_AL and not _in_it(i):
                        break
                    a = i.next
                    continue
                if n in ("tbb", "tbh"):
                    mem = ops[0].mem
                    idx = reg_num(i.cs, mem.index)
                    before = sorted((x for x in f.insns.values() if x.addr < i.addr), key=lambda x: x.addr)
                    count = _switch_bound(before + [i], idx, f.insns)
                    base = i.next  # PC for Thumb reads as addr+4; tbb/tbh are 4 bytes
                    if count is None:
                        targets = _table_by_layout(img, base, n)
                        if not targets:
                            warnings.append(f"{f.name}: unbounded {n} at {i.addr:#x}")
                            f.indirect_jumps.append(i.addr)
                            break
                        warnings.append(f"{f.name}: {n} at {i.addr:#x} bounded by table layout "
                                        f"({len(targets)} entries)")
                    else:
                        targets = []
                        for k in range(count):
                            off = img.u8(base + k) if n == "tbb" else img.u16(base + 2 * k)
                            targets.append(base + 2 * off)
                    f.tables[i.addr] = targets
                    for t in targets:
                        f.labels.add(t)
                        work.append(t)
                    break
                if _is_return(i):
                    if cc == COND_AL and not _in_it(i):
                        break
                    a = i.next
                    continue
                if writes_pc(i) or n == "bx":
                    f.indirect_jumps.append(i.addr)
                    if cc == COND_AL and not _in_it(i):
                        break
                    a = i.next
                    continue
                if n in ("udf", "bkpt"):
                    break
                a = i.next
        # Computed jumps that we could not resolve: sweep the whole function body
        # linearly so every instruction becomes a possible landing site.
        # `bx rX` costuma ser retorno/chamada por ponteiro, MAS é salto interno quando rX vem do próprio
        # pc (`add ip, pc, #k; sub ip, ip, r3, lsl #2; bx ip` na __udivsi3 do compiler-rt).
        pc_regs = {reg_num(x.cs, x.cs.operands[0].reg) for x in f.insns.values()
                   if x.name in ("add", "sub", "adr") and x.cs.operands and x.cs.operands[0].type == A.ARM_OP_REG
                   and (x.name == "adr" or any(o.type == A.ARM_OP_REG and reg_num(x.cs, o.reg) == 15
                                               for o in x.cs.operands[1:]))}
        def _internal(j: Insn) -> bool:
            if _is_return(j):
                return False
            if j.name == "bx":
                return bool(j.cs.operands) and reg_num(j.cs, j.cs.operands[0].reg) in pc_regs
            return True
        if any(_internal(f.insns[a]) for a in f.indirect_jumps if a in f.insns):
            end = img.sizes.get(key)
            end = f.entry + end if end else min((k & ~1 for k in entries if (k & ~1) > f.entry), default=None)
            if end is None:
                warnings.append(f"{f.name}: computed jump without a known function end")
            else:
                a = f.entry
                while a < end:
                    i = f.insns.get(a) or dec.at(a, thumb)
                    if i is None:
                        a += 2 if thumb else 4
                        continue
                    f.insns[a] = i
                    f.labels.add(a)
                    a = i.next
                f.swept = True
        # Indirect jumps may land on any code pointer inside this function.
        if f.indirect_jumps:
            for p in img.code_ptrs:
                if (p & 1) == (1 if thumb else 0) and (p & ~1) in f.insns:
                    f.labels.add(p & ~1)
    # Second chance: entries found late may have been absorbed as labels earlier; harmless
    # (code is duplicated into each function) but recorded for the report.
    return Program(img, functions, warnings)


def _in_it(i: Insn) -> bool:
    return i.thumb and i.cs.cc != COND_AL


def _cname(name: str) -> str:
    out = "".join(c if c.isalnum() or c == "_" else "_" for c in name)
    return out[:60] or "anon"
