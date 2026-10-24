"""Pack de texturas do PCSX2 -> vitart, cobrindo também as texturas que mudaram de nome no Vita.

    python tools/texpack_match.py <iso do PS2 | nomes.tsv | -> <psarc do Vita> <pack.zip | pasta> <saída> [--rel relatório.tsv]

Sem a ISO ("-") só o casamento por conteúdo (passo 2). O pack pode ser o .zip ou a pasta "replacements" já
extraída. Funciona com o GoW1 e o GoW2 (registros dos WADs do Vita: tag 30 no GoW1, tag 1 no GoW2).

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
import subprocess
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


def full_key(gxt, ti, poff):
    """Hash do nível base inteiro (e da paleta): distingue texturas cuja chave do vitart (4096 bytes) colide."""
    off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', gxt, 32 + 32 * ti)
    n = min(B.base_size(w, h, ty >> 29, fmt & 0xFF000000) or size, size)
    k = B.fnv(gxt[off:off + n], B.fnv(struct.pack('<II', w, h)))
    return B.fnv(gxt[poff:poff + 1024], k) if poff is not None else k


def _unit(x):
    x = x - x.mean()
    n = float(np.linalg.norm(x))
    return (x / n if n > 1e-3 else x * 0), n


def signature(rgba):
    """Assinatura 16x16: luminância e alfa normalizados CADA UM (média 0, norma 1), cor média e o
    contraste da luminância. (Juntos num vetor só, o alfa quase sempre 255 dominava e tudo parecia igual.)"""
    im = Image.fromarray(rgba, 'RGBA').resize((16, 16), Image.BOX)
    a = np.asarray(im, dtype=np.float32)
    lum = (0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]).ravel()
    vl, nl = _unit(lum)
    va, na = _unit(a[..., 3].ravel())
    col = a[..., :3].reshape(-1, 3).mean(0)
    return (vl, va, na), col, nl


def _detail_and_rgb(rgba):
    """32x32: os três canais (pré-multiplicados pelo alfa) e a energia de detalhe (imagem - borrada)."""
    im = Image.fromarray(rgba, 'RGBA').resize((32, 32), Image.BOX)
    a = np.asarray(im, dtype=np.float32)
    # alfa relativo ao maior da imagem: no PS2 128 é opaco (os PNGs dos packs vêm assim) e no Vita 255
    rgb = a[..., :3] * (a[..., 3:] / max(float(a[..., 3].max()), 1.0))
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    blur = np.asarray(Image.fromarray(lum.astype(np.uint8)).resize((8, 8), Image.BOX).resize((32, 32), Image.BILINEAR),
                      dtype=np.float32)
    return rgb, float(np.abs(lum - blur).mean())


def verify(vita_rgba, png):
    """Confirma o candidato: cada canal de cor correlaciona (variantes de cor ficam de fora) e as duas
    imagens têm detalhe parecido e de verdade (efeitos borrados -- brilhos, fumaça -- parecem todos iguais
    na assinatura pequena)."""
    va, dv = _detail_and_rgb(vita_rgba)
    pa, dp = _detail_and_rgb(np.asarray(png.convert('RGBA')))
    # medido em pares conferidos a olho: com pouco detalhe (paredes e chão lisos do GoW2, 3,5..7) aceita só se
    # as cores correlacionam ainda mais (abaixo)
    if dv < 3.5 or dp < 3.5 or not (0.7 <= dp / dv <= 1.43):
        return 0.0
    low = dv < 7.0 or dp < 7.0
    # saturação parecida: a versão petrificada (cinza) de um inimigo tem o mesmo desenho da colorida
    def sat(rgb):
        mx, mn = rgb.max(-1), rgb.min(-1)
        return float(((mx - mn) / np.maximum(mx, 1.0)).mean())
    if abs(sat(va) - sat(pa)) > 0.12:
        return 0.0
    cs = []
    for ch in range(3):
        x, nx = _unit(va[..., ch].ravel())
        y, ny = _unit(pa[..., ch].ravel())
        if nx < 1e-3 or ny < 1e-3:
            continue
        cs.append(float(x @ y))
    if not cs:
        return 0.0
    return min(cs) if not low or min(cs) >= 0.92 else 0.0


MIN_CORR, MIN_GAP = float(os.environ.get('TEXPACK_MIN_CORR', '0.85')), float(os.environ.get('TEXPACK_MIN_GAP', '0.03'))
MIN_VERIFY = float(os.environ.get('TEXPACK_MIN_VERIFY', '0.80'))
NAME_MIN_CORR = float(os.environ.get('TEXPACK_NAME_MIN_CORR', '0.6'))  # par por nome: semelhança mínima


def pkey(info):
    parts = info.filename.replace(chr(92), '/').rsplit('/', 1)[-1][:-4].split('-')
    return '-'.join(parts[:2])


class DirPack:
    """Uma pasta de PNGs com a interface do zipfile que o resto usa (infolist, open)."""

    class Info:
        def __init__(self, path, rel):
            self.path, self.filename = path, rel

    def __init__(self, root):
        self.items = []
        for dp, _, fs in os.walk(root):
            for fn in sorted(fs):
                full = os.path.join(dp, fn)
                self.items.append(DirPack.Info(full, os.path.relpath(full, root)))

    def infolist(self):
        return self.items

    def open(self, info):
        return open(info.path, 'rb')


def wad_records(d, game):
    """Registros (tag, nome, corpo) de um .wad_psp2: tags sem corpo 24 (GoW1) / 0 e 24 (GoW2)."""
    nobody = (0, 24) if game == 'gow2' else (24,)
    o, out = 0, []
    while o + 32 <= len(d):
        tag, flags, size = struct.unpack_from('<HHI', d, o)
        name = d[o + 8:o + 32].split(b'\0')[0].decode('latin1')
        body = b'' if tag in nobody else d[o + 32:o + 32 + size]
        out.append((tag, name, body))
        o = (o + 32 + (0 if tag in nobody else size) + 15) & ~15
    return out


def names_tsv(path):
    """A tabela de nomes da ISO já extraída (wad, TXR, GFX, PAL, imagem, paleta, nome PCSX2), para casar por nome
    sem a ISO: {TXR: {imagem: [(WAD, chave), ...]}} com a paleta 0 de TODOS os WADs -- no GoW2 o mesmo nome de TXR
    aparece em vários WADs com imagens diferentes, e o PNG do pack pode ser o de qualquer um deles."""
    out = {}
    for ln in open(path, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) < 7 or c[5] not in ('0', 'None'):
            continue
        parts = c[6].split('-')
        k = '-'.join(parts[:2]) if len(parts) >= 3 else parts[0]
        lst = out.setdefault(c[1], {}).setdefault(int(c[4]), [])
        if all(k != kk for _, kk in lst):
            lst.append((c[0].upper(), k))
    return out


def name_candidates(names, txr, ti, vita_wad):
    """[(chave PCSX2, confiável)] para o TXR/imagem do Vita: primeiro as do WAD de mesmo nome no PS2, depois as
    outras. Confiável = do mesmo WAD, ou o nome do TXR existe em no máximo 2 WADs (nomes genéricos como TXR_file1
    se repetem em dezenas de WADs com imagens diferentes)."""
    lst = names.get(txr, {}).get(ti, [])
    w = os.path.basename(vita_wad).upper().replace('.WAD_PSP2', '.WAD')
    nw = len({wd for g in names.get(txr, {}).values() for wd, _ in g})
    return [(k, True) for wd, k in lst if wd == w] + [(k, nw <= 2) for wd, k in lst if wd != w]


def name_relaxed(rgba, png):
    """Par por nome confiável que falhou a semelhança de desenho: as repinturas do pack (pedra, chão, madeira) muitas
    vezes trocam o desenho do material inteiro. Aceita se a proporção, a presença de alfa e o matiz (cor relativa ao
    brilho: a repintura costuma ser mais escura) batem e a textura do Vita não é minúscula. Medido em 72 recusas
    rotuladas a olho no GoW2: ~40 de 45 certas recuperadas, 1 errada aceita."""
    h, w = rgba.shape[:2]
    if max(w, h) < 64 or min(w, h) < 32 or abs(png.width / png.height - w / h) >= 0.02:
        return False
    (_, _, na), cv, _ = signature(rgba)
    (_, _, pna), cp, _ = signature(np.asarray(png.convert('RGBA')))
    if (na > 40) != (pna > 40) or np.abs(cv - cp).mean() > 70:
        return False
    return float(np.abs(cv / max(cv.mean(), 1.0) - cp / max(cp.mean(), 1.0)).mean()) <= 0.3


def main():
    iso, psarc, packzip, outdir = sys.argv[1:5]
    game = 'gow2' if 'gow2' in os.path.basename(psarc).lower() else 'gow1'
    rtag = 1 if game == 'gow2' else 30
    rel = sys.argv[sys.argv.index('--rel') + 1] if '--rel' in sys.argv else None
    # --cand arquivo: para cada textura sem par, o melhor candidato e a nota (para escolher limiares);
    # --dump pasta: grava essas texturas do Vita em PNG para conferir
    cand_out = open(sys.argv[sys.argv.index('--cand') + 1], 'w', encoding='utf-8') if '--cand' in sys.argv else None
    dump_dir = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else None
    if dump_dir:
        os.makedirs(dump_dir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    z = DirPack(packzip) if os.path.isdir(packzip) else zipfile.ZipFile(packzip)

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
    sig_of = {}  # id do membro -> (vetor, cor): para ver se dois candidatos empatados são a mesma imagem
    for i, info in enumerate(members):
        with z.open(info) as fh:
            im = Image.open(io.BytesIO(fh.read())).convert('RGBA')
        w, h = im.size
        v, col, nrm = signature(np.asarray(im))
        sigs.setdefault(round(w / h, 3), []).append((info, v, col, nrm, w))
        sig_of[id(info)] = (v, col)
    if iso.lower().endswith('.iso'):
        names = {t: {g: [(None, k)] for g, k in keys.items()} for t, keys in B.ps2_names(iso).items()}
    else:
        names = names_tsv(iso) if iso != '-' else {}
    print(f'PS2: {len(names)} TXR', flush=True)

    # ---- Vita
    f = open(psarc, 'rb')
    ents, zs, block = P.entries(f)
    files = P.read_file(f, ents[0], zs, block).decode('latin1').split('\n')
    index, how = {}, {}
    seen, ambig = {}, set()  # chave do vitart -> hash completo; chaves com mais de uma textura
    stats = dict(vita=0, nome=0, conteudo=0, sem=0, plano=0, nome_recusado=0, nome_solto=0, sem_txr_ps2=0, sem_png=0)
    # --diag arquivo: pares por nome recusados pela semelhança (textura do Vita, WAD, TXR, PNG do pack)
    diag_out = open(sys.argv[sys.argv.index('--diag') + 1], 'w', encoding='utf-8') if '--diag' in sys.argv else None
    for k, fn in enumerate(files):
        if not fn.lower().endswith('.wad_psp2') or '_vitatv/' in fn.lower():
            continue  # o _VitaTV tem as mesmas texturas
        recs = [(name, body) for tag, name, body in wad_records(P.read_file(f, ents[k + 1], zs, block), game)
                if tag == rtag and len(body) >= 4]
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
                if vk is None:
                    continue
                # a chave só olha os primeiros 4096 bytes: texturas diferentes com o mesmo começo (topo transparente,
                # por exemplo) colidem e a troca de uma trocaria a outra no jogo -- essas chaves ficam fora do pack
                full = full_key(gxt, ti, poff)
                if vk in seen:
                    if seen[vk] != full:
                        ambig.add(vk)
                    continue
                seen[vk] = full
                stats['vita'] += 1
                members_n = [(pack[k], ok) for k, ok in (name_candidates(names, txr, ti, fn) if txr else []) if k in pack]
                member = relaxed = None
                if not txr or txr not in names:
                    stats['sem_txr_ps2'] += 1
                elif not members_n:
                    stats['sem_png'] += 1  # o PS2 tem a textura, o pack não
                for cand, trusted in members_n:
                    if os.environ.get('TEXPACK_NAME_CHECK', '1') == '0':
                        member = cand
                        break
                    # o nome do TXR coincide entre PS2 e Vita, mas o Vita reorganizou muitas texturas (no GoW2 ~1/3
                    # dos pares por nome eram imagens diferentes): o par por nome também tem de se parecer
                    try:
                        rgba_n = D.decode(GxtMem(gxt), t)
                        with z.open(cand) as fh:
                            png_n = Image.open(io.BytesIO(fh.read()))
                            png_n.load()
                        hn, wn = rgba_n.shape[:2]
                        same_aspect = abs(png_n.width / png_n.height - wn / hn) < 0.02
                        (vl, va, na), coln, _ = signature(rgba_n)
                        (pl, pa, pna), colp, _ = signature(np.asarray(png_n.convert('RGBA')))
                        good = same_aspect and float(vl @ pl) >= NAME_MIN_CORR and np.abs(coln - colp).mean() <= 40
                    except Exception:
                        good = False
                    if good:
                        member = cand
                        break
                    if trusted and relaxed is None and os.environ.get('TEXPACK_NAME_RELAXED', '1') != '0':
                        try:
                            relaxed = cand if name_relaxed(rgba_n, png_n) else None
                        except Exception:
                            pass
                if members_n and not member and not relaxed:
                    stats['nome_recusado'] += 1  # segue para o casamento por conteúdo
                    if diag_out:
                        diag_out.write('%016x\t%s\t%s\t%s\n' % (vk, fn, txr, members_n[0][0].filename))
                if member:
                    index[vk], how[vk] = member, ('nome', 1.0, fn, gname)
                    stats['nome'] += 1
                    if dump_dir and os.environ.get('TEXPACK_DUMP_ALL'):  # revisão a olho
                        try:
                            Image.fromarray(D.decode(GxtMem(gxt), t), 'RGBA').save(os.path.join(dump_dir, '%016x.png' % vk))
                        except Exception:
                            pass
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
                    if relaxed is not None:
                        index[vk], how[vk] = relaxed, ('nome_solto', 0.5, fn, gname)
                        stats['nome_solto'] += 1
                        if dump_dir and os.environ.get('TEXPACK_DUMP_ALL'):
                            Image.fromarray(rgba, 'RGBA').save(os.path.join(dump_dir, '%016x.png' % vk))
                        continue
                    stats['plano'] += 1
                    continue
                cands = sigs.get(round(w / h, 3), [])
                best = second = (-1.0, None)
                vl, va, na = v
                for info, pv, pc, pn, pw in cands:
                    if pw < w or pn < 40:
                        continue  # o pack é HD: não menor que a textura do Vita
                    pl, pa, pna = pv
                    c = float(vl @ pl)
                    if na > 40 and pna > 40:  # as duas com alfa variando: o desenho do alfa também tem de bater
                        c = min(c, float(va @ pa))
                    elif (na > 40) != (pna > 40):
                        continue
                    if np.abs(col - pc).mean() > 24:
                        continue
                    # cópias do mesmo PNG no pack (mesmo TEX0-CLUT, sufixo diferente) não contam como rival
                    same = best[1] is not None and pkey(info) == pkey(best[1])
                    if c > best[0]:
                        if not same:
                            second = best
                        best = (c, info)
                    elif c > second[0] and not same:
                        second = (c, info)
                # empate entre duas cópias da mesma imagem (o pack repete PNGs em pastas diferentes, com outro nome):
                # se os dois candidatos são praticamente iguais entre si, o empate não conta
                tie_ok = False
                if best[1] is not None and second[1] is not None and best[0] - second[0] < MIN_GAP:
                    (bl, ba, _), bc = sig_of[id(best[1])]
                    (sl, sa, _), sc = sig_of[id(second[1])]
                    tie_ok = float(bl @ sl) >= 0.98 and np.abs(bc - sc).mean() <= 8
                ok = best[1] is not None and best[0] >= MIN_CORR and (best[0] - second[0] >= MIN_GAP or tie_ok)
                if ok:
                    with z.open(best[1]) as fh:
                        vc = verify(rgba, Image.open(io.BytesIO(fh.read())))
                    ok = vc >= MIN_VERIFY
                if ok:
                    index[vk], how[vk] = best[1], ('conteudo', min(best[0], vc), fn, gname)
                    if dump_dir and os.environ.get('TEXPACK_DUMP_ALL'):  # revisão a olho dos aceitos
                        Image.fromarray(rgba, 'RGBA').save(os.path.join(dump_dir, '%016x.png' % vk))
                    stats['conteudo'] += 1
                elif relaxed is not None:
                    index[vk], how[vk] = relaxed, ('nome_solto', 0.5, fn, gname)
                    stats['nome_solto'] += 1
                    if dump_dir and os.environ.get('TEXPACK_DUMP_ALL'):
                        Image.fromarray(rgba, 'RGBA').save(os.path.join(dump_dir, '%016x.png' % vk))
                else:
                    stats['sem'] += 1
                    if cand_out and best[1] is not None:
                        cand_out.write(chr(9).join(['%016x', '%dx%d', '%.3f', '%.3f', '%s', '%s', '%s']) % (vk, w, h, best[0], second[0], fn, gname, best[1].filename) + chr(10))
                        if dump_dir:
                            Image.fromarray(rgba, 'RGBA').save(os.path.join(dump_dir, '%016x.png' % vk))
        print(f'{fn}: {stats}', flush=True)

    # trocas recusadas na revisão a olho (texpack_gow1_recusadas.tsv): ficam com a textura do Vita
    rej_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'texpack_%s_recusadas.tsv' % game)
    if os.path.exists(rej_path):
        rej = {int(l.split(chr(9))[0], 16) for l in open(rej_path, encoding='utf-8') if l.strip() and not l.startswith('#')}
        for vk in [vk for vk in index if vk in rej]:
            del index[vk]
            how.pop(vk, None)
        print(f'recusadas na revisão: {len(rej)}', flush=True)
    gone = [vk for vk in index if vk in ambig]
    for vk in gone:
        del index[vk]
        how.pop(vk, None)
    print(f'chaves do vitart com mais de uma textura: {len(ambig)} ({len(gone)} tiradas do pack)', flush=True)
    # extras à mão (texpack_<jogo>_extras.tsv): texturas fora dos WADs (HUD do Flash, chave tirada do VITART_TEXDUMP)
    # ou montadas por um script; valem por cima do casamento
    extras = {}
    ext_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'texpack_%s_extras.tsv' % game)
    if os.path.exists(ext_path):
        by_name = {m.filename.replace('\\', '/').rsplit('/', 1)[-1]: m for m in members}
        for ln in open(ext_path, encoding='utf-8'):
            c = ln.rstrip('\n').split('\t')
            if len(c) < 2 or ln.startswith('#'):
                continue
            vk = int(c[0], 16)
            if c[1].startswith('script:'):  # script:<ferramenta> [argumento]
                spec = c[1][7:].split()
                tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), spec[0])
                fname = '_'.join([os.path.splitext(spec[0])[0]] + spec[1:]) + '.png'
                subprocess.run([sys.executable, tool, packzip, os.path.join(outdir, fname)] + spec[1:], check=True)
            elif c[1].startswith('fixo:'):  # PNG pronto fora do pack (p. ex. textura do Vita ampliada): texpack_fixos/<jogo>
                fname = c[1][5:]
                src = os.path.join(os.environ.get('TEXPACK_FIXOS', os.path.join(os.path.dirname(os.path.abspath(outdir)),
                                                                                 '..', 'texpack_fixos', game)), fname)
                if not os.path.exists(src):
                    print(f'extra fixo não achado: {src}', flush=True)
                    continue
                with open(src, 'rb') as fi, open(os.path.join(outdir, fname), 'wb') as fo:
                    fo.write(fi.read())
            elif c[1] in by_name:
                fname = c[1]
                index[vk] = by_name[fname]
            else:
                print(f'extra sem PNG no pack: {c[1]}', flush=True)
                continue
            extras[vk] = fname
            how.pop(vk, None)
        print(f'extras à mão: {len(extras)}', flush=True)
    used = {}
    for vk, member in index.items():
        used[member.filename.replace('\\', '/').rsplit('/', 1)[-1]] = member
    rows = {vk: m.filename.replace('\\', '/').rsplit('/', 1)[-1] for vk, m in index.items()}
    rows.update(extras)
    with open(os.path.join(outdir, 'index.tsv'), 'w', encoding='utf-8') as o:
        for vk, fname in sorted(rows.items()):
            o.write('%016x\t%s\n' % (vk, fname))
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
          f'por nome (regra solta) {stats["nome_solto"]}, lisas {stats["plano"]}, sem par {stats["sem"]}; PNGs do pack usados: {len(used)} de {len(members)}')


if __name__ == '__main__':
    main()
