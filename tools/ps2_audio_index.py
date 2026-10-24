"""Monta o índice que troca o áudio em streaming do Vita pelo do PS2 (ver docs/audio-ps2.md).

    python tools/ps2_audio_index.py <gow1_psp2_SCEA.psarc> <iso do PS2> <saída ps2audio.idx>
    (GoW2: gow2_psp2_SCEA.psarc, a ISO do GoW2 e a saída ps2audio_gow2.idx)

Para cada .at9 de wad/_sound/dvd_streams/PSP2 (inclusive as subpastas de idioma), cada superquadro
(nBlockAlign bytes a partir do bloco "data") vira uma chave crc32<<32|adler32 -> (arquivo, índice).
Formato (little-endian):
  "GPS2", u32 arquivos, u32 chaves
  arquivos: u32 setor na ISO, u32 tamanho, u32 amostras do Vita (fact), u32 amostras por superquadro,
            u32 canais do Vita, u32 reservado
  chaves (ordenadas): u64 chave, u32 arquivo, u32 superquadro
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
import ps2_audio  # noqa: E402
import psarc_gxp as P  # noqa: E402
import texpack_ps2  # noqa: E402


def riff(d):
    i, info = 12, {}
    while i + 8 <= len(d):
        cid, sz = d[i:i + 4], struct.unpack_from('<I', d, i + 4)[0]
        if cid == b'fmt ':
            info['ch'] = struct.unpack_from('<H', d, i + 10)[0]
            info['align'] = struct.unpack_from('<H', d, i + 20)[0]
            info['sps'] = struct.unpack_from('<H', d, i + 26)[0]  # wSamplesPerBlock da extensão
        elif cid == b'fact':
            info['samples'] = struct.unpack_from('<I', d, i + 8)[0]
        elif cid == b'data':
            info['data'] = (i + 8, sz)
            break
        i += 8 + sz + (sz & 1)
    return info


MAX_DUR_DIFF = float(os.environ.get('PS2_AUDIO_MAX_DUR_DIFF', '0.03'))


def main():
    psarc, iso_path, out = sys.argv[1:4]
    iso = open(iso_path, 'rb')
    ps2 = texpack_ps2.toc_any(iso)  # GoW1 ou GoW2
    f = open(psarc, 'rb')
    ents, zs, block = P.entries(f)
    names = P.read_file(f, ents[0], zs, block).decode('latin1').split('\n')
    files, keys, miss = [], [], []
    for k, name in enumerate(names):
        low = name.lower()
        if not (low.startswith('wad/_sound/dvd_streams/psp2/') and low.endswith('.at9')):
            continue
        base = name.rsplit('/', 1)[-1].rsplit('.', 1)[0].upper()
        pair = next((ps2[base + e] for e in ('.VPK', '.VAG') if base + e in ps2), None)
        d = P.read_file(f, ents[k + 1], zs, block)
        info = riff(d)
        if not pair or 'data' not in info or not info.get('align'):
            miss.append(name)
            continue
        size, lba = pair
        # duração do PS2 (cabeçalho) x do Vita: as dublagens do Vita (French/...) apontam para o nome em inglês
        # e têm outra duração; esses ficam com o áudio do Vita (a troca esticaria a fala). GoW2: 406 de 408 em
        # inglês batem a menos de 2%.
        iso.seek(lba * 2048)
        h = iso.read(0x30)
        if h[:4] == b'VAGp':
            psz, rate = struct.unpack_from('>II', h, 12)
        else:
            psz, _, _, rate = struct.unpack_from('<IIII', h, 4)
        ratio = (psz // 16 * 28 / rate) / (info['samples'] / 48000) if rate and info.get('samples') else 0
        if abs(ratio - 1) > MAX_DUR_DIFF:
            miss.append('%s (duração %.2fx)' % (name, ratio))
            continue
        fi = len(files)
        files.append((lba, size, info['samples'], info['sps'], info['ch'], 0))
        off, sz = info['data']
        a = info['align']
        for s in range(sz // a):
            sf = d[off + s * a:off + (s + 1) * a]
            keys.append(((zlib.crc32(sf) << 32) | zlib.adler32(sf), fi, s))
    keys.sort()
    with open(out, 'wb') as o:
        o.write(b'GPS2' + struct.pack('<II', len(files), len(keys)))
        for e in files:
            o.write(struct.pack('<6I', *e))
        for key, fi, s in keys:
            o.write(struct.pack('<QII', key, fi, s))
    uniq = len(set(k for k, _, _ in keys))
    print(f'{len(files)} arquivos com par no PS2, {len(miss)} sem; {len(keys)} superquadros, {uniq} chaves distintas -> {out}')
    for m in miss[:10]:
        print('  sem par:', m)


if __name__ == '__main__':
    main()
