"""Pack de texturas do PCSX2 -> vitart, cobrindo também as texturas que mudaram de nome no Vita.

    python tools/texpack_match.py <iso do PS2> <psarc do Vita> <pack.zip> <saída> [--rel relatório.tsv]

1. Casamento por nome (tools/texpack_build.py): TXR do Vita -> TXR do PS2 -> nome PCSX2 (TEX0-CLUT).
2. Casamento por conteúdo para o que sobrou: cada textura do Vita (GXT decodificado) é comparada com os
   PNGs do pack de mesma proporção por uma assinatura 16x16 (luminância pré-multiplicada pelo alfa,
   alfa e cor média). Aceita só correlação alta, cor parecida e folga sobre o segundo candidato. Pega
   texturas renomeadas, reorganizadas no Vita, e variantes de paleta que o nome do PS2 (paleta 0) não
   alcança.
3. Chave do vitart (hle_gxm.cpp, texpack_key): FNV-1a 64 de (largura, altura) + até 4096 bytes do nível
   base + a paleta inteira nas texturas com paleta (variantes de cor da mesma imagem são texturas
   diferentes).
Saída: <saída>/index.tsv e os PNGs usados; --rel grava como cada textura foi casada.
"""
import io
import os
import struct
import sys
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxm_tex_dump as D  # noqa: E402
import psarc_gxp as P  # noqa: E402
import texpack_build as B  # noqa: E402
import texpack_ps2 as T  # noqa: E402


class GxtMem:
    """Leitura por endereço dentro de um GXT (endereço = deslocamento)."""

    def __init__(self, b):
        self.b = np.frombuffer(b, dtype=np.uint8)

    def read(self, addr, n):
        out = np.zeros(n, dtype=np.uint8)
        avail = max(0, min(n, len(self.b) - addr))
        out[:avail] = self.b[addr:addr + avail]
        return out


def gxt_textures(gxt):
    """[(índice, dict do decodificador, deslocamento da paleta P8 ou None)]."""
    magic, ver, n, doff, dsize, np4, np8 = struct.unpack_from('<4s6I', gxt, 0)
    pal_base = doff + dsize - np8 * 1024 - np4 * 64  # paletas no fim dos dados: P4 (64 B) e depois P8 (1 KB)
    out = []
    for ti in range(n):
        off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', gxt, 32 + 32 * ti)
        base = fmt & 0xFF000000
        t = dict(type=ty >> 29, base=base, swizzle=(fmt >> 12) & 7, width=w, height=h, stride=None, data=off, palette=0)
        poff = None
        if base == 0x95000000 and pal != 0xFFFFFFFF:
            poff = pal_base + np4 * 64 + pal * 1024
            t['palette'] = poff
        out.append((ti, t, poff))
    return out


def vita_key(gxt, ti, poff):
    off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', gxt, 32 + 32 * ti)
    n = min(4096, B.base_size(w, h, ty >> 29, fmt & 0xFF000000), size)
    if not n:
        return None
    k = B.fnv(gxt[off:off + n], B.fnv(struct.pack('<II', w, h)))
    if poff is not None:
        k = B.fnv(gxt[poff:poff + 1024], k)
    return k


def signature(rgba):
    """Assinatura 16x16: luminância pré-multiplicada pelo alfa e alfa (512 valores) + cor média."""
    im = Image.fromarray(rgba, 'RGBA').resize((16, 16), Image.BOX)
    a = np.asarray(im, dtype=np.float32)
    al = a[..., 3] / 255.0
    lum = (0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]) * al
    v = np.concatenate([lum.ravel(), a[..., 3].ravel()])
    v = v - v.mean()
    nrm = np.linalg.norm(v)
    col = (a[..., :3] * al[..., None]).reshape(-1, 3).mean(0)
    return (v / nrm if nrm > 1e-3 else v), col, nrm


def main():
    iso, psarc, packzip, outdir = sys.argv[1:5]
    rel = sys.argv[sys.argv.index('--rel') + 1] if '--rel' in sys.argv else None
    os.makedirs(outdir, exist_ok=True)
    z = zipfile.ZipFile(packzip)

    # ---- pack: chave PCSX2 -> membro; assinaturas por proporção
    pack, members = {}, []
    for info in z.infolist():
        base = info.filename.replace('\\', '/').rsplit('/', 1)[-1]
        if not base.lower().endswith('.png'):
            continue
        parts = base[:-4].split('-')
        pack.setdefault('-'.join(parts[:2]) if len(parts) >= 3 else parts[0], info)
        members.append(info)
    print(f'pack: {len(members)} PNGs; assinaturas...', flush=True)
    sigs = {}  # proporção -> lista de (membro, vetor, cor, largura)
    for i, info in enumerate(members):
        with z.open(info) as fh:
            im = Image.open(io.BytesIO(fh.read())).convert('RGBA')
        w, h = im.size
        v, col, nrm = signature(np.asarray(im))
        sigs.setdefault(round(w / h, 3), []).append((info, v, col, nrm, w))
    names = B.ps2_names(iso)
    print(f'PS2: {len(names)} TXR', flush=True)

    # ---- Vita
    f = open(psarc, 'rb')
    ents, zs, block = P.entries(f)
    files = P.read_file(f, ents[0], zs, block).decode('latin1').split('\n')
    index, how = {}, {}
    seen = set()
    stats = dict(vita=0, nome=0, conteudo=0, sem=0, plano=0)
    for k, fn in enumerate(files):
        if not fn.lower().endswith('.wad_psp2') or '_vitatv/' in fn.lower():
            continue  # o _VitaTV tem as mesmas texturas
        recs = [(name, body) for tag, name, body in T.walk(P.read_file(f, ents[k + 1], zs, block)) if tag == 30 and len(body) >= 4]
        gxts = {}
        for name, body in recs:
            if (struct.unpack_from('<I', body, 0)[0] == 0x00070007 and len(body) >= 14 + 32 and body[4:8] == b'TXTR'
                    and body[14:18] == b'GXT\0' and name.startswith('~')):
                gxts[name[4:]] = body[14:]
        txr_of = {}
        for name, body in recs:
            if struct.unpack_from('<I', body, 0)[0] != 7 or len(body) < 52:
                continue
            gname = body[4:28].split(b'\0')[0].decode('latin1')
            pname = body[28:52].split(b'\0')[0].decode('latin1')
            g = gname[4:] if gname.upper().startswith('GFX_') else gname
            for cand in ((g + '~' + pname).lower(), g.lower()):
                if cand[:19] in gxts:
                    txr_of.setdefault(cand[:19], name)
                    break
        for gname, gxt in gxts.items():
            txr = txr_of.get(gname)
            for ti, t, poff in gxt_textures(gxt):
                vk = vita_key(gxt, ti, poff)
                if vk is None or vk in seen:
                    continue
                seen.add(vk)
                stats['vita'] += 1
                pk = names.get(txr, {}).get(ti) if txr else None
                member = pack.get(pk) if pk else None
                if member:
                    index[vk], how[vk] = member, ('nome', 1.0, fn, gname)
                    stats['nome'] += 1
                    continue
                try:
                    rgba = D.decode(GxtMem(gxt), t)
                except Exception:
                    rgba = None
                if rgba is None or rgba.shape[0] < 8 or rgba.shape[1] < 8:
                    stats['sem'] += 1
                    continue
                h, w = rgba.shape[:2]
                v, col, nrm = signature(rgba)
                if nrm < 40:  # quase lisa: assinatura não distingue nada
                    stats['plano'] += 1
                    continue
                cands = sigs.get(round(w / h, 3), [])
                best = second = (-1.0, None)
                for info, pv, pc, pn, pw in cands:
                    if pw < w or pn < 40:
                        continue  # o pack é HD: não menor que a textura do Vita
                    c = float(v @ pv)
                    if np.abs(col - pc).mean() > 24:
                        continue
                    if c > best[0]:
                        best, second = (c, info), best
                    elif c > second[0]:
                        second = (c, info)
                if best[1] is not None and best[0] >= 0.93 and best[0] - second[0] >= 0.02:
                    index[vk], how[vk] = best[1], ('conteudo', best[0], fn, gname)
                    stats['conteudo'] += 1
                else:
                    stats['sem'] += 1
        print(f'{fn}: {stats}', flush=True)

    used = {}
    for vk, member in index.items():
        used[member.filename.replace('\\', '/').rsplit('/', 1)[-1]] = member
    with open(os.path.join(outdir, 'index.tsv'), 'w', encoding='utf-8') as o:
        for vk, member in sorted(index.items()):
            o.write('%016x\t%s\n' % (vk, member.filename.replace('\\', '/').rsplit('/', 1)[-1]))
    for fname, member in used.items():
        dst = os.path.join(outdir, fname)
        if not os.path.exists(dst):
            with z.open(member) as src, open(dst, 'wb') as d:
                d.write(src.read())
    if rel:
        with open(rel, 'w', encoding='utf-8') as o:
            for vk, (kind, score, fn, gname) in sorted(how.items()):
                o.write('%016x\t%s\t%.3f\t%s\t%s\t%s\n' % (vk, kind, score, fn, gname, index[vk].filename))
    print(f'Vita: {stats["vita"]} texturas; por nome {stats["nome"]}, por conteúdo {stats["conteudo"]}, '
          f'lisas {stats["plano"]}, sem par {stats["sem"]}; PNGs do pack usados: {len(used)} de {len(members)}')


if __name__ == '__main__':
    main()
