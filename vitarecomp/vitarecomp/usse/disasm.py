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
    if ins.op in ("vmad", "vdp", "vcomp") and f.get("repeat_count"):
        return None  # repetição ainda não modelada (resultado seria válido mas errado)
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
    if ins.op == "vpck":
        # Só formatos de ponto flutuante (f32/f16) por enquanto; cada componente vira uma fonte de
        # canal único em srcs (a escolha entre src1/src2 já resolvida).
        fmts = {5: "f16", 6: "f32"}
        if f["src_fmt"] == 6 and f["dest_fmt"] == 3:
            fmts = {6: "f32", 3: "u16"}  # f32 -> u16 (conversão sem escala)
            if f["scale"]:
                return None
        if f["src_fmt"] not in fmts or f["dest_fmt"] not in fmts or f["repeat_count"]:
            return None
        sf, df = fmts[f["src_fmt"]], fmts[f["dest_fmt"]]
        d = O.dest(f["dest_n"], f["dest_bank_sel"], f["dest_bank_ext"], False, 7, second)
        s1 = O.src12(f["src1_n"], f["src1_bank_sel"], f["src1_bank_ext"], True, 7, second)
        s2 = O.src12(f["src2_n"], f["src2_bank_sel"], f["src2_bank_ext"], True, 7, second)
        if sf == "f32":
            c0 = f["comp_sel_0_bit0"] | (f["comp0_sel_bit1"] & 1) << 1
        else:
            c0 = f["comp_sel_0_bit0"] | (f["src2_n"] & 1) << 1
        sw = (c0, f["comp_sel_1"], f["comp_sel_2"], f["comp_sel_3"])
        mask = f["dest_mask"]
        n = bin(mask).count("1")
        use_src2 = False
        if sf == "f32" and s2.bank != O.IMM:
            same = s1.num == s2.num and s1.bank == s2.bank
            contiguous = mask in (1, 3, 7, 15)
            use_src2 = sw[:n] != (0, 1, 2, 3)[:n] or not same or not contiguous
        picks = []
        for i in range(4):
            if mask >> i & 1:
                ch = sw[i]
                if use_src2:
                    base, ch = (s1, ch) if ch < 2 else (s2, ch - 2)
                else:
                    base = s1
                picks.append(O.Operand(base.bank, base.num, (ch, ch, ch, ch)))
        return ("vpck_%s_%s" % (sf, df), d, mask, picks)
    if ins.op == "vbw":
        # Operações de bits em uma palavra de 32 bits (bitwise_partial = 16 bits: ainda não).
        op = {(2, 0): "and", (2, 1): "or", (3, 0): "xor", (3, 1): "xor", (4, 0): "shl", (5, 0): "shr", (5, 1): "asr"}.get(
            (f["op1"], f["op2"]))
        if op is None or f["bitwise_partial"] or f["repeat_count"]:
            return None
        d = O.dest(f["dest_n"], f["dest_bank"], f["dest_ext"], False, 7, second)
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_ext"], False, 7, second)
        s2 = O.src12(f["src2_n"], f["src2_bank"], f["src2_ext"], False, 7, second)
        if f["src2_ext"] and s2.bank == O.IMM:
            v = f["src2_n"] | f["src2_sel"] << 7 | f["src2_exth"] << 14
            r = f["src2_rot"]
            if r:
                v = ((v << r) | (v >> (32 - r))) & 0xFFFFFFFF
            if f["src2_invert"]:
                v = ~v & 0xFFFFFFFF
            s2 = O.Operand(O.IMM, v)
        elif f["src2_rot"] or f["src2_invert"]:
            return None
        return ("vbw_" + op, d, 1, [s1, s2])
    if ins.op == "i32mad":
        # dest(32) = meia16(src0) * meia16(src1) + src2 (32 bits, ou 16 se src2_type != 2)
        if f["repeat_count"] or f["is_sat"]:
            return None
        d = O.dest(f["dest_n"], f["dest_bank"], f["dest_bank_ext"], False, 7, second)
        s0 = O.src0(f["src0_n"], f["src0_bank"], 0, False, 7, second)
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], False, 7, second)
        s2 = O.src12(f["src2_n"], f["src2_bank"], f["src2_bank_ext"], False, 7, second)
        s0.swizzle = (f["src0_high"],) * 4
        s1.swizzle = (f["src1_high"],) * 4
        s2.swizzle = (9 if f["src2_type"] == 2 else f["src2_high"],) * 4  # 9 = palavra inteira
        return ("i32mad_" + ("s" if f["is_signed"] else "u"), d, 1, [s0, s1, s2])
    if ins.op == "vldst":
        # Carga de memória: endereço = src0 + src1 + src2 (bytes); mask_count + 1 palavras F32.
        if f["op1"] != 1 or f["data_type"] != 0:
            return None
        s0 = O.src0(f["src0_n"], f["src0_bank"], f["src0_bank_ext"], False, 7, second)
        s1 = O.src12(f["src1_n"], f["src1_bank"], f["src1_bank_ext"], False, 7, second)
        s2 = O.src12(f["src2_n"], f["src2_bank"], f["src2_bank_ext"], False, 7, second)
        if s1.bank == O.IMM:
            s1.num *= 4
        bank = O.SA if second else (O.PA if f["dest_bank_primattr"] else O.TEMP)
        d = O.Operand(bank, f["dest_n"])
        return ("vldst", d, f["mask_count"] + 1, [s0, s1, s2])
    if ins.op == "kill":
        return ("kill", O.Operand(O.PRED, 0), 0, [])
    if ins.op in ("nop", "phas", "spec", "smlsi"):
        # spec: sincronização; smlsi: incrementos de repetição (recusamos instruções repetidas)
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
