"""d.py <elf> <ini> <fim> : desmonta Thumb2, anota movw/movt com a string apontada"""
import sys, struct, re
import capstone as cs
d = open(sys.argv[1], 'rb').read()
a0, a1 = int(sys.argv[2], 0), int(sys.argv[3], 0)
VA, OFF = 0x81000000, 0x100
def s_at(va):
    o = va - VA + OFF
    if 0 <= o < len(d):
        m = re.match(rb'[\x20-\x7e]{3,60}\x00', d[o:o + 64])
        if m: return m.group()[:-1].decode()
md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB); md.skipdata = True
lo = {}
for ins in md.disasm(d[a0 - VA + OFF:a1 - VA + OFF], a0):
    note = ''
    if ins.mnemonic == 'movw':
        r, imm = ins.op_str.split(', '); lo[r] = int(imm.lstrip('#'), 0)
    elif ins.mnemonic == 'movt':
        r, imm = ins.op_str.split(', ')
        c = (int(imm.lstrip('#'), 0) << 16) | lo.get(r, 0)
        s = s_at(c); note = '; %s = %#x %s' % (r, c, repr(s) if s else '')
    print('%x: %-8s %s %s' % (ins.address, ins.mnemonic, ins.op_str, note))
