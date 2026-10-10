"""Desmontagem legível de USSE: rebaixa os campos crus de cada instrução para operandos
(operands.py). Cobre as classes aritméticas F32 mais frequentes; as demais saem com os campos
crus. É o mesmo rebaixamento que o interpretador e o emissor SPIR-V vão consumir.
"""
from . import operands as O

V32_OPS = ["vmul", "vadd", "vfrc", "vdsx", "vdsy", "vmin", "vmax", "vdp"]


def mask_text(m):
    return "".join("xyzw"[i] for i in range(4) if m >> i & 1)


def lower(ins, second=False):
    """Devolve (mnemônico, destino, máscara, [fontes]) ou None se a classe ainda não é rebaixada."""
    f = ins.f
    if ins.op == "vmad":
        kind = 2 if f["opcode2"] else 1
        d = O.dest(f["dest_n"], f["dest_bank"], f["dest_use_bank_ext"], second=second)
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], second=second)
        s1.swizzle = O.vec34_swizzle(f["src1_swiz"], f["src1_swiz_ext"], kind)
        s1.neg, s1.abs = bool(f["src1_neg"]), bool(f["src1_abs"])
        g0 = O.gpi(f["gpi0_n"], O.vec34_swizzle(f["gpi0_swiz"], f["gpi0_swiz_ext"], kind))
        g0.neg, g0.abs = bool(f["gpi0_neg"]), bool(f["gpi0_abs"])
        g1 = O.gpi(f["gpi1_n"], O.vec34_swizzle(f["gpi1_swiz"], f["gpi1_swiz_ext"], kind))
        g1.neg, g1.abs = bool(f["gpi1_neg"]), bool(f["gpi1_abs"])
        # fma(gpi0, src1, gpi1)
        return ("vmad%d" % (kind + 2), d, f["write_mask"], [g0, s1, g1])
    if ins.op == "vdp":
        kind = 2 if f["opcode2"] else 1
        d = O.dest(f["dest_n"], f["dest_bank"], f["dest_use_bank_ext"], second=second)
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], second=second)
        tb = [O.X, O.Y, O.Z, O.W, O.C0, O.C1, O.C2, O.CH]
        s1.swizzle = tuple(tb[f[k]] for k in ("src1_swiz_x", "src1_swiz_y", "src1_swiz_z", "src1_swiz_w"))
        s1.neg, s1.abs = bool(f["src1_neg"]), bool(f["src1_abs"])
        g0 = O.gpi(f["gpi0_n"], O.vec34_swizzle(f["gpi0_swiz"], False, kind))
        g0.abs = bool(f["gpi0_abs"])
        return ("vdp%d" % (kind + 2), d, f["write_mask"], [s1, g0])
    if ins.op in ("v32nmad", "v16nmad"):
        is32 = bool(ins.raw >> 59 & 1)
        d = O.dest(f["dest_n"], f["dest_bank_sel"], f["dest_bank_ext"], second=second)
        s1 = O.modifier(O.src12(f["src1_n"], f["src1_bank_sel"], f["src1_bank_ext"], second=second), f["src1_mod"])
        s1.swizzle = O.swizzle4(f["src1_swiz_0_6"] | f["src1_swiz_7_8"] << 7 | f["src1_swiz_9"] << 9 | f["src1_swiz_10_11"] << 10)
        s2 = O.src12(f["src2_n"], f["src2_bank_sel"], f["src2_bank_ext"], second=second)
        s2.swizzle = O.vec34_swizzle(f["src2_swiz"], False, 2)
        s2.abs = f["src2_mod"] == 1
        return (V32_OPS[f["op2"]] + ("" if is32 else ".f16"), d, f["dest_mask"], [s1, s2])
    return None


def text(ins, second=False):
    low = lower(ins, second)
    if low is None:
        return "%04d %-8s %s" % (ins.pc, ins.op, " ".join("%s=%d" % kv for kv in sorted(ins.f.items())))
    mn, d, m, srcs = low
    smask = 0xF if mn.startswith("vdp") else m
    return "%04d %-8s %s.%s = %s" % (ins.pc, mn, d.text(0xF).split(".")[0], mask_text(m),
                                    ", ".join(s.text(smask) for s in srcs))
