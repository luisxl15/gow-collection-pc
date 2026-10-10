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
    if ins.op == "vcomp":
        d = O.dest(f["dest_n"], f["dest_bank"], f["dest_bank_ext"], bits=8, second=second)
        s1 = O.modifier(O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], bits=8, second=second), f["src1_mod"])
        c = f["src_comp"]
        s1.swizzle = (c, c, c, c)  # um componente, replicado na máscara de destino
        if f["src_type"] != 0 or f["dest_type"] != 0:
            return None  # só F32 por enquanto
        return (["vrcp", "vrsq", "vlog", "vexp"][f["op2"]], d, f["write_mask"], [s1])
    if ins.op == "vmad2":
        f16 = bool(f["dat_fmt"])
        d = O.dest(f["dest_n"], f["dest_bank"], 0, second=second)
        s0 = O.src0(f["src0_n"], f["src0_bank"], 0, second=second)
        s1 = O.modifier(O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], second=second), f["src1_mod"])
        s2 = O.modifier(O.src12(f["src2_n"], f["src2_bank"], f["src2_bank_ext"], second=second), f["src2_mod"])
        s0.swizzle = O.VMAD2_SRC0[f["src0_swiz_bits01"] | f["src0_swiz_bits2"] << 2]
        s1.swizzle = O.VMAD2_SRC1[f["src1_swiz_bits01"] | f["src1_swiz_bit2"] << 2]
        s2.swizzle = O.VMAD2_SRC2[f["src2_swiz"]]
        s0.abs = bool(f["src0_abs"])
        return ("vmad" + (".f16" if f16 else ""), d, O.write_mask(d.bank, f["dest_mask"], f16), [s0, s1, s2])
    if ins.op == "vmov":
        # move_data_type: 0 i8, 1 i16, 2 i32, 3 c10, 4 f16, 5 f32, 6 u8, 7 u16, 8 u32
        t = f["move_data_type"]
        if t not in (2, 4, 5, 8) or f["move_type"] == 2 or f["repeat_count"]:
            return None
        double = t in (4, 5)
        bits = 7 if double else 6
        d = O.dest(f["dest_n"], f["dest_bank_sel"], f["dest_bank_ext"], double, bits, second)
        s1 = O.src12(f["src1_n"], f["src1_bank_sel"], f["src1_bank_ext"], double, bits, second)
        mask = O.write_mask(d.bank, f["dest_mask"], t == 4) if double else 1
        if double:
            s1.swizzle = O.vec34_swizzle(f["src0_swiz"], False, 2)
        suffix = ".f16" if t == 4 else ""
        if f["move_type"] == 0:
            return ("vmov" + suffix, d, mask, [s1])
        if t not in (4, 5):
            return None  # comparação inteira: ainda não
        s0 = O.src0(f["src0_n"], f["src0_bank_sel"], f["end_or_src0_bank_ext"], double, bits, second)
        s2 = O.src12(f["src2_n"], f["src2_bank_sel"], f["src2_bank_ext"], double, bits, second)
        if f["src0_comp_sel"]:
            s0.swizzle = s1.swizzle
        s2.swizzle = s1.swizzle
        cmp = ["eq", "ne", "lt", "le"][f["test_bit_2"] << 1 | f["test_bit_1"]]
        # resultado = src1 se (src0 cmp 0) senão src2
        return ("vmovc_" + cmp + suffix, d, mask, [s0, s1, s2])
    if ins.op == "vtst":
        # Teste: compara (src1 - src2) ou alu(src1, src2) com zero, num canal; grava o predicado.
        if f["alu_sel"] != 0 or f["chan_cc"] >= 4 or f["rpt_count"]:
            return None
        alu = {2: "add", 9: "min", 10: "max", 13: "mul", 14: "sub"}.get(f["alu_op"])
        if alu is None:
            return None
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_ext"], True, 8, second)
        s2 = O.src12(f["src2_n"], f["src2_bank"], f["src2_ext"], True, 8, second)
        s1.neg = bool(f["src1_neg"])
        if f["src2_vscomp"]:
            s2.swizzle = (O.X, O.X, O.X, O.X)
        eq = f["zero_test"] == 1
        if f["sign_test"] == 3:
            return None
        cmp = [["ne", "lt", "gt"], ["eq", "le", "ge"]][eq][f["sign_test"]]
        d = O.Operand(O.PRED, f["pdst_n"])
        return ("vtst_%s_%s%s" % (alu, cmp, "" if f["prec"] else ".f16"), d, 1 << f["chan_cc"], [s1, s2])
    if ins.op == "kill":
        return ("kill", O.Operand(O.PRED, 0), 0, [])
    if ins.op in ("nop", "phas"):
        return (ins.op, None, 0, [])
    return None


def text(ins, second=False):
    low = lower(ins, second)
    if low is None:
        return "%04d %-8s %s" % (ins.pc, ins.op, " ".join("%s=%d" % kv for kv in sorted(ins.f.items())))
    mn, d, m, srcs = low
    if d is None:
        return "%04d %s" % (ins.pc, mn)
    smask = 0xF if mn.startswith("vdp") else m
    return "%04d %-8s %s.%s = %s" % (ins.pc, mn, d.text(0xF).split(".")[0], mask_text(m),
                                    ", ".join(s.text(smask) for s in srcs))
