"""Malha do GoW2 de PS2 (registro MDL_*_N, magia 0x0001000f): partes -> grupos de LOD -> objetos -> pacotes VIF.

    python tools/ps2_mesh.py <WAD do PS2> <nome do registro>     # resumo e alguns vértices

Porte do god_of_war_browser (mogaika, pack/wad/mesh: gow2.go e dmaVif.go). Cada objeto tem cadeias DMA (REF/RET);
os UNPACKs do VIF trazem, por pacote: XYZW (int16 / 16; flag 0x8000 do w = pular o triângulo, resto = peso / 4096),
UV (int16 ou int32 / 4096), normais (int8 / 127), RGBA (u8) e blocos de 16 bytes com os ossos de cada grupo de
vértices (block[12] >> 2 e block[13] >> 4, índices no mapa de ossos do objeto). Os triângulos são tiras: cada vértice
fecha um triângulo com os dois anteriores, a não ser que tenha a flag de pular.
"""
import struct
import sys

MESH_MAGIC = 0x0001000f
UNPACK_BASES = (0, 0x155, 0x2ab)


class Packet:
    def __init__(self):
        self.xyz, self.skip, self.weight = [], [], []
        self.uv, self.norm, self.rgba = None, None, None
        self.joints = None  # (j1, j2) por vértice
        self.bounds = None


class State:
    def __init__(self, buf):
        self.buf = buf
        self.xyzw = self.rgba = self.uv = self.norm = self.meta = self.bounds = None
        self.uvw = 0

    def packet(self):
        if self.xyzw is None:
            return None
        p = Packet()
        n = len(self.xyzw) // 8
        for i in range(n):
            x, y, z, f = struct.unpack_from('<hhhH', self.xyzw, 8 * i)
            p.xyz.append((x / 16.0, y / 16.0, z / 16.0))
            p.skip.append(bool(f & 0x8000))
            p.weight.append((f & 0x7fff) / 4096.0)
        if self.uv is not None:
            if self.uvw == 2:
                p.uv = [(u / 4096.0, v / 4096.0) for u, v in struct.iter_unpack('<hh', self.uv[:len(self.uv) // 4 * 4])]
            else:
                p.uv = [(u / 4096.0, v / 4096.0) for u, v in struct.iter_unpack('<ii', self.uv[:len(self.uv) // 8 * 8])]
        if self.norm is not None:
            p.norm = [tuple(c / 127.0 for c in struct.unpack_from('<bbb', self.norm, 3 * i)) for i in range(len(self.norm) // 3)]
        if self.rgba is not None:
            p.rgba = [tuple(self.rgba[4 * i:4 * i + 4]) for i in range(len(self.rgba) // 4)]
        if self.bounds is not None:
            p.bounds = struct.unpack_from('<4f', self.bounds)
        if self.meta is not None:
            blocks = [self.meta[i:i + 16] for i in range(0, len(self.meta) // 16 * 16, 16)]
            p.joints = []
            stitch = 0

            def jj(b):
                return (b[12] >> 2, b[13] >> 4)
            for ib, b in enumerate(blocks):
                for j in range(b[0]):
                    cur = jj(b)
                    if b[5] & 0x20:
                        if b[5] & 0x10:
                            if stitch % 2:
                                cur = jj(blocks[ib + 1])
                            stitch += 1
                        else:
                            stitch -= 1
                            if stitch % 2:
                                cur = jj(blocks[ib - 1])
                    p.joints.append(cur)
        return p


def parse_vif(data, st, packets):
    pos = 0
    while True:
        pos = (pos + 3) & ~3
        if pos >= len(data):
            break
        code = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        cmd, num, imm = code >> 24, (code >> 16) & 0xff, code & 0xffff
        if cmd > 0x60:
            comps = ((cmd >> 2) & 3) + 1
            width = (32, 16, 8, 4)[cmd & 3]
            size = comps * (width * (num or 256) // 8)
            signed = not (imm >> 14) & 1
            target = imm & 0x3ff
            buf = 1 + sum(1 for b in UNPACK_BASES if target >= b)
            if st[0] is None:
                st[0] = State(buf)
            s = st[0]
            blk = data[pos:pos + size]
            if width == 32 and signed and comps == 4:
                if target in UNPACK_BASES:
                    s.meta = blk
                else:
                    s.bounds = blk
            elif width == 32 and signed and comps == 2:
                s.uv, s.uvw = blk, 4
            elif width == 16 and signed and comps == 4:
                s.xyzw = blk
            elif width == 16 and signed and comps == 2:
                s.uv, s.uvw = blk, 2
            elif width == 8 and comps == 3 and signed:
                s.norm = blk
            elif width == 8 and comps == 4:
                s.rgba = blk
            else:
                raise ValueError('UNPACK não tratado: cmd %#x %d bits %d comps' % (cmd, width, comps))
            pos += size
        else:
            if cmd == 0x14:  # MSCAL: fim do pacote
                if st[0] is not None:
                    p = st[0].packet()
                    if p:
                        packets.append(p)
                st[0] = None
            elif cmd == 0x30:  # STROW
                pos += 16


class Obj:
    pass


def parse_object(b, o, size):
    ob = Obj()
    ob.offset = o
    ob.type, _, ob.dma_tags = struct.unpack_from('<HHI', b, o)
    ob.material, ob.jm_count = struct.unpack_from('<HH', b, o + 8)
    ob.instances, ob.flags, ob.mask = struct.unpack_from('<III', b, o + 0xc)
    ob.layers, ob.programs = b[o + 0x18], b[o + 0x19]
    ob.src_faces, ob.src_verts = struct.unpack_from('<HH', b, o + 0x1c)
    calls = ob.instances * ob.layers
    ob.packets = []
    for c in range(calls):
        po = o + 0x20 + c * ob.dma_tags * 0x10
        st = [None]
        pk = []
        for i in range(ob.dma_tags):
            tp = po + i * 0x10
            tag = struct.unpack_from('<Q', b, tp)[0]
            qwc, tid, addr = tag & 0xffff, (tag >> 28) & 7, (tag >> 32) & 0x7fffffff
            parse_vif(b[tp + 8:tp + 16], st, pk)
            if tid == 3:  # REF
                start = addr + o
                parse_vif(b[start:start + qwc * 16], st, pk)
            elif tid != 6:
                raise ValueError('tag DMA %d' % tid)
        ob.packets.append(pk)
    jo = o + 0x20 + calls * 0x10 * ob.dma_tags
    ob.joint_map = list(struct.unpack_from('<%dI' % ob.jm_count, b, jo)) if ob.jm_count else []
    return ob


def parse(b):
    assert struct.unpack_from('<I', b, 0)[0] == MESH_MAGIC
    end = struct.unpack_from('<I', b, 4)[0]
    nparts = struct.unpack_from('<H', b, 8)[0]
    parts = []
    poffs = [struct.unpack_from('<I', b, 0x18 + 4 * i)[0] for i in range(nparts)]
    for ip, po in enumerate(poffs):
        pend = poffs[ip + 1] if ip + 1 < nparts else end
        ngroups = struct.unpack_from('<H', b, po + 2)[0]
        goffs = [struct.unpack_from('<I', b, po + 4 + 4 * i)[0] for i in range(ngroups)]
        joint = struct.unpack_from('<H', b, po + 4 + 4 * ngroups)[0]
        groups = []
        for ig, go in enumerate(goffs):
            gp = po + go
            gend = po + goffs[ig + 1] if ig + 1 < ngroups else pend
            hide = struct.unpack_from('<f', b, gp)[0]
            nobj = struct.unpack_from('<H', b, gp + 4)[0]
            objs = []
            if nobj:
                ooffs = [struct.unpack_from('<I', b, gp + 8 + 4 * i)[0] for i in range(nobj)]
                for io, oo in enumerate(ooffs):
                    oend = ooffs[io + 1] if io + 1 < nobj else gend - gp
                    objs.append(parse_object(b, gp + oo, oend - oo))
            groups.append((hide, objs))
        parts.append((joint, groups))
    return parts


def main():
    sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
    import texpack_ps2
    wad, name = sys.argv[1], sys.argv[2]
    rec = next(body for t, n, body in texpack_ps2.walk(open(wad, 'rb').read()) if n == name)
    parts = parse(rec)
    tv = 0
    for ip, (joint, groups) in enumerate(parts):
        for ig, (hide, objs) in enumerate(groups):
            for io, ob in enumerate(objs):
                nv = sum(len(p.xyz) for p in ob.packets[0])
                tv += nv
                print('parte %d (osso %d) LOD %d (%.0f) obj %d: tipo %#x material %d camadas %d inst %d pacotes %d vértices %d'
                      ' (fonte %d) ossos %s' % (ip, joint, ig, hide, io, ob.type, ob.material, ob.layers, ob.instances,
                                                len(ob.packets[0]), nv, ob.src_verts, ob.joint_map[:8]))
    print('total de vértices (1a camada):', tv)
    p = parts[0][1][0][1][0].packets[0][0]
    for i in range(6):
        print('  v%d xyz=%s uv=%s n=%s j=%s w=%.3f skip=%s' % (i, p.xyz[i], p.uv[i] if p.uv else None,
              p.norm[i] if p.norm else None, p.joints[i] if p.joints else None, p.weight[i], p.skip[i]))


if __name__ == '__main__':
    main()
