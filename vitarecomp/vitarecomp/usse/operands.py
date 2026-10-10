"""Operandos do USSE: bancos de registradores, numeração e swizzles.

Regras (as mesmas para o interpretador e para o emissor SPIR-V):
  * registradores de 32 bits; instruções F32 vetoriais usam "registradores duplos": o número
    codificado é multiplicado por 2 (exceto nos bancos SPECIAL e IMMEDIATE);
  * TEMP com número >= 2^bits - (8 se duplo, senão 4) é FPINTERNAL (GPI), número
    (n - limite) / (2 se duplo); cada FPINTERNAL é um vec4 (4 palavras);
  * SPECIAL com o bit 0x40 é GLOBAL; sem ele, FPCONSTANT (tabela de constantes do hardware);
  * no programa secundário, todo banco que não seja FPINTERNAL/FPCONSTANT/IMMEDIATE é SA;
  * leitura: componente c = banco[num + swizzle[c]] (canais 0, 1, 2 e 0,5 são constantes);
  * escrita: os resultados compactados vão para banco[num + i] em cada bit i da máscara.
"""
from dataclasses import dataclass, field

TEMP, PA, OUT, SA, FPI, SPECIAL, GLOBAL, FPCONST, IMM, INDEX, IDX1, IDX2, PRED = (
    "t", "pa", "o", "sa", "fpi", "spec", "g", "fpc", "imm", "idx", "ix1", "ix2", "p")

X, Y, Z, W, C0, C1, C2, CH, UND = range(9)
CH_NAME = "xyzw012h?"


def _sw(s):
    return tuple({"x": X, "y": Y, "z": Z, "w": W, "0": C0, "1": C1, "2": C2, "h": CH, "?": UND}[c] for c in s)


V4_STD = [_sw(s) for s in "xxxx yyyy zzzz wwww xyzw yzww xyzz xxyz xyxy xywz zxyw zwzw yzxz xxyy xzww xyz1".split()]
V4_EXT = [_sw(s) for s in "yzxw zwxy xzwy yyww wyzw wzwz xyzx zzww xwzx yyyx yyyz xzyw xxxy zyxw yyzz zzzy".split()]
V3_STD = [_sw(s + "x") for s in "xxx yyy zzz www xyz yzw xxy xyx yyx yyz zxy xzy yzx zyx zzy xy1".split()]
V3_EXT = [_sw(s + "x") for s in "xyy yxy xxz yxx xy0 x10 000 111 hhh 222 x00 ??? ??? ??? ??? ???".split()]
SCALAR = [_sw(s) for s in "xxxx yyyy zzzz wwww 0000 1111 2222 hhhh".split()] + [_sw("????")] * 8
# Swizzles da instrução dual (vdual), que diferem nas posições 12-15.
DUAL_V4_STD = V4_STD[:12] + [_sw(s) for s in "0000 hhhh 1111 2222".split()]
DUAL_V4_EXT = V4_EXT[:11] + [_sw(s) for s in "zwzw yzxz xxyy xzww xyz1".split()]
DUAL_V3_STD = [_sw(s + "x") for s in "xxx yyy zzz www xyz yzw xxy xyx yyx yyz zxy xzy 000 hhh 111 222".split()]
DUAL_V3_EXT = [_sw(s + "x") for s in "xyy yxy xxz yxx xy0 x10 xzy yzx zyx zzy xy1 ??? ??? ??? ??? ???".split()]


def vec34_swizzle(code, ext, kind):
    """kind: 0 escalar, 1 vec3, 2 vec4."""
    if kind == 0:
        return _sw("????") if ext else SCALAR[code]
    if kind == 1:
        return (V3_EXT if ext else V3_STD)[code]
    return (V4_EXT if ext else V4_STD)[code]


def swizzle4(enc):
    """Swizzle de 12 bits, 3 bits por canal."""
    return tuple((enc >> (3 * i)) & 7 for i in range(4))


@dataclass
class Operand:
    bank: str
    num: int
    swizzle: tuple = (X, Y, Z, W)
    neg: bool = False
    abs: bool = False
    gpi: bool = False

    def text(self, mask=0xF):
        sw = "".join(CH_NAME[self.swizzle[i]] for i in range(4) if mask >> i & 1)
        s = "%s%d" % (self.bank, self.num)
        if sw and sw != "xyzw"[:len(sw)]:
            s += "." + sw
        if self.abs:
            s = "|%s|" % s
        return ("-" if self.neg else "") + s


def _finalize(op, double, bits, second):
    temps = 8 if double else 4
    limit = (1 << bits) - temps
    if op.bank == TEMP and op.num >= limit:
        op.num -= limit
        if double:
            op.num >>= 1
        op.bank = FPI
    if op.bank == SPECIAL:
        if op.num & 0x40:
            op.num &= ~0x40
            op.bank = GLOBAL
        else:
            op.bank = FPCONST
    if second and op.bank not in (FPI, FPCONST, IMM):
        op.bank = SA
    return op


def _double(num, bank):
    return (num << 1) & 0xFF if bank not in (SPECIAL, IMM) else num


def dest(n, bank_sel, ext, double=True, bits=7, second=False):
    bank = ([SA, SPECIAL, INDEX, IDX2] if ext else [TEMP, OUT, PA, IDX1])[bank_sel]
    op = Operand(bank, _double(n, bank) if double else n)
    return _finalize(op, double, bits, second)


def src0(n, bank_sel, ext, double=True, bits=7, second=False):
    bank = ([OUT, SA] if ext else [TEMP, PA])[bank_sel]
    op = Operand(bank, _double(n, bank) if double else n)
    return _finalize(op, double, bits, second)


def src12(n, bank_sel, ext, double=True, bits=7, second=False):
    bank = ([IDX1, SPECIAL, IMM, IDX2] if ext else [TEMP, OUT, PA, SA])[bank_sel]
    op = Operand(bank, _double(n, bank) if double else n)
    return _finalize(op, double, bits, second)


def gpi(n, swizzle):
    return Operand(FPI, n, swizzle, gpi=True)


def modifier(op, mod):
    op.neg, op.abs = bool(mod & 1), bool(mod & 2)
    return op


def write_mask(bank, mask, f16):
    """Máscara de escrita das instruções de 64 bits (vmad2, vmov): F32 fora de FPINTERNAL escreve
    2 componentes; F16 expande cada bit par para um par de metades."""
    if bank == FPI or (f16 and bank not in (PA, SA, OUT, FPI, TEMP)):
        return mask
    if f16:
        return (0b11 if mask & 1 else 0) | (0b1100 if mask & 4 else 0)
    return mask & 0b11


VMAD2_SRC0 = [_sw(s) for s in "xxxx yyyy zzzz wwww xyzw yzxw xyww zwxy".split()]
VMAD2_SRC1 = [_sw(s) for s in "xxxx yyyy zzzz wwww xyzw xyyz yyww wyzw".split()]
VMAD2_SRC2 = [_sw(s) for s in "xxxx yyyy zzzz wwww xyzw xzww xxyz xyzz".split()]


# Predicado de execução: (número do registrador P, negado) ou None. "PN" (por componente) e NEGP2
# ainda não são suportados e levantam ValueError.
_EXT = {0: None, 1: (0, False), 2: (1, False), 3: (2, False), 4: (3, False), 5: (0, True), 6: (1, True)}
_EXT_VEC = {0: None, 1: (0, False), 2: (1, False), 3: (2, False), 4: (0, True), 5: (1, True)}
_SHORT = {0: None, 1: (0, False), 2: (1, False), 3: (0, True)}


def predicate(ins):
    v = ins.f.get("pred", 0)
    if ins.op == "vmad2":
        table = {0: None, 1: (0, False), 2: (1, False), 3: (2, False)}
    elif ins.op == "kill":
        # o inverso do predicado curto: 1 = !P0, 2 = !P1, 3 = P0
        table = {0: None, 1: (0, True), 2: (1, True), 3: (0, False)}
    elif ins.op in ("v32nmad", "v16nmad", "vmad", "vdp"):
        table = _EXT_VEC
    elif ins.op in ("sop2", "sop2m", "sop3", "i8mad", "i16mad", "i32mad"):
        table = _SHORT
    else:
        table = _EXT
    if v not in table:
        raise ValueError("predicado %d não suportado em %s" % (v, ins.op))
    return table[v]
