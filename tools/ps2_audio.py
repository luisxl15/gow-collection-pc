"""Áudio do God of War 1 de PS2: lê GODOFWAR.TOC + PART1.PAK direto da ISO e decodifica VPK/VAG para WAV.

    python tools/ps2_audio.py <iso> list                 # nome, tamanho, setor
    python tools/ps2_audio.py <iso> wav <NOME> <saída.wav>

TOC (arquivo GODOFWAR.TOC da ISO): entradas de 24 bytes = nome[16], tamanho u32, setor u32 (relativo ao
PART1.PAK). Nomes repetidos com sufixo \\x01 são a cópia da segunda camada; vale a primeira.
VPK: " KPV" + tamanho dos dados por canal (0x04), início (0x08), intercalação total (0x0C; cada canal leva metade),
taxa (0x10), canais (0x14); PS-ADPCM. VAG: "VAGp" big-endian, tamanho em 0x0C, taxa em 0x10, mono, dados
em 0x30. Os dois casam pelo nome com os .at9 do psarc do Vita (wad/_sound/dvd_streams/PSP2/<nome>.at9).
"""
import struct
import sys
import wave

SECTOR = 2048
F = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def iso_file(iso, name):
    """(lba, tamanho) de um arquivo na raiz da ISO9660."""
    iso.seek(16 * SECTOR)
    pvd = iso.read(SECTOR)
    lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    iso.seek(lba * SECTOR)
    d = iso.read(size)
    i = 0
    while i < len(d):
        n = d[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        r = d[i:i + n]
        nm = r[33:33 + r[32]].decode('latin1').split(';')[0]
        if nm == name:
            return struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]
        i += n
    raise KeyError(name)


def part2_lba(iso):
    """Início do PART2.PAK: a segunda camada do DVD começa 16 setores antes do fim declarado da primeira
    (volume próprio, com o PVD no setor 16 dela) e as LBAs do diretório dela são relativas a esse início."""
    iso.seek(16 * SECTOR)
    base = struct.unpack_from('<I', iso.read(SECTOR), 80)[0] - 16
    iso.seek((base + 16) * SECTOR)
    pvd = iso.read(SECTOR)
    if pvd[1:6] != b'CD001':
        return None
    lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    iso.seek((base + lba) * SECTOR)
    d = iso.read(size)
    i = 0
    while i < len(d):
        n = d[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        r = d[i:i + n]
        if r[33:33 + r[32]].decode('latin1').split(';')[0] == 'PART2.PAK':
            return base + struct.unpack_from('<I', r, 2)[0]
        i += n
    return None


def toc(iso):
    """nome -> (tamanho, setor absoluto na ISO). Sufixo \\x01 no nome = arquivo do PART2.PAK (segunda
    camada); o mesmo nome nas duas camadas fica com a primeira."""
    tlba, tsize = iso_file(iso, 'GODOFWAR.TOC')
    p1, _ = iso_file(iso, 'PART1.PAK')
    p2 = part2_lba(iso)
    iso.seek(tlba * SECTOR)
    t = iso.read(tsize)
    out = {}
    for i in range(0, len(t) - 23, 24):
        raw = t[i:i + 16].split(b'\0')[0].decode('latin1')
        size, sec = struct.unpack_from('<II', t, i + 16)
        second = raw.endswith('\x01')
        n = raw.rstrip('\x01')
        if not n or (second and p2 is None):
            continue
        if n not in out or (not second and out[n][2]):
            out[n] = (size, (p2 if second else p1) + sec, second)
    return {n: (size, lba) for n, (size, lba, _) in out.items()}


def adpcm(block_stream):
    """PS-ADPCM (16 bytes -> 28 amostras) de um canal."""
    s1 = s2 = 0
    out = []
    for o in range(0, len(block_stream) - 15, 16):
        b = block_stream[o:o + 16]
        shift, filt = b[0] & 15, (b[0] >> 4) & 7
        if filt > 4:
            filt = 0
        f0, f1 = F[filt]
        for k in range(28):
            byte = b[2 + k // 2]
            nib = (byte >> (4 * (k & 1))) & 15
            if nib >= 8:
                nib -= 16
            v = (nib << 12) >> shift
            v += (s1 * f0 + s2 * f1 + 32) >> 6
            v = max(-32768, min(32767, v))
            out.append(v)
            s2, s1 = s1, v
    return out


def decode(data):
    """(taxa, canais, [amostras por canal])."""
    if data[:4] == b' KPV':
        size, start, inter, rate, ch = struct.unpack_from('<IIIII', data, 4)
        body = data[start:start + size * ch]  # tamanho do cabeçalho é por canal
        il = inter // ch
        chans = [bytearray() for _ in range(ch)]
        for o in range(0, len(body), il * ch):
            for c in range(ch):
                chans[c] += body[o + c * il:o + (c + 1) * il]
        return rate, ch, [adpcm(bytes(c)) for c in chans]
    if data[:4] == b'VAGp':
        size, rate = struct.unpack_from('>II', data, 12)
        return rate, 1, [adpcm(data[0x30:0x30 + size])]
    raise ValueError('formato desconhecido: %r' % data[:4])


def main():
    iso = open(sys.argv[1], 'rb')
    files = toc(iso)
    if sys.argv[2] == 'list':
        for n, (size, lba) in sorted(files.items()):
            print('%-16s %10d %8d' % (n, size, lba))
        return
    name, out = sys.argv[3], sys.argv[4]
    size, lba = files[name]
    iso.seek(lba * SECTOR)
    rate, ch, pcm = decode(iso.read(size))
    n = min(len(c) for c in pcm)
    w = wave.open(out, 'wb')
    w.setnchannels(ch)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        for c in range(ch):
            frames += struct.pack('<h', pcm[c][i])
    w.writeframes(bytes(frames))
    w.close()
    print('%s: %d Hz, %d canais, %.2f s -> %s' % (name, rate, ch, n / rate, out))


if __name__ == '__main__':
    main()
