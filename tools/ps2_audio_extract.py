"""Tira da ISO do GoW1 de PS2 só os sons que o vitart usa, para não depender mais da ISO.

    python tools/ps2_audio_extract.py <iso do PS2> <ps2audio.idx> <pasta de dados do vitart>

Lê o índice (tools/ps2_audio_index.py: setor e tamanho de cada VPK/VAG na ISO), copia cada arquivo para
<dados>/ps2audio.bin alinhado a 2048 bytes e grava <dados>/ps2audio_files.idx: o mesmo formato, com o
setor agora relativo ao ps2audio.bin. O vitart (hle_ngs.cpp) prefere esse par à ISO.
"""
import os
import struct
import sys

SECTOR = 2048


def main():
    iso_path, idx_path, out = sys.argv[1:4]
    d = open(idx_path, 'rb').read()
    magic, nf, nk = struct.unpack_from('<III', d, 0)
    assert magic == 0x32535047, 'índice inválido'
    files = [list(struct.unpack_from('<6I', d, 12 + 24 * i)) for i in range(nf)]
    iso = open(iso_path, 'rb')
    tmp = os.path.join(out, 'ps2audio.bin.tmp')
    sector = 0
    with open(tmp, 'wb') as o:
        for i, f in enumerate(files):
            lba, size = f[0], f[1]
            iso.seek(lba * SECTOR)
            data = iso.read(size)
            assert len(data) == size, 'arquivo %d curto na ISO' % i
            f[0] = sector
            o.write(data)
            pad = (-size) % SECTOR
            o.write(bytes(pad))
            sector += (size + pad) // SECTOR
            if i % 100 == 0:
                print('%d/%d' % (i, nf), flush=True)
    idx = bytearray(d)
    for i, f in enumerate(files):
        struct.pack_into('<6I', idx, 12 + 24 * i, *f)
    with open(os.path.join(out, 'ps2audio_files.idx'), 'wb') as o:
        o.write(idx)
    os.replace(tmp, os.path.join(out, 'ps2audio.bin'))
    print('%d sons, %.0f MB em %s' % (nf, sector * SECTOR / 2 ** 20, os.path.join(out, 'ps2audio.bin')))


if __name__ == '__main__':
    main()
