"""Corta um vídeo com todas as cenas remasterizadas nos vídeos do jogo, achando cada um pelo ÁUDIO.

    python tools/cutscene_match.py <vídeo remasterizado> <pasta dos .bk2> <saída> [--ffmpeg caminho]
                                   [--min 0.25] [--encoder hevc_nvenc]

O FFmpeg não decodifica a imagem do Bink 2, mas lê o áudio (binkaudio_dct). Cada .bk2 tem o áudio
correlacionado com o do remaster: primeiro pelo envelope (10 ms, sem a tendência de volume) no vídeo
inteiro, depois pela forma de onda (8 kHz) em volta do melhor ponto. Aceita a nota de onda >= --min e
corta [início, início + duração do .bk2] em HEVC sem som (o jogo toca o próprio). Grava <saída>/index.tsv
para o runtime (runtime/vitart/video_hd.cpp). Vídeos sem par ficam com a imagem original.

Variantes de idioma do mesmo trecho (mesma duração, nome terminado em letra) recebem o mesmo arquivo.
"""
import argparse
import glob
import os
import struct
import subprocess

import numpy as np

SR, HOP = 8000, 80


def pcm(ff, path, mix_streams=True):
    n = 1
    if mix_streams:
        probe = subprocess.run([ff.replace('ffmpeg', 'ffprobe'), '-v', 'error', '-select_streams', 'a', '-show_entries',
                                'stream=index', '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.split()
        n = len(probe)
        if n == 0:
            return None
    args = [ff, '-v', 'error', '-i', path]
    if n > 1:
        args += ['-filter_complex', 'amix=inputs=%d:normalize=0' % n]
    else:
        args += ['-vn']
    args += ['-ac', '1', '-ar', str(SR), '-f', 's16le', '-']
    return np.frombuffer(subprocess.run(args, capture_output=True).stdout, dtype=np.int16).astype(np.float64)


def env(x):
    n = len(x) // HOP
    e = np.log(np.sqrt((x[:n * HOP].reshape(n, HOP) ** 2).mean(1) + 1.0))
    k = 50
    c = np.cumsum(np.pad(e, (k, k), mode='edge'))
    return e - ((c[2 * k:] - c[:-2 * k]) / (2 * k))[:len(e)]


def ncc(big, small):
    """Correlação normalizada de small em cada posição de big."""
    N = 1 << int(np.ceil(np.log2(len(big) + len(small))))
    cc = np.fft.irfft(np.fft.rfft(big, N) * np.conj(np.fft.rfft(small, N)), N)[:len(big) - len(small) + 1]
    c2 = np.concatenate([[0], np.cumsum(big ** 2)])
    win = np.sqrt(np.maximum(c2[len(small):] - c2[:len(big) - len(small) + 1], 1e-9))
    return cc / (win * np.linalg.norm(small) + 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('remaster')
    ap.add_argument('bk2_dir')
    ap.add_argument('out')
    ap.add_argument('--ffmpeg', default='ffmpeg')
    ap.add_argument('--min', type=float, default=0.25)
    ap.add_argument('--encoder', default='hevc_nvenc')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    R = pcm(a.ffmpeg, a.remaster, mix_streams=False)
    total = len(R) / SR
    er = env(R)
    found = []
    for f in sorted(glob.glob(os.path.join(a.bk2_dir, '*.bk2'))):
        name = os.path.basename(f)[:-4]
        hd = open(f, 'rb').read(36)
        frames = struct.unpack_from('<I', hd, 8)[0]
        fn, fd = struct.unpack_from('<II', hd, 28)
        dur = frames * fd / fn
        x = pcm(a.ffmpeg, f)
        if x is None or len(x) < SR:
            continue
        ex = env(x)
        if len(ex) >= len(er):
            continue
        t0 = int(np.argmax(ncc(er, ex - ex.mean()))) * HOP / SR
        # forma de onda em volta (até 20 s do começo do vídeo)
        xs = x[:SR * 20]
        lo = max(0, int((t0 - 1.0) * SR))
        seg = R[lo:int((t0 + 1.0) * SR) + len(xs)]
        if len(seg) < len(xs):
            continue
        sc = ncc(seg, xs)
        i = int(np.argmax(sc))
        start, score = (lo + i) / SR, float(sc[i])
        print('%-10s %8.3f s  nota %.2f  duração %.1f s' % (name, start, score, dur))
        if score >= a.min:
            found.append((name, start, min(dur, total - start)))
    lines = ['# Cenas remasterizadas (runtime/vitart/video_hd.cpp): vídeo do jogo<TAB>arquivo[<TAB>atraso em s]']
    done = {}
    for name, start, dur in found:
        key = round(start, 1)
        if key in done:  # variante do mesmo trecho
            lines.append('%s\t%s' % (name, done[key]))
            continue
        out = name + '.mp4'
        subprocess.check_call([a.ffmpeg, '-v', 'error', '-y', '-ss', '%.3f' % start, '-i', a.remaster, '-t', '%.3f' % dur,
                               '-an', '-c:v', a.encoder, '-preset', 'p5', '-rc', 'vbr', '-cq', '22', '-b:v', '0', '-g', '60',
                               '-pix_fmt', 'yuv420p', os.path.join(a.out, out)])
        done[key] = out
        lines.append('%s\t%s' % (name, out))
    with open(os.path.join(a.out, 'index.tsv'), 'w', encoding='utf-8', newline='\n') as o:
        o.write('\n'.join(lines) + '\n')
    print('%d vídeos com versão remasterizada -> %s' % (len(found), a.out))


if __name__ == '__main__':
    main()
