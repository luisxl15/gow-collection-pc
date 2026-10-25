"""Aplica um patch xdelta numa ISO do GoW2 de PS2 sem gravar a ISO resultante: guarda só os arquivos do
GODOFWAR.TOC que o patch muda (WADs do Kratos, armas, efeitos...).

    python tools/ps2_xdelta_wads.py <xdelta3.exe> <patch.xdelta | mod.zip> <iso original> <pasta de saída>

O patch pode estar dentro de um .zip (lido em fluxo, sem extrair). Duas passadas pelo xdelta3 (-d, saída no stdout):
  1. guarda os primeiros 64 MB de cada camada do DVD (descritores ISO9660, diretórios e o GODOFWAR.TOC);
  2. com o TOC novo, recolhe cada arquivo, compara com o da ISO original pelo nome e grava os diferentes.
Espaço em disco: só os arquivos mudados. Ver docs/mod-gow3-ps2-edition.md.
"""
import hashlib
import os
import struct
import subprocess
import sys
import threading
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps2_audio  # noqa: E402
import texpack_ps2  # noqa: E402

SECTOR = 2048
HEAD = 64 << 20


def patch_stream(path):
    if path.lower().endswith('.zip'):
        z = zipfile.ZipFile(path)
        name = next(n for n in z.namelist() if n.lower().endswith(('.xdelta', '.vcdiff', '.xd3')))
        return z.open(name)
    return open(path, 'rb')


def decode(xd, patch, iso):
    """Gerador de pedaços da ISO com o patch, em ordem."""
    p = subprocess.Popen([xd, '-d', '-c', '-s', iso], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def feed():
        with patch_stream(patch) as src:
            while True:
                b = src.read(1 << 20)
                if not b:
                    break
                p.stdin.write(b)
        p.stdin.close()

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    while True:
        b = p.stdout.read(4 << 20)
        if not b:
            break
        yield b
    t.join()
    if p.wait() != 0:
        raise SystemExit('xdelta3 falhou (código %d)' % p.returncode)


class Sparse:
    """Arquivo esparso em memória (trechos guardados da ISO nova) com seek/read, para os leitores de ISO."""

    def __init__(self):
        self.parts = {}  # início -> bytearray
        self.pos = 0

    def seek(self, p):
        self.pos = p

    def read(self, n):
        for s, b in self.parts.items():
            if s <= self.pos and self.pos + n <= s + len(b):
                r = bytes(b[self.pos - s:self.pos - s + n])
                self.pos += n
                return r
        self.pos += n
        return bytes(n)  # fora do que foi guardado (ex.: a 2ª camada de uma ISO de camada única): zeros


def pass1(xd, patch, iso):
    sp = Sparse()
    sp.parts[0] = bytearray()
    base2 = None
    off = 0
    for b in decode(xd, patch, iso):
        if off < HEAD:
            sp.parts[0] += b[:HEAD - off]
            if base2 is None and len(sp.parts[0]) >= 17 * SECTOR:
                # fim declarado da 1ª camada - 16 = início da 2ª (ps2_audio.part2_lba)
                base2 = (struct.unpack_from('<I', sp.parts[0], 16 * SECTOR + 80)[0] - 16) * SECTOR
                sp.parts[base2] = bytearray()
        if base2 is not None and base2 < off + len(b) and off < base2 + HEAD:
            lo, hi = max(off, base2), min(off + len(b), base2 + HEAD)
            sp.parts[base2] += b[lo - off:hi - off]
        off += len(b)
        if off % (1 << 30) < len(b):
            print('  passada 1: %.1f GB' % (off / 1e9), flush=True)
    sp.parts = {s: v for s, v in sp.parts.items() if v}
    print('  ISO nova: %.2f GB' % (off / 1e9))
    return sp


def toc_new(iso):
    """Como texpack_ps2.toc_any (GoW2), mas a ISO do mod pode ser de camada única: aí o PART2.PAK está no mesmo volume."""
    tlba, tsize = ps2_audio.iso_file(iso, 'GODOFWAR.TOC')
    iso.seek(tlba * SECTOR)
    t = iso.read(tsize)
    n = struct.unpack_from('<I', t, 0)[0]
    secs_at = 4 + n * 36
    p1 = ps2_audio.iso_file(iso, 'PART1.PAK')[0]
    p2 = ps2_audio.part2_lba(iso)
    if p2 is None:
        try:
            p2 = ps2_audio.iso_file(iso, 'PART2.PAK')[0]
        except KeyError:
            p2 = None
    out = {}
    for i in range(n):
        o = 4 + i * 36
        name = t[o:o + 24].split(b'\0')[0].decode('latin1')
        size, cnt, idx = struct.unpack_from('<3I', t, o + 24)
        if not cnt or secs_at + 4 * idx + 4 > tsize:
            continue
        v = struct.unpack_from('<I', t, secs_at + 4 * idx)[0]
        if v >= 10000000 and p2 is None:
            print('  sem PART2.PAK para', name)
            continue
        out[name] = (size, p1 + v if v < 10000000 else p2 + v - 10000000)
    return out


def main():
    xd, patch, iso, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    with open(iso, 'rb') as f:
        old = texpack_ps2.toc_any(f)
    print('TOC original: %d arquivos' % len(old))
    sp = pass1(xd, patch, iso)
    new = toc_new(sp)
    print('TOC novo: %d arquivos' % len(new))
    added = sorted(set(new) - set(old))
    print('  novos no TOC:', added[:20], '...' if len(added) > 20 else '')
    # passada 2: recolhe cada arquivo do TOC novo
    spans = sorted((sec * SECTOR, sec * SECTOR + size, name) for name, (size, sec) in new.items())
    changed = []
    with open(iso, 'rb') as fo:
        def old_bytes(name):
            size, sec = old[name]
            fo.seek(sec * SECTOR)
            return fo.read(size)

        i, off, cur = 0, 0, {}
        for b in decode(xd, patch, iso):
            end = off + len(b)
            while i < len(spans) and spans[i][0] < end:
                cur[spans[i][2]] = [spans[i][0], spans[i][1], bytearray()]
                i += 1
            for name in list(cur):
                s, e, buf = cur[name]
                lo, hi = max(s, off), min(e, end)
                if lo < hi:
                    buf += b[lo - off:hi - off]
                if e <= end:
                    del cur[name]
                    if name not in old or hashlib.md5(buf).digest() != hashlib.md5(old_bytes(name)).digest():
                        with open(os.path.join(out, name), 'wb') as w:
                            w.write(buf)
                        changed.append((name, len(buf), name not in old))
            off = end
            if off % (1 << 30) < len(b):
                print('  passada 2: %.1f GB, %d mudados' % (off / 1e9, len(changed)), flush=True)
    with open(os.path.join(out, 'mudados.tsv'), 'w') as w:
        for name, size, is_new in changed:
            w.write('%s\t%d\t%s\n' % (name, size, 'novo' if is_new else 'mudado'))
    print('%d arquivos mudados -> %s' % (len(changed), out))


if __name__ == '__main__':
    main()
