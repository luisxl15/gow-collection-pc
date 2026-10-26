"""dead_flags.py <elf> : globais lidas e testadas (ldr/ldrb + cbz/cbnz/cmp/tst logo depois) que nenhuma
instrução grava — candidatas a interruptor fixo no código. Thumb2; endereço montado por movw/movt."""
import re
import struct
import sys
from collections import defaultdict

import capstone as cs

elf = sys.argv[1]
d = open(elf, 'rb').read()
phoff = struct.unpack_from('<I', d, 0x1c)[0]
phnum = struct.unpack_from('<H', d, 0x2c)[0]
segs = []
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
    if t == 1:
        segs.append((va, off, fsz, msz, fl))
text = segs[0]
data_lo, data_hi = segs[1][0], segs[1][0] + segs[1][3]


def init_val(va):
    for sva, off, fsz, msz, fl in segs:
        if sva <= va < sva + fsz:
            return struct.unpack_from('<I', d, off + va - sva)[0]
        if sva <= va < sva + msz:
            return 0  # bss
    return None


md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
md.skipdata = True
md.detail = False
ins_list = list(md.disasm(d[text[1]:text[1] + text[2]], text[0]))
regs = {}
reads, writes, tests = defaultdict(list), defaultdict(int), defaultdict(int)
mem_re = re.compile(r'(\w+), \[(\w+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]')
for k, ins in enumerate(ins_list):
    m, ops = ins.mnemonic, ins.op_str
    if m in ('movw', 'movt'):
        r, imm = ops.split(', ')[:2]
        v = int(imm.lstrip('#'), 0)
        regs[r] = (regs.get(r, 0) & 0xFFFF0000 | v) if m == 'movw' else ((v << 16) | (regs.get(r, 0) & 0xFFFF))
        continue
    mm = mem_re.fullmatch(ops)
    if mm and mm.group(2) in regs and m.startswith(('ldr', 'str')):
        a = regs[mm.group(2)] + (int(mm.group(3), 0) if mm.group(3) else 0)
        if data_lo <= a < data_hi:
            if m.startswith('str'):
                writes[a] += 1
            else:
                reads[a].append(ins.address)
                dst = mm.group(1)
                for nxt in ins_list[k + 1:k + 4]:
                    if nxt.mnemonic in ('cbz', 'cbnz') and nxt.op_str.startswith(dst + ','):
                        tests[a] += 1
                        break
                    if nxt.mnemonic in ('cmp', 'tst', 'ands', 'ands.w', 'lsls') and nxt.op_str.startswith(dst):
                        tests[a] += 1
                        break
    # um registrador escrito por outra instrução deixa de valer o endereço
    if ops and m not in ('cmp', 'tst', 'cbz', 'cbnz', 'b', 'bl', 'blx', 'bx') and not m.startswith(('str', 'push', 'pop', 'v')):
        dst = ops.split(',')[0].strip()
        if dst in regs and not (mm and mm.group(2) == dst):
            regs.pop(dst, None)
    if m in ('bl', 'blx', 'bx', 'pop', 'b') or m.startswith('pop'):
        regs = {}
for a in sorted(tests):
    if writes[a] == 0:
        print('%08x init=%08x leituras=%d testes=%d em %s' % (a, init_val(a) or 0, len(reads[a]), tests[a],
                                                              ' '.join('%08x' % x for x in reads[a][:6])))
