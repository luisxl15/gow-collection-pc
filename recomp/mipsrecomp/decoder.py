"""MIPS32 Release 2 (+ COP1/COP1X) instruction decoder.

Decoding is table-free bit slicing; every instruction the recompiler can
translate has a mnemonic here. Anything else decodes to ``op = None`` so the
recompiler can fail closed with an exact address.
"""
from __future__ import annotations

from dataclasses import dataclass

# Control-flow classes
BRANCH = "branch"          # conditional/unconditional PC-relative, has delay slot
BRANCH_LIKELY = "likely"   # delay slot annulled when not taken
BRANCH_LINK = "blink"      # PC-relative call (bal, bgezal, bltzal)
JUMP = "jump"              # j
CALL = "call"              # jal
JUMP_REG = "jr"            # jr
CALL_REG = "jalr"          # jalr

FMT_S, FMT_D, FMT_W, FMT_L, FMT_PS = 16, 17, 20, 21, 22


@dataclass
class Insn:
    addr: int
    word: int
    op: str | None
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0      # zero-extended 16-bit immediate
    simm: int = 0     # sign-extended 16-bit immediate
    target: int = 0   # absolute target for branches / jumps
    fmt: int = 0
    ft: int = 0
    fs: int = 0
    fd: int = 0
    fr: int = 0
    cc: int = 0
    cond: int = 0
    flow: str | None = None

    @property
    def has_delay_slot(self) -> bool:
        return self.flow is not None

    def __str__(self) -> str:
        return f"{self.addr:08x}: {self.word:08x} {self.op or '<unknown>'}"


SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall", 0x0D: "break",
    0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x18: "mult",
    0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub",
    0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
    0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu", 0x34: "teq", 0x36: "tne",
}
REGIMM = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x08: "tgei", 0x09: "tgeiu",
    0x0A: "tlti", 0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei", 0x10: "bltzal", 0x11: "bgezal",
    0x12: "bltzall", 0x13: "bgezall",
}
PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi",
    0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori",
    0x0F: "lui", 0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl", 0x20: "lb",
    0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x28: "sb",
    0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x2F: "cache", 0x30: "ll", 0x31: "lwc1",
    0x33: "pref", 0x35: "ldc1", 0x38: "sc", 0x39: "swc1", 0x3D: "sdc1",
}
SPECIAL2 = {0x00: "madd", 0x01: "maddu", 0x02: "mul", 0x04: "msub", 0x05: "msubu",
            0x20: "clz", 0x21: "clo"}
COP1_ARITH = {
    0x00: "add", 0x01: "sub", 0x02: "mul", 0x03: "div", 0x04: "sqrt", 0x05: "abs", 0x06: "mov",
    0x07: "neg", 0x0C: "round.w", 0x0D: "trunc.w", 0x0E: "ceil.w", 0x0F: "floor.w",
    0x11: "movcf", 0x12: "movz", 0x13: "movn", 0x20: "cvt.s", 0x21: "cvt.d", 0x24: "cvt.w",
}
COP1X = {0x00: "lwxc1", 0x01: "ldxc1", 0x08: "swxc1", 0x09: "sdxc1", 0x0F: "prefx"}
COP1X_ARITH = {4: "madd", 5: "msub", 6: "nmadd", 7: "nmsub"}

FLOW = {
    "beq": BRANCH, "bne": BRANCH, "blez": BRANCH, "bgtz": BRANCH, "bltz": BRANCH, "bgez": BRANCH,
    "beql": BRANCH_LIKELY, "bnel": BRANCH_LIKELY, "blezl": BRANCH_LIKELY,
    "bgtzl": BRANCH_LIKELY, "bltzl": BRANCH_LIKELY, "bgezl": BRANCH_LIKELY,
    "bltzal": BRANCH_LINK, "bgezal": BRANCH_LINK,
    "bltzall": BRANCH_LINK, "bgezall": BRANCH_LINK,
    "bc1f": BRANCH, "bc1t": BRANCH, "bc1fl": BRANCH_LIKELY, "bc1tl": BRANCH_LIKELY,
    "j": JUMP, "jal": CALL, "jr": JUMP_REG, "jalr": CALL_REG,
}


def decode(addr: int, word: int) -> Insn:
    opc = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    imm = word & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    i = Insn(addr, word, None, rs=rs, rt=rt, rd=rd, sa=sa, imm=imm, simm=simm)
    branch_target = (addr + 4 + (simm << 2)) & 0xFFFFFFFF

    if opc == 0x00:
        op = SPECIAL.get(funct)
        if op == "srl" and rs == 1:
            op = "rotr"
        elif op == "srlv" and sa == 1:
            op = "rotrv"
        elif funct == 0x01:
            op = "movt" if rt & 1 else "movf"
            i.cc = rt >> 2
        i.op = op
    elif opc == 0x01:
        i.op = REGIMM.get(rt)
        i.target = branch_target
    elif opc in (0x02, 0x03):
        i.op = PRIMARY[opc]
        i.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    elif opc == 0x1C:
        i.op = SPECIAL2.get(funct)
        if funct == 0x3F:
            i.op = "sdbbp"
    elif opc == 0x1F:  # SPECIAL3
        if funct == 0x00:
            i.op = "ext"
        elif funct == 0x04:
            i.op = "ins"
        elif funct == 0x20:
            i.op = {0x02: "wsbh", 0x10: "seb", 0x18: "seh"}.get(sa)
        elif funct == 0x3B:
            i.op = "rdhwr"
    elif opc == 0x11:
        _decode_cop1(i, word, branch_target)
    elif opc == 0x13:
        _decode_cop1x(i, word)
    else:
        i.op = PRIMARY.get(opc)
        if opc in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
            i.target = branch_target

    if i.op in FLOW:
        i.flow = FLOW[i.op]
    return i


def _decode_cop1(i: Insn, word: int, branch_target: int) -> None:
    fmt = (word >> 21) & 31
    i.fmt = fmt
    i.ft = (word >> 16) & 31
    i.fs = (word >> 11) & 31
    i.fd = (word >> 6) & 31
    funct = word & 63
    if fmt in (0x00, 0x02, 0x03, 0x04, 0x06, 0x07):
        i.op = {0x00: "mfc1", 0x02: "cfc1", 0x03: "mfhc1", 0x04: "mtc1", 0x06: "ctc1",
                0x07: "mthc1"}[fmt]
    elif fmt == 0x08:
        nd_tf = (word >> 16) & 3
        i.cc = (word >> 18) & 7
        i.op = ("bc1f", "bc1t", "bc1fl", "bc1tl")[nd_tf]
        i.target = branch_target
    elif fmt in (FMT_S, FMT_D, FMT_W, FMT_L):
        if funct >= 0x30:
            if fmt in (FMT_S, FMT_D):
                i.op = "c"
                i.cond = funct & 15
                i.cc = (word >> 8) & 7
        else:
            op = COP1_ARITH.get(funct)
            if op == "movcf":
                op = "movt" if i.ft & 1 else "movf"
                i.cc = i.ft >> 2
                op = op + ".fmt"
            elif op in ("movz", "movn"):
                op = op + ".fmt"
            if fmt in (FMT_W, FMT_L) and op not in ("cvt.s", "cvt.d"):
                op = None
            if fmt == FMT_L:  # 64-bit integer FPU formats need FR=1; unsupported
                op = None
            i.op = op


def _decode_cop1x(i: Insn, word: int) -> None:
    funct = word & 63
    i.fr = (word >> 21) & 31   # also 'base' for indexed loads
    i.ft = (word >> 16) & 31   # also 'index'
    i.fs = (word >> 11) & 31
    i.fd = (word >> 6) & 31
    if funct in COP1X:
        i.op = COP1X[funct]
    elif funct >> 3 in COP1X_ARITH and funct & 7 in (0, 1):
        i.op = COP1X_ARITH[funct >> 3] + ".fmt"
        i.fmt = FMT_S if funct & 7 == 0 else FMT_D
