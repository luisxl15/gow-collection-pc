"""Monta o pack de texturas do vitart a partir de um pack do PCSX2 (God of War de PS2).

    python tools/texpack_build.py <iso do PS2> <psarc do Vita> <pack.zip> <saída (ex.: vitart-data/texpack/gow1)>

1. Calcula o nome PCSX2 de cada textura dos .WAD do PS2 (tools/texpack_ps2.py) e acha o PNG do pack.
2. Lê os .wad_psp2 do Vita: cada TXR (tipo 0x00070007) traz "TXTR" + GXT; cada textura do GXT recebe a
   chave que o vitart calcula ao vivo: FNV-1a 64 de (largura, altura em u32) + os primeiros
   min(4096, tamanho do nível base) bytes (gxmtex::data_size).
3. Liga as duas pelo nome do TXR (o mesmo nas duas versões) e pelo índice da imagem.
Saída: <saída>/index.tsv (chave em hex, arquivo) e os PNGs usados, com o nome do PCSX2.
"""
import os
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(__file__))
import psarc_gxp as P  # noqa: E402
import ps2_audio  # noqa: E402
import texpack_ps2 as T  # noqa: E402


def fnv(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def pow2(v):
    p = 1
    while p < v:
        p <<= 1
    return p


def base_size(w, h, ty, base):
    """gxmtex::data_size: tamanho do nível base."""
    swz = ty in (0, 1, 5, 7)
    if base in (0x85000000, 0x86000000, 0x87000000):
        bw, bh = (w + 3) // 4, (h + 3) // 4
        bs = 8 if base == 0x85000000 else 16
        return (pow2(bw) * pow2(bh) if swz else bw * bh) * bs
    bpp = {0x0C000000: 4, 0x95000000: 1, 0x00000000: 1, 0x07000000: 2}.get(base)
    if bpp is None:
        return 0
    if swz:
        return pow2(w) * pow2(h) * bpp
    if ty == 4:
        return ((w + 31) // 32) * ((h + 31) // 32) * 1024 * bpp
    return ((w + 7) & ~7) * bpp * (h - 1) + w * bpp


def vita_key(gxt, ti):
    off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', gxt, 32 + 32 * ti)
    n = min(4096, base_size(w, h, ty >> 29, fmt & 0xFF000000), size)
    if not n:
        return None
    return fnv(gxt[off:off + n], fnv(struct.pack('<II', w, h)))


def ps2_names(iso_path):
    """TXR -> {índice da imagem: chave TEX0-CLUT} (paleta 0)."""
    iso = open(iso_path, 'rb')
    toc = ps2_audio.toc(iso)
    out = {}
    for wad in sorted(n for n in toc if n.endswith('.WAD')):
        size, lba = toc[wad]
        iso.seek(lba * 2048)
        recs = T.walk(iso.read(size))
        by = {name: body for tag, name, body in recs if tag == 30 and len(body) >= 4}
        for name, body in by.items():
            if name in out or struct.unpack_from('<I', body, 0)[0] != 7 or len(body) < 88:
                continue
            gname = body[4:28].split(b'\0')[0].decode('latin1')
            pname = body[28:52].split(b'\0')[0].decode('latin1')
            g = T.gfx_parse(by.get(gname, b'\0' * 24))
            if not g:
                continue
            p = T.gfx_parse(by[pname]) if pname in by else None
            keys = {}
            for gi in range(len(g['data'])):
                th = T.tex0hash(g, gi)
                if not th:
                    continue
                if p and g['bpi'] in (8, 4):
                    keys[gi] = '%x-%x' % (th[0], __import__('xxhash').xxh3_64_intdigest(T.palette(p, 0)))
                else:
                    keys[gi] = '%x' % th[0]
            out[name] = keys
    return out


def main():
    iso, psarc, packzip, outdir = sys.argv[1:5]
    os.makedirs(outdir, exist_ok=True)
    z = zipfile.ZipFile(packzip)
    pack = {}  # chave TEX0-CLUT (ou TEX0) -> membro do zip
    for info in z.infolist():
        base = info.filename.replace('\\', '/').rsplit('/', 1)[-1]
        if not base.lower().endswith('.png'):
            continue
        parts = base[:-4].split('-')
        key = '-'.join(parts[:2]) if len(parts) >= 3 else parts[0]
        pack.setdefault(key, info)
    names = ps2_names(iso)
    print(f'PS2: {len(names)} TXR; pack: {len(pack)} PNGs')
    f = open(psarc, 'rb')
    ents, zs, block = P.entries(f)
    files = P.read_file(f, ents[0], zs, block).decode('latin1').split('\n')
    index, used, vita_tex = {}, {}, 0
    for k, fn in enumerate(files):
        if not fn.lower().endswith('.wad_psp2'):
            continue
        recs = [(name, body) for tag, name, body in T.walk(P.read_file(f, ents[k + 1], zs, block)) if tag == 30 and len(body) >= 4]
        # No Vita o TXR (tipo 7) aponta para GFX_/PAL_ sem pixels; o GXT fica num recurso 0x00070007 chamado
        # "~" + 3 dígitos hex + (gfx sem "GFX_") + "~" + paleta, em minúsculas e cortado em 23 caracteres.
        gxts = {}
        for name, body in recs:
            if (struct.unpack_from('<I', body, 0)[0] == 0x00070007 and len(body) >= 14 + 32 and body[4:8] == b'TXTR'
                    and body[14:18] == b'GXT\0' and name.startswith('~')):
                gxts[name[4:]] = body[14:]
        for name, body in recs:
            if struct.unpack_from('<I', body, 0)[0] != 7 or len(body) < 52:
                continue
            gname = body[4:28].split(b'\0')[0].decode('latin1')
            pname = body[28:52].split(b'\0')[0].decode('latin1')
            g = gname[4:] if gname.upper().startswith('GFX_') else gname
            gxt = None
            for cand in ((g + '~' + pname).lower(), g.lower()):
                gxt = gxts.get(cand[:19])
                if gxt:
                    break
            if not gxt:
                continue
            count = struct.unpack_from('<I', gxt, 8)[0]
            for ti in range(count):
                vk = vita_key(gxt, ti)
                if vk is None:
                    continue
                vita_tex += 1
                pk = names.get(name, {}).get(ti)
                member = pack.get(pk) if pk else None
                if member and vk not in index:
                    index[vk] = member
    for vk, member in index.items():
        fname = member.filename.replace('\\', '/').rsplit('/', 1)[-1]
        used[fname] = member
    with open(os.path.join(outdir, 'index.tsv'), 'w', encoding='utf-8') as o:
        for vk, member in sorted(index.items()):
            o.write('%016x\t%s\n' % (vk, member.filename.replace('\\', '/').rsplit('/', 1)[-1]))
    for fname, member in used.items():
        dst = os.path.join(outdir, fname)
        if not os.path.exists(dst):
            with z.open(member) as src, open(dst, 'wb') as d:
                d.write(src.read())
    print(f'Vita: {vita_tex} texturas; com PNG do pack: {len(index)} ({len(used)} arquivos) -> {outdir}')


if __name__ == '__main__':
    main()
