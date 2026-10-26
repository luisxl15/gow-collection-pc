"""scr_cmds.py <elf> <registrador> [reg_nome reg_função] : lista os comandos de script (nome -> função) registrados por bl <registrador>
e marca as funções vazias (retornam logo, ou só zeram o retorno). Thumb2, segmentos do ELF."""
import re
import struct
import sys

import capstone as cs

elf, reg_fn = sys.argv[1], int(sys.argv[2], 0)
NAME_R, FN_R = (sys.argv[3], sys.argv[4]) if len(sys.argv) > 4 else ('r0', 'r1')
d = open(elf, 'rb').read()
phoff = struct.unpack_from('<I', d, 0x1c)[0]
phnum = struct.unpack_from('<H', d, 0x2c)[0]
segs = []
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
    if t == 1:
        segs.append((va, off, fsz))


def rd(va, n):
    for sva, off, fsz in segs:
        if sva <= va < sva + fsz:
            return d[off + va - sva: off + va - sva + n]
    return None


def s_at(va):
    b = rd(va, 80)
    m = re.match(rb'[\x20-\x7e]{2,79}\x00', b or b'')
    return m.group()[:-1].decode() if m else None


md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
md.skipdata = True
va0, off0, fsz0 = segs[0]
regs = {}
out = []
for ins in md.disasm(d[off0:off0 + fsz0], va0):
    m, ops = ins.mnemonic, ins.op_str
    if m in ('movw', 'movt'):
        r, imm = ops.split(', ')
        v = int(imm.lstrip('#'), 0)
        regs[r] = (regs.get(r, 0) & 0xFFFF0000 | v) if m == 'movw' else ((v << 16) | (regs.get(r, 0) & 0xFFFF))
    elif m == 'ldr' and re.fullmatch(r'(r\d+), \[(r\d+)\]', ops) and ops.split(', [')[1][:-1] in regs:
        dst, src = ops.split(', [')[0], ops.split(', [')[1][:-1]
        w = rd(regs[src], 4)
        regs[dst] = struct.unpack('<I', w)[0] if w else 0
    elif m == 'bl' and int(ops.lstrip('#'), 0) == reg_fn:
        name = s_at(regs.get(NAME_R, 0))
        out.append((ins.address, name, regs.get(FN_R)))
        regs = {}


def body(fn):
    fn &= ~1
    b = rd(fn, 16) or b''
    return [(i.mnemonic, i.op_str) for i in md.disasm(b, fn)][:4]


for at, name, fn in out:
    if fn is None or name is None:
        continue
    b = body(fn)
    empty = b and (b[0] == ('bx', 'lr') or (b[0][0] == 'movs' and b[0][1].startswith('r0, #') and b[1] == ('bx', 'lr')))
    print('%s %-32s %08x %s' % ('VAZIA' if empty else '     ', name, fn, ' ; '.join('%s %s' % x for x in b[:3])))
