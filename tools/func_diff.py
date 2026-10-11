"""Diferencial de uma função: o ARM original no Unicorn contra a tradução para C.

O runtime grava o estado na entrada e na saída de uma chamada (VITART_SNAP_DIR, marcas @SNAP-ENTRY e
@SNAP-EXIT emitidas por uma casca na função gerada). Este script carrega a entrada no Unicorn, roda a
função até voltar ao LR e compara, endereço a endereço, tudo o que o ARM escreveu com o que a tradução
deixou na memória, além dos registradores.

uso: func_diff.py <snap_dir> <endereço_da_função> [--trace]
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_WRITE, UC_HOOK_CODE, UcError
from unicorn.arm_const import *

CTX_FMT = "<16I4I32QI"   # r[16], n z c v, d[32], fpscr (início de arc_ctx)


def load_regions(d):
    regs = []
    for line in open(os.path.join(d, "regions.txt")):
        p = line.split()
        if len(p) >= 2:
            regs.append((int(p[0], 16), int(p[1], 16)))
    return regs


def page_ranges(regs):
    pages = sorted({a >> 12 for base, size in regs for a in range(base & ~0xFFF, base + size, 0x1000)})
    out, start, prev = [], None, None
    for p in pages:
        if start is None:
            start = prev = p
        elif p == prev + 1:
            prev = p
        else:
            out.append((start << 12, (prev - start + 1) << 12))
            start = prev = p
    if start is not None:
        out.append((start << 12, (prev - start + 1) << 12))
    return out


def read_mem_image(d, regs):
    img = {}
    for base, size in regs:
        f = os.path.join(d, "mem_%08x.bin" % base)
        if os.path.exists(f):
            img[base] = open(f, "rb").read()
    return img


def main():
    snap = sys.argv[1]
    func = int(sys.argv[2], 16)
    trace = "--trace" in sys.argv
    ent, ext = os.path.join(snap, "entry"), os.path.join(snap, "exit")
    regs = load_regions(ent)
    ctx = struct.unpack_from(CTX_FMT, open(os.path.join(ent, "ctx.bin"), "rb").read())
    ctx_out = struct.unpack_from(CTX_FMT, open(os.path.join(ext, "ctx.bin"), "rb").read())

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for base, size in page_ranges(regs):
        uc.mem_map(base, size)
    for base, data in read_mem_image(ent, regs).items():
        uc.mem_write(base, data)

    r, (n, z, c, v), dregs, fpscr = ctx[:16], ctx[16:20], ctx[20:52], ctx[52]
    for i in range(13):
        uc.reg_write(UC_ARM_REG_R0 + i, r[i])
    uc.reg_write(UC_ARM_REG_SP, r[13])
    uc.reg_write(UC_ARM_REG_LR, r[14])
    # VFP ligado: CPACR com acesso completo a cp10/cp11 e FPEXC.EN.
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    for i in range(32):
        uc.reg_write(UC_ARM_REG_D0 + i, dregs[i])
    uc.reg_write(UC_ARM_REG_FPSCR, fpscr)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR) & 0x0FFFFFFF
    uc.reg_write(UC_ARM_REG_CPSR, cpsr | (n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x20)

    written = {}

    def on_write(uc_, access, addr, size, value, _):
        for k in range(size):
            written[addr + k] = None

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    reads = {}
    if "--reads" in sys.argv:
        from unicorn import UC_HOOK_MEM_READ

        def on_read(uc_, access, addr, size, value, _):
            reads.setdefault(addr & ~0x3F, set()).add(addr & 0x3F)
        uc.hook_add(UC_HOOK_MEM_READ, on_read)
    if trace:
        def on_code(uc_, addr, size, _):
            print("  %08x" % addr)
        uc.hook_add(UC_HOOK_CODE, on_code)

    lr = r[14] & ~1
    try:
        uc.emu_start(func | 1, lr, count=50_000_000)
    except UcError as e:
        print("Unicorn parou em %08x: %s" % (uc.reg_read(UC_ARM_REG_PC), e))
        return 1
    pc = uc.reg_read(UC_ARM_REG_PC)
    print("Unicorn voltou: pc=%08x (esperado %08x), %d bytes escritos" % (pc, lr, len(written)))

    if reads:
        sp0 = r[13]
        for blk in sorted(reads):
            if sp0 - 0x8000 <= blk < sp0 + 0x100:
                continue
            f = struct.unpack("<16f", bytes(uc.mem_read(blk, 64)))
            print("LIDO %08x (%2d palavras): %s" % (blk, len(reads[blk]) // 4 or 1,
                  " ".join("%.4g" % x for x in f)))
    # registradores
    names = ["r%d" % i for i in range(13)] + ["sp"]
    for i, nm in enumerate(names):
        got = uc.reg_read(UC_ARM_REG_R0 + i) if i < 13 else uc.reg_read(UC_ARM_REG_SP)
        if got != ctx_out[i] and i not in (12,):
            print("  REG %s: arm=%08x tradução=%08x" % (nm, got, ctx_out[i]))
    for i in range(32):
        got = uc.reg_read(UC_ARM_REG_D0 + i)
        if got != ctx_out[20 + i]:
            print("  REG d%d: arm=%016x tradução=%016x" % (i, got, ctx_out[20 + i]))

    # memória: só os bytes que o ARM escreveu (a pilha abaixo do sp final fica de fora)
    out_img = read_mem_image(ext, load_regions(ext))
    bases = sorted(out_img)

    def out_byte(a):
        import bisect
        j = bisect.bisect_right(bases, a) - 1
        if j >= 0 and a < bases[j] + len(out_img[bases[j]]):
            return out_img[bases[j]][a - bases[j]]
        return None

    sp_end = uc.reg_read(UC_ARM_REG_SP)
    diffs = []
    in_img = read_mem_image(ent, regs)
    ibases = sorted(in_img)
    import bisect
    compared = changed = 0
    for a in sorted(written):
        j = bisect.bisect_right(ibases, a) - 1
        before = in_img[ibases[j]][a - ibases[j]] if j >= 0 and a < ibases[j] + len(in_img[ibases[j]]) else None
        if before is not None and before != uc.mem_read(a, 1)[0]:
            changed += 1
        if sp_end - 0x4000 <= a < sp_end:
            continue
        armb = uc.mem_read(a, 1)[0]
        tb = out_byte(a)
        if tb is not None:
            compared += 1
        if tb is not None and tb != armb:
            diffs.append(a)
    print("comparados: %d; mudados pelo ARM: %d; bytes diferentes: %d" % (compared, changed, len(diffs)))
    ranges = {}
    for a in written:
        ranges.setdefault(a >> 8, 0)
    print("faixas de 256 B escritas:", " ".join("%08x" % (k << 8) for k in sorted(ranges)[:40]))
    shown = set()
    for a in diffs:
        w = a & ~3
        if w in shown:
            continue
        shown.add(w)
        armw = struct.unpack("<I", bytes(uc.mem_read(w, 4)))[0]
        tw = bytes(out_byte(w + k) or 0 for k in range(4))
        tw = struct.unpack("<I", tw)[0]
        af, tf = struct.unpack("<f", struct.pack("<I", armw))[0], struct.unpack("<f", struct.pack("<I", tw))[0]
        print("  %08x: arm=%08x (%g) tradução=%08x (%g)" % (w, armw, af, tw, tf))
        if len(shown) >= 64:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
