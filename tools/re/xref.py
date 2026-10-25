"""xref.py <elf> : indexa movw/movt (Thumb2) -> {constante: [endereços]} e grava <elf>.xref.pkl"""
import sys, struct, pickle, os
import capstone as cs
elf = sys.argv[1]
d = open(elf, 'rb').read()
phoff = struct.unpack_from('<I', d, 0x1c)[0]; phnum = struct.unpack_from('<H', d, 0x2c)[0]
segs = []
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
    if t == 1: segs.append((va, off, fsz, fl))
print([(hex(v), hex(o), hex(s), f) for v, o, s, f in segs])
va, off, fsz, fl = next(s for s in segs if s[3] & 1)
code = d[off:off + fsz]
md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
md.skipdata = True
lo = {}
refs = {}
calls = {}
pos = 0
while pos < len(code):
    n = 0
    for ins in md.disasm(code[pos:], va + pos):
        n = ins.address + ins.size - va
        m = ins.mnemonic
        if m == 'movw':
            r, imm = ins.op_str.split(', ')
            lo[r] = (int(imm.lstrip('#'), 0), ins.address)
        elif m == 'movt':
            r, imm = ins.op_str.split(', ')
            if r in lo and ins.address - lo[r][1] < 40:
                c = (int(imm.lstrip('#'), 0) << 16) | lo[r][0]
                refs.setdefault(c, []).append(lo[r][1])
        elif m in ('bl', 'blx') and ins.op_str.startswith('#'):
            calls.setdefault(int(ins.op_str[1:], 0), []).append(ins.address)
    pos = max(n, pos + 2)
pickle.dump({'segs': segs, 'refs': refs, 'calls': calls}, open(elf + '.xref.pkl', 'wb'))
print(len(refs), 'constantes', len(calls), 'alvos de bl')
