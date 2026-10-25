"""WAD de PS2 do GoW2 (de um mod) -> WAD do Vita (.wad_psp2), no formato do porte da Sanzaru.

    python tools/gow2_mod_wad.py <WAD do mod (PS2)> <WAD original do Vita> <saída .wad_psp2>

O WAD do Vita é a mesma árvore do PS2, registro por registro (MAT, ANM, malha de PS2, esqueleto... byte a byte
iguais), com três diferenças, medidas no r_hero01:
  - GFX_*/PAL_* viram tocos: só os primeiros 32 bytes do registro (o cabeçalho), tamanho 32;
  - depois de cada PAL_* entra '~NNN<gfx>~<pal>' (tipo 0x00070007: 'TXTR', 6 bytes, GXT com a textura em BC3
    256x256 swizzled e mipmaps) e o TXR_* aponta para esse nome no 3º campo (+0x34);
  - depois de cada MDL_<nome> (modelo, 0x0002000f) entra '~MD_<nome>' (tipo 0x0003000f): o GMDL gerado da malha
    MDL_<nome>_0 de PS2 (ver tools/gmdl.py e tools/ps2_mesh.py).
Conversão da malha (conferida no Kratos original: 7683 vértices do PS2 -> GMDL do Vita):
  POS0 = (x, y, z, 1 - peso), BONI = (osso1 se 1 - peso > 0, osso2 se peso > 0, 0, 0) com o mapa de ossos do
  objeto em ordem crescente (índices renumerados), NRM0 = normal int8 do PS2, COL0 = RGB / 255 e alfa * 2 / 255 em
  half, TEX0 = UV em half; triângulos (a, b, c) para cada vértice sem a flag de pular, vértices desduplicados.
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxm_tex_dump as D  # noqa: E402
import ps2_mesh  # noqa: E402
import texpack_ps2 as T  # noqa: E402


def records(d):
    """[(tag, flags, nome, corpo, tem_corpo)] de um WAD do GoW2 (PS2 ou Vita)."""
    o, out = 0, []
    while o + 32 <= len(d):
        tag, flags, size = struct.unpack_from('<HHI', d, o)
        name = d[o + 8:o + 32].split(b'\0')[0].decode('latin1')
        nobody = tag in (0, 2, 3, 24)
        out.append([tag, flags, name, d[o + 32:o + 32 + (0 if nobody else size)], not nobody, size])
        o = (o + 32 + (0 if nobody else size) + 15) & ~15
    return out


def write(recs):
    w = bytearray()
    for tag, flags, name, body, has_body, size in recs:
        nm = name.encode('latin1')[:23]
        w += struct.pack('<HHI', tag, flags, len(body) if has_body else size) + nm + bytes(24 - len(nm))
        if has_body:
            w += body
            w += bytes((-len(w)) % 16)
    return bytes(w)


# ------------------------------------------------------------------ texturas
def ps2_rgba(gfx, pal):
    """GFX + PAL do PS2 (8 bits, CSM1) -> RGBA (alfa do PS2: 0x80 = opaco)."""
    g, p = T.gfx_parse(gfx), T.gfx_parse(pal)
    idx = np.array(T.indexes(g, 0), dtype=np.uint8)
    cols = np.frombuffer(T.palette(p, 0), dtype=np.uint8).reshape(-1, 4).copy()
    cols[:, 3] = np.minimum(cols[:, 3].astype(np.int32) * 2, 255)
    return cols[idx]


def bc3_swizzled(rgba):
    """RGBA (lados potência de 2) -> BC3 com mipmaps até 1x1, blocos em ordem Z do Vita."""
    out = bytearray()
    im = Image.fromarray(rgba, 'RGBA')
    w, h = im.size
    while True:
        lvl = im.resize((max(w, 1), max(h, 1)), Image.LANCZOS) if (w, h) != im.size else im
        pw, ph = (w + 3) & ~3, (h + 3) & ~3  # blocos inteiros de 4x4
        if (pw, ph) != (w, h):
            canvas = Image.new('RGBA', (pw, ph))
            canvas.paste(lvl, (0, 0))
            lvl = canvas
        b = io.BytesIO()
        lvl.save(b, 'DDS', pixel_format='DXT5')
        blocks = np.frombuffer(b.getvalue()[128:], dtype=np.uint8).reshape(-1, 16)
        bw, bh = pw // 4, ph // 4
        z = D.zorder_index(D.pow2(bw), D.pow2(bh))[:bh, :bw].reshape(-1)
        sw = np.zeros((D.pow2(bw) * D.pow2(bh), 16), np.uint8)
        sw[z] = blocks
        out += sw.tobytes()
        if w == 1 and h == 1:
            break
        w, h = max(w // 2, 1), max(h // 2, 1)
    return bytes(out)


def tex_record(template, rgba):
    """Registro '~' do Vita: 'TXTR' e os 6 bytes do modelo, GXT com uma textura BC3 swizzled (formato 0x87000000) com
    mipmaps até 1x1 (campo de níveis no fim da descrição da textura, como nas do Vita)."""
    h, w = rgba.shape[:2]
    data = bc3_swizzled(rgba)
    levels = max(w, h).bit_length()
    gxt = bytearray(struct.pack('<4s6I', b'GXT\x00', 0x10000003, 1, 0x40, len(data), 0, 0) + bytes(4))
    gxt += struct.pack('<6I2HI', 0x40, len(data), 0xFFFFFFFF, 0, 0, 0x87000000, w, h, levels)
    return template[:14] + bytes(gxt) + data


# ------------------------------------------------------------------ malha
def model_material_rgba(recs, i):
    """Texturas (RGBA) dos materiais do modelo recs[i], na ordem do índice de material da malha: as referências
    MAT_* logo depois do MDL_ -> corpo do MAT (nome do TXR) -> TXR (GFX e PAL)."""
    body = {}
    for r in recs:
        if r[4] and r[3]:
            body.setdefault(r[2], r[3])
    out = []
    for r in recs[i + 1:]:
        if not (r[0] == 1 and not r[3] and r[2].startswith('MAT_')):
            break
        rgba = None
        try:
            mb = body[r[2]]
            k = mb.index(b'TXR_')
            tb = body[mb[k:k + 24].split(bytes(1))[0].decode('latin1')]
            g, pl = (tb[o:o + 24].split(bytes(1))[0].decode('latin1') for o in (4, 28))
            rgba = ps2_rgba(body[g], body[pl])
        except (KeyError, ValueError, TypeError):
            pass
        out.append(rgba)
    return out


def gmdl_record(mesh, template, nmat, mat_rgba=None):
    """mat_rgba: textura de cada material; objetos que usam a parte transparente dela (franjas de saia, por exemplo)
    vão para o fim da lista de desenho, depois do que fica atrás deles (senão gravam profundidade nos pixels
    transparentes e escondem o que é desenhado depois)."""
    parts = ps2_mesh.parse(mesh)
    t = template[4:]
    tmpl_offs = struct.unpack_from('>%dI' % struct.unpack_from('>I', t, 16)[0], t, 0x18)
    models = []
    for ip, (joint, groups) in enumerate(parts):
        objs = groups[0][1]
        P, N, C, U, B, idx, meta = [], [], [], [], [], [], []
        for ob in objs:
            jmap = sorted(ob.joint_map)
            remap = {i: jmap.index(j) for i, j in enumerate(ob.joint_map)}
            v0, i0 = len(P), len(idx)
            seen = {}
            img = mat_rgba[ob.material] if mat_rgba and ob.material < len(mat_rgba) else None
            clear = tris = 0
            for p in ob.packets[0]:
                ids = []
                for i in range(len(p.xyz)):
                    w = 1.0 - p.weight[i]
                    j1, j2 = p.joints[i]
                    boni = (remap.get(j1, 0) if w > 0 else 0, remap.get(j2, 0) if p.weight[i] > 0 else 0, 0, 0)
                    n = tuple(int(round(c * 127)) for c in p.norm[i]) if p.norm else (0, 0, 127)
                    rgba = p.rgba[i] if p.rgba else (128, 128, 128, 128)
                    col = (rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0, min(rgba[3] * 2, 255) / 255.0)
                    uv = p.uv[i] if p.uv else (0.0, 0.0)
                    key = (p.xyz[i], w, n, rgba, uv, boni)
                    if key not in seen:
                        seen[key] = len(P)
                        P.append((*p.xyz[i], w)), N.append(n), C.append(col), U.append(uv), B.append(boni)
                    ids.append(seen[key])
                for i in range(2, len(ids)):
                    if not p.skip[i]:
                        idx += [ids[i - 2], ids[i - 1], ids[i]]
                        if img is not None and p.uv:
                            u = (p.uv[i - 2][0] + p.uv[i - 1][0] + p.uv[i][0]) / 3
                            v = (p.uv[i - 2][1] + p.uv[i - 1][1] + p.uv[i][1]) / 3
                            h, w = img.shape[:2]
                            clear += img[int((v % 1) * h) % h, int((u % 1) * w) % w, 3] < 250
                            tris += 1
            pos = np.array(P[v0:], np.float32)[:, :3]
            meta.append((pos.min(0), pos.max(0), ob.material, v0, len(P) - v0, i0, len(idx) - i0, jmap,
                         tris and clear / tris > 0.05))
        meta = [m[:8] for m in sorted(meta, key=lambda m: m[8])]  # transparentes por último (ordem estável)
        assert len(P) < 65536
        streams = [('POS0', 2, np.array(P, '<f4').tobytes()), ('NRM0', 9, np.array(N, 'i1').tobytes()),
                   ('COL0', 6, np.array(C, '<f2').tobytes()), ('TEX0', 5, np.array(U, '<f2').tobytes()),
                   ('BONI', 4, np.array(B, 'u1').tobytes())]
        mo = tmpl_offs[min(ip, len(tmpl_offs) - 1)]
        data = bytearray(struct.pack('<I', len(streams)) + bytes(16 * len(streams)))
        table = []
        for name, sid, raw in streams:
            data += bytes((-len(data)) % 16)
            table.append((name, sid, len(data), len(raw)))
            data += raw
        for k, (name, sid, off, size) in enumerate(table):
            struct.pack_into('<4sHHII', data, 4 + 16 * k, name.encode()[::-1], sid, 2, off, size)
        data += bytes((-len(data)) % 16)
        ioff = len(data)
        ihead = t[mo + 0xc + struct.unpack_from('>I', t, mo + 8)[0]:][:12]  # 2 floats e u32 do modelo original
        data += ihead + struct.pack('>I', len(idx)) + struct.pack('>%dH' % len(idx), *idx)
        data += struct.pack('>II', nmat, len(meta))
        for lo, hi, mat, v0, vn, i0, inn, jmap in meta:
            data += struct.pack('>6f', *lo, *hi) + struct.pack('>HB', mat, 0)
            data += struct.pack('>5I', v0, vn, i0, inn, len(jmap)) + struct.pack('>%dI' % len(jmap), *jmap)
        head = t[mo:mo + 8] + struct.pack('>I', ioff)
        models.append(head + bytes(data))
    hdr = bytearray(t[:0x18])
    struct.pack_into('>I', hdr, 16, len(models))
    offs, pos = [], 0x18 + 4 * len(models)
    for m in models:
        offs.append(pos)
        pos += len(m)
    out = bytes(hdr) + struct.pack('>%dI' % len(models), *offs) + b''.join(models)
    return template[:4] + out


# ------------------------------------------------------------------ WAD
def is_mesh(r):
    return r[4] and len(r[3]) >= 4 and struct.unpack_from('<I', r[3], 0)[0] == ps2_mesh.MESH_MAGIC


def model_meshes(recs, i):
    """Malhas do grupo do modelo recs[i] (até o GroupEnd do mesmo nível)."""
    depth, out = 0, []
    for r in recs[i + 1:]:
        if r[0] == 2:
            depth += 1
        elif r[0] == 3:
            if depth == 0:
                break
            depth -= 1
        elif is_mesh(r):
            out.append(r)
    return out


def convert(mod, vita):
    mrec, vrec = records(mod), records(vita)
    vtex = [r for r in vrec if r[2].startswith('~') and r[3][:4] == b'\x07\x00\x07\x00']
    vgmdl = {r[2]: r[3] for r in vrec if r[2].startswith('~MD_')}
    vstub = {r[2]: r[3] for r in vrec if r[2].startswith(('GFX_', 'PAL_')) and len(r[3]) == 32}
    vmesh = {r[2]: r[3] for r in vrec if is_mesh(r)}
    vtweak = {(r[0], r[2]): r for r in vrec if 11 <= r[0] <= 16}
    # (GFX, PAL) -> nome do registro '~' que o TXR do Vita aponta (3o campo)
    vtxr = {(r[3][4:28].split(b'\x00')[0].decode('latin1'), r[3][28:52].split(b'\x00')[0].decode('latin1')):
            r[3][0x34:0x4c].split(b'\x00')[0].decode('latin1')
            for r in vrec if r[4] and len(r[3]) >= 0x4c and struct.unpack_from('<I', r[3], 0)[0] == 7}
    # 1a passada: o nome do registro '~' de cada par GFX+PAL (o TXR acha a textura por esses nomes)
    texname, newtex, tex_n, gfx = {}, {}, 0, None
    for r in mrec:
        tag, flags, name, body, has_body, size = r
        if not (has_body and len(body) >= 4 and struct.unpack_from('<I', body, 0)[0] == 0xc):
            continue
        if name.startswith('GFX_'):
            gfx = r
        elif name.startswith('PAL_') and gfx is not None:
            gname, gbody = gfx[2], gfx[3]
            # textura que o mod não mudou: os tocos (cabeçalho + 8 bytes de pixels/cores) batem com os do Vita, e o
            # registro '~' do Vita vale como está
            vname = vtxr.get((gname, name))
            orig = next((v for v in vtex if v[2] == vname), None)
            if vstub.get(gname) == gbody[:32] and vstub.get(name) == body[:32] and orig is not None:
                texname[(gname, name)] = orig[2]
                newtex[name] = list(orig)
            else:
                rgba = ps2_rgba(gbody, body)
                tmpl = vtex[0][3] if vtex else b'\x07\x00\x07\x00TXTR\x00\x00\x00\x01\x00\xff'
                tname = ('~%03d%s~%s' % (tex_n, gname[4:], 'pal_' + name[4:])).lower()[:23]
                texname[(gname, name)] = tname
                newtex[name] = [1, 0, tname, tex_record(tmpl, rgba), True, 0]
                print('  textura nova %s (%dx%d)' % (tname, rgba.shape[1], rgba.shape[0]))
            tex_n += 1
            gfx = None
    out, gm_new = [], []
    for i, r in enumerate(mrec):
        tag, flags, name, body, has_body, size = r
        magic = struct.unpack_from('<I', body, 0)[0] if has_body and len(body) >= 4 else None
        if 11 <= tag <= 16 and (tag, name) in vtweak:  # tweak templates: os do Vita (o porte veio de outra versão)
            out.append(list(vtweak[(tag, name)]))
            continue
        if has_body and magic == 0xc and name.startswith(('GFX_', 'PAL_')):
            out.append([tag, flags, name, body[:32], True, 32])
            if name in newtex:
                out.append(newtex[name])
            continue
        if has_body and magic == 7 and name.startswith('TXR_') and len(body) >= 0x4c:
            g = body[4:28].split(b'\x00')[0].decode('latin1')
            pl = body[28:52].split(b'\x00')[0].decode('latin1')
            nm = texname.get((g, pl))
            if nm:
                b2 = bytearray(body)
                b2[0x34:0x4c] = nm.encode('latin1') + bytes(24 - len(nm))
                out.append([tag, flags, name, bytes(b2), True, len(b2)])
                continue
        out.append(r)
        if has_body and magic == 0x0002000f and name.startswith('MDL_'):
            gname = ('~MD_' + name[4:])[:23]
            meshes = model_meshes(mrec, i)
            if not meshes:
                continue
            mesh = meshes[0]
            if gname in vgmdl and vmesh.get(mesh[2]) == mesh[3]:
                out.append([1, 0, gname, vgmdl[gname], True, 0])  # malha igual à original: o GMDL do Vita
            else:
                nmat = struct.unpack_from('<I', body, 0x14)[0]  # TextureCount do modelo (god_of_war_browser, mdl.go)
                tmpl = vgmdl.get(gname) or next(iter(vgmdl.values()))
                out.append([1, 0, gname, gmdl_record(mesh[3], tmpl, nmat, model_material_rgba(mrec, i)), True, 0])
                gm_new.append(gname)
    print('texturas novas: %d de %d, GMDL novos: %s, registros: %d -> %d' % (
        sum(1 for v in newtex.values() if v[2] not in {x[2] for x in vtex}), tex_n, gm_new, len(mrec), len(out)))
    return write(out)


def convert_merge(mod, orig, vita, skip=('MSGS_TXT', 'MSGS_COUNT', 'MSGS_LINES')):
    """Para WADs em que o Vita acrescentou registros próprios (r_shella, r_perma): parte do WAD do Vita e troca só o
    que o mod mudou em relação ao PS2 original -- texturas (o '~' do par GFX+PAL e os tocos), malhas (a cópia de PS2 e
    o GMDL do modelo dono dela) e registros que o Vita deixou iguais aos do PS2. Os textos (skip) ficam os do Vita."""
    mrec, orec, vrec = records(mod), records(orig), records(vita)

    def occ(recs):  # (tag, nome, n-ésima ocorrência) -> índice: há muitos nomes repetidos (MAT_pticleMat, FXC_...)
        cnt, out = {}, {}
        for i, r in enumerate(recs):
            k = (r[0], r[2])
            out[k + (cnt.get(k, 0),)] = i
            cnt[k] = cnt.get(k, 0) + 1
        return out
    oocc, vocc, mocc = occ(orec), occ(vrec), occ(mrec)
    oby = {k: orec[i] for k, i in oocc.items()}
    vidx = {}
    for i, r in enumerate(vrec):
        vidx.setdefault((r[0], r[2]), i)
    vtxr = {(r[3][4:28].split(b'\x00')[0].decode('latin1'), r[3][28:52].split(b'\x00')[0].decode('latin1')):
            r[3][0x34:0x4c].split(b'\x00')[0].decode('latin1')
            for r in vrec if r[4] and len(r[3]) >= 0x4c and struct.unpack_from('<I', r[3], 0)[0] == 7}
    out = [list(r) for r in vrec]
    # só os registros que o mod mudou de fato (mesma ocorrência no original), e que existem no Vita
    changed = [(k, mrec[i]) for k, i in mocc.items() if k in oby and mrec[i][4] and mrec[i][3] != oby[k][3] and k in vocc]
    done = []
    gfx = {r[2]: r for r in mrec if r[2].startswith('GFX_')}
    for k, r in changed:
        tag, flags, name, body, has_body, size = r
        magic = struct.unpack_from('<I', body, 0)[0] if len(body) >= 4 else None
        if name in skip:
            continue
        vi = vocc[k]
        if magic == 0xc and name.startswith('PAL_'):
            g = gfx.get('GFX_' + name[4:])
            tname = vtxr.get((g[2], name)) if g else None
            ti = vidx.get((1, tname))
            if g is None or ti is None:
                print('  sem par/registro ~ para', name)
                continue
            out[ti][3] = tex_record(vrec[ti][3], ps2_rgba(g[3], body))
            out[vi][3], out[vi][5] = body[:32], 32
            gi = vidx.get((g[0], g[2]))
            if gi is not None:
                out[gi][3], out[gi][5] = g[3][:32], 32
            done.append(tname)
        elif magic == 0xc:
            continue  # GFX: tratado junto com o PAL
        elif magic == ps2_mesh.MESH_MAGIC:
            out[vi][3] = body
            # o modelo dono da malha: o MDL_ cujo grupo contém esta malha
            for mi in range(vi, -1, -1):
                mr = vrec[mi]
                if mr[4] and len(mr[3]) >= 4 and struct.unpack_from('<I', mr[3], 0)[0] == 0x0002000f:
                    if any(m[2] == name for m in model_meshes(vrec, mi)):
                        gname = ('~MD_' + mr[2][4:])[:23]
                        gi = vidx.get((1, gname))
                        if gi is not None:
                            nmat = struct.unpack_from('<I', mr[3], 0x14)[0]
                            out[gi][3] = gmdl_record(body, vrec[gi][3], nmat)
                            done.append(gname)
                        break
            done.append(name)
        elif vrec[vi][3] == oby[k][3]:
            out[vi][3] = body
            done.append(name)
        else:
            print('  %s: o Vita mudou este registro, fica o do Vita' % name)
    print('mesclados: %s' % done)
    return write(out)


MERGE = ('R_SHELLA', 'R_PERMA')  # WADs com registros próprios do Vita


def convert_all(moddir, psarc, outdir):
    """Todos os WADs de <moddir>/mudados.tsv (tools/ps2_xdelta_wads.py; originais do PS2 em <moddir>/orig) ->
    <outdir>/<nome>.wad_psp2. Imprime os argumentos --wad para o tools/gow_menu_mod.py."""
    import psarc_gxp as P
    os.makedirs(outdir, exist_ok=True)
    args = []
    with open(psarc, 'rb') as fh:
        ents, zs, block = P.entries(fh)
        names = P.read_file(fh, ents[0], zs, block).decode('latin1').split('\n')
        for line in open(os.path.join(moddir, 'mudados.tsv')):
            n = line.split('\t')[0]
            if not n.upper().endswith('.WAD'):
                continue
            base = n[:-4]
            vn = 'wad/' + base.lower() + '.wad_psp2'
            if vn not in names:
                print('sem', vn)
                continue
            mod = open(os.path.join(moddir, n), 'rb').read()
            print('==', n)
            # a versão do PS TV (wad/_VitaTV/, a que o vitart carrega) tem conteúdo próprio em alguns WADs (ícones e
            # textos de botão): é convertida contra ela mesma; igual à normal, vale o mesmo arquivo
            base_vita = None
            for sub in ('wad/', 'wad/_VitaTV/'):
                v = sub + base.lower() + '.wad_psp2'
                if v not in names:
                    continue
                vita = P.read_file(fh, ents[names.index(v) + 1], zs, block)
                if sub != 'wad/' and vita == base_vita:
                    continue
                if sub == 'wad/':
                    base_vita = vita
                if base.upper() in MERGE:
                    res = convert_merge(mod, open(os.path.join(moddir, 'orig', n), 'rb').read(), vita)
                else:
                    res = convert(mod, vita)
                dst = os.path.join(outdir, ('vitatv_' if sub != 'wad/' else '') + base.lower() + '.wad_psp2')
                open(dst, 'wb').write(res)
                args.append('--wad %s=%s' % (v, dst))
    open(os.path.join(outdir, 'wad_args.txt'), 'w', newline='\n').write('\n'.join(args) + '\n')
    print('%d WADs -> %s (argumentos em wad_args.txt)' % (len(args), outdir))


def main():
    if sys.argv[1] == '--all':  # --all <pasta do mod> <psarc do Vita> <saída>
        return convert_all(*sys.argv[2:5])
    if sys.argv[1] == '--merge':  # --merge <mod PS2> <original PS2> <Vita> <saída>
        mod, orig, vita, out = sys.argv[2:6]
        res = convert_merge(open(mod, 'rb').read(), open(orig, 'rb').read(), open(vita, 'rb').read())
    else:
        mod, vita, out = sys.argv[1:4]
        res = convert(open(mod, 'rb').read(), open(vita, 'rb').read())
    open(out, 'wb').write(res)
    print('%s: %d bytes' % (out, len(res)))


if __name__ == '__main__':
    main()
