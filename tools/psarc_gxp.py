"""Extrai todos os programas GXP (shaders do Vita) de um .psarc, para pré-traduzir o cache de SPIR-V.

    python tools/psarc_gxp.py <arquivo.psarc> <saída>

Formato PSARC: cabeçalho "PSAR" (big-endian) com compressão, tamanho da TOC, tamanho da entrada, número
de arquivos e tamanho do bloco; entradas de 30 bytes (md5, índice do 1º bloco, tamanho de 40 bits,
deslocamento de 40 bits) seguidas da tabela de tamanhos comprimidos por bloco (2, 3 ou 4 bytes). Bloco
com tamanho 0 = bloco cheio sem compressão; dados que começam com cabeçalho zlib são inflados.
Cada arquivo é varrido atrás de "GXP\\0"; o tamanho do programa está em +8 (little-endian). A saída usa
o FNV-1a de 64 bits do programa como nome, igual ao hash do vitart.
"""
import os
import struct
import sys
import zlib


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def entries(f):
    magic, ver, comp, toc_len, ent_size, n, block, flags = struct.unpack('>4sI4sIIIII', f.read(32))
    assert magic == b'PSAR', magic
    assert comp == b'zlib', comp
    toc = f.read(toc_len - 32)
    ents = []
    for i in range(n):
        e = toc[i * ent_size:(i + 1) * ent_size]
        idx = struct.unpack('>I', e[16:20])[0]
        size = int.from_bytes(e[20:25], 'big')
        off = int.from_bytes(e[25:30], 'big')
        ents.append((idx, size, off))
    tab = toc[n * ent_size:]
    w = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
    zs = [int.from_bytes(tab[k:k + w], 'big') for k in range(0, len(tab) - w + 1, w)]
    return ents, zs, block


def read_file(f, ent, zs, block):
    idx, size, off = ent
    out = bytearray()
    f.seek(off)
    while len(out) < size:
        z = zs[idx]
        want = min(block, size - len(out))
        raw = f.read(z if z else block)
        if z and raw[:1] == b'\x78' and z != want:
            try:
                raw = zlib.decompress(raw)
            except zlib.error:
                pass
        out += raw[:want]
        idx += 1
    return bytes(out)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    seen = set()
    with open(src, 'rb') as f:
        ents, zs, block = entries(f)
        for k, ent in enumerate(ents):
            if k == 0 or ent[1] == 0:  # entrada 0 = manifesto de nomes
                continue
            data = read_file(f, ent, zs, block)
            p = data.find(b'GXP\x00')
            while p >= 0:
                if p + 12 <= len(data):
                    size = struct.unpack_from('<I', data, p + 8)[0]
                    if 32 <= size <= len(data) - p:
                        blob = data[p:p + size]
                        h = fnv1a64(blob)
                        if h not in seen:
                            seen.add(h)
                            open(os.path.join(out, '%016x.gxp' % h), 'wb').write(blob)
                p = data.find(b'GXP\x00', p + 4)
    print(f'{len(ents) - 1} arquivos, {len(seen)} programas GXP distintos -> {out}')


if __name__ == '__main__':
    main()
