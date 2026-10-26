"""vitart Mod Kit: biblioteca e linha de comando do carregador de mods do vitart (docs/modding/README.md).
A interface gráfica é o tools/vitart_modstudio.py.

    python tools/vitart_modkit.py list
    python tools/vitart_modkit.py enable|disable <id>
    python tools/vitart_modkit.py rebuild
    python tools/vitart_modkit.py convert-ps2 <gow2> <patch.xdelta|mod.zip> <ISO|.7z> <id> "<nome>" [--author A]
    python tools/vitart_modkit.py textures <gow1|gow2> <wad/..wad_psp2>                 # lista as texturas
    python tools/vitart_modkit.py export-tex <jogo> <wad> <registro ~...> <saída.png>
    python tools/vitart_modkit.py texture-mod <jogo> <id> "<nome>" <wad>=<registro>=<png> [...]
    python tools/vitart_modkit.py text-mod <jogo> <id> "<nome>" <textos.txt> [--wad wad/_VitaTV/r_perma.wad_psp2]

Um mod é uma pasta em <vitart-data>/mods/<id>/:
    mod.ini        name, game (gow1/gow2), author, version, description
    files/<caminho no psarc>   arquivos inteiros que substituem os do jogo (ex.: files/wad/_VitaTV/r_hero01.wad_psp2)
    src/<caminho no psarc>     arquivos que o patch do vitart também muda (r_perma, r_shella): o rebuild aplica por
                               cima deles o menu e os ícones do vitart e grava o resultado em files/
    entries.tsv    gerado pelo rebuild: "<psarc>\\t<entrada da TOC>\\tfiles/<caminho>" (o que o vitart lê)
    records.tsv    troca de registros dentro de um WAD (ex.: os textos MSGS_TXT de uma tradução):
                   "<psarc>\\t<entrada>\\t<tag>\\t<registro>\\t<ocorrência>\\t<arquivo ou =valor>"
<vitart-data>/mods/modlist.tsv dá a ordem (e o índice na página Mods do menu de pausa); ligar = mod_<id>=1 no
vitart_options.ini. No jogo, Options > Mods > Apply liga/desliga na hora os mods de arquivo inteiro (com o herói, o
jogo volta ao último checkpoint); registros (records.tsv) e o r_perma/r_shella valem ao abrir o jogo de novo.
"""
import configparser
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

PSARC = {'gow1': 'gow1_psp2_SCEA.psarc', 'gow2': 'gow2_psp2_SCEA.psarc'}
# arquivos que o patch do próprio vitart muda (tools/gow_menu_mod.py): um mod que os troca guarda a versão dele em src/
BASE_FILES = ('wad/r_perma.wad_psp2', 'wad/_VitaTV/r_perma.wad_psp2', 'wad/r_shella.wad_psp2', 'wad/_VitaTV/r_shella.wad_psp2')
CONFIG = os.path.join(TOOLS, 'modkit_config.json')


# ------------------------------------------------------------------ configuração
def default_config():
    proj = os.path.dirname(REPO)
    return {
        'app': os.path.join(proj, '_deps', 'vita3k-win', 'bin', 'portable', 'fs', 'ux0', 'app', 'PCSA00126'),
        'data': os.path.join(proj, 'gow-dump', 'vitart-data'),
        'work': os.path.join(proj, 'gow-dump', 'modkit_work'),
        'xdelta3': os.path.join(TOOLS, 'bin', 'xdelta3.exe'),
        'sevenzip': r'C:\Program Files\7-Zip\7z.exe',
        'python': sys.executable,
    }


def load_config():
    cfg = default_config()
    if os.path.exists(CONFIG):
        cfg.update(json.load(open(CONFIG, encoding='utf-8')))
    return cfg


def save_config(cfg):
    json.dump(cfg, open(CONFIG, 'w', encoding='utf-8'), indent=2, ensure_ascii=False)


def mods_dir(cfg):
    return os.path.join(cfg['data'], 'mods')


def psarc_path(cfg, game):
    return os.path.join(cfg['app'], 'exec', PSARC[game])


# ------------------------------------------------------------------ psarc
_MANIFEST = {}


def manifest(psarc):
    """Nomes do psarc na ordem da TOC (entrada k+1 = nomes[k])."""
    import psarc_gxp as P
    if psarc not in _MANIFEST:
        with open(psarc, 'rb') as fh:
            ents, zs, block = P.entries(fh)
            _MANIFEST[psarc] = (P.read_file(fh, ents[0], zs, block).decode('latin1').split('\n'), ents, zs, block)
    return _MANIFEST[psarc][0]


def read_psarc_file(psarc, name):
    import psarc_gxp as P
    names, ents, zs, block = manifest(psarc), *_MANIFEST[psarc][1:]
    with open(psarc, 'rb') as fh:
        return P.read_file(fh, ents[names.index(name) + 1], zs, block)


# ------------------------------------------------------------------ WAD (GoW1: dados na tag 30; GoW2: tag 1)
def wad_records(d, game):
    nobody = (0, 2, 3, 24) if game == 'gow2' else (24,)
    o, out = 0, []
    while o + 32 <= len(d):
        tag, flags, size = struct.unpack_from('<HHI', d, o)
        name = d[o + 8:o + 32].split(b'\0')[0].decode('latin1')
        nb = tag in nobody
        out.append([tag, flags, name, d[o + 32:o + 32 + (0 if nb else size)], not nb, size])
        o = (o + 32 + (0 if nb else size) + 15) & ~15
    return out


def wad_write(recs):
    import gow2_mod_wad
    return gow2_mod_wad.write(recs)


def is_texture(rec):
    b = rec[3]
    return rec[4] and rec[2].startswith('~') and len(b) >= 14 + 64 and b[:4] == b'\x07\x00\x07\x00' and b[14:18] == b'GXT\0'


def wad_textures(wad, game):
    """[(nome do registro, largura, altura, formato)] das texturas do Vita do WAD."""
    out = []
    for r in wad_records(wad, game):
        if is_texture(r):
            off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', r[3], 14 + 32)
            out.append((r[2], w, h, {0x85000000: 'BC1', 0x86000000: 'BC2', 0x87000000: 'BC3', 0x95000000: 'P8',
                                      0x0C000000: 'RGBA8'}.get(fmt & 0xFF000000, hex(fmt))))
    return out


def texture_rgba(wad, game, recname):
    """Decodifica a textura (o 1o nível) para um array RGBA."""
    import gxm_tex_dump as D
    import texpack_match as TM
    rec = next(r for r in wad_records(wad, game) if r[2] == recname)
    gxt = rec[3][14:]
    ti, t, poff = TM.gxt_textures(gxt)[0]
    return D.decode(TM.GxtMem(gxt), t)


def replace_texture(wad, game, recname, rgba):
    """Troca a textura do registro '~' por rgba (BC3 com mipmaps; qualquer tamanho potência de 2)."""
    import gow2_mod_wad
    recs = wad_records(wad, game)
    for r in recs:
        if r[2] == recname and is_texture(r):
            r[3] = gow2_mod_wad.tex_record(r[3], rgba)
            return wad_write(recs)
    raise KeyError(recname)


# ------------------------------------------------------------------ mods instalados
class Mod:
    def __init__(self, cfg, mid):
        self.id = mid
        self.dir = os.path.join(mods_dir(cfg), mid)
        ini = configparser.ConfigParser(interpolation=None)
        p = os.path.join(self.dir, 'mod.ini')
        if os.path.exists(p):
            txt = open(p, encoding='utf-8').read()
            ini.read_string('[mod]\n' + txt if not txt.lstrip().startswith('[') else txt)
        sec = ini[ini.sections()[0]] if ini.sections() else {}
        self.name = sec.get('name', mid)
        self.game = sec.get('game', 'gow2')
        self.author = sec.get('author', '')
        self.version = sec.get('version', '')
        self.description = sec.get('description', '')
        self.enabled = False

    def files(self, sub='files'):
        root = os.path.join(self.dir, sub)
        out = []
        for d, _, fs in os.walk(root):
            for f in fs:
                out.append(os.path.relpath(os.path.join(d, f), root).replace('\\', '/'))
        return sorted(out)

    def size(self):
        return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(self.dir) for f in fs)


def ini_path(cfg):
    return os.path.join(cfg['data'], 'vitart_options.ini')


def enabled_ids(cfg):
    on = set()
    if os.path.exists(ini_path(cfg)):
        for line in open(ini_path(cfg), encoding='utf-8', errors='replace'):
            m = re.match(r'mod_([A-Za-z0-9_]+)=(\d+)', line.strip())
            if m and int(m.group(2)):
                on.add(m.group(1))
    return on


def set_enabled(cfg, mid, on):
    """mod_<id>=0/1 no vitart_options.ini, mantendo as outras linhas."""
    lines = open(ini_path(cfg), encoding='utf-8', errors='replace').read().splitlines() if os.path.exists(ini_path(cfg)) else []
    key = 'mod_%s=' % mid
    lines = [l for l in lines if not l.startswith(key)] + [key + ('1' if on else '0')]
    open(ini_path(cfg), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')


def read_modlist(cfg):
    out = []
    p = os.path.join(mods_dir(cfg), 'modlist.tsv')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            f = line.rstrip('\r\n').split('\t')
            if len(f) >= 3:
                out.append(f[2])
    return out


def list_mods(cfg):
    root = mods_dir(cfg)
    ids = [d for d in sorted(os.listdir(root)) if os.path.isfile(os.path.join(root, d, 'mod.ini'))] if os.path.isdir(root) else []
    order = [i for i in read_modlist(cfg) if i in ids] + [i for i in ids if i not in read_modlist(cfg)]
    on = enabled_ids(cfg)
    mods = []
    for mid in order:
        m = Mod(cfg, mid)
        m.enabled = mid in on
        mods.append(m)
    return mods


def write_modlist(cfg, mods):
    """mods/modlist.tsv: "<jogo>\\t<índice no menu do jogo>\\t<id>\\t<nome>" (até 16 por jogo)."""
    lines, idx = [], {}
    for m in mods:
        i = idx.get(m.game, 0)
        if i >= 16:
            continue
        idx[m.game] = i + 1
        lines.append('%s\t%d\t%s\t%s' % (m.game, i, m.id, m.name.replace('\t', ' ')))
    os.makedirs(mods_dir(cfg), exist_ok=True)
    open(os.path.join(mods_dir(cfg), 'modlist.tsv'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')


def safe_id(text):
    s = re.sub(r'[^A-Za-z0-9_]+', '_', text.strip()).strip('_').lower()
    return s or 'mod'


def install_mod(cfg, mid, name, game, files, author='', version='1.0', description='', log=print):
    """Cria (ou substitui) <mods>/<id> com os arquivos {caminho no psarc: bytes}. Não reconstrói (rebuild)."""
    mid = safe_id(mid)
    d = os.path.join(mods_dir(cfg), mid)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for name_in_psarc, body in files.items():
        sub = 'src' if name_in_psarc in BASE_FILES else 'files'
        dst = os.path.join(d, sub, *name_in_psarc.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(body)
    ini = ['# Mod do vitart (docs/modding/README.md)', 'name=%s' % name, 'game=%s' % game, 'author=%s' % author,
           'version=%s' % version, 'description=%s' % description.replace('\n', ' ')]
    open(os.path.join(d, 'mod.ini'), 'w', encoding='utf-8', newline='\n').write('\n'.join(ini) + '\n')
    log('mod %s instalado em %s (%d arquivos)' % (mid, d, len(files)))
    return mid


def run(cmd, log=print, cwd=None):
    """Roda um comando e manda a saída para log, linha a linha."""
    log('> ' + ' '.join('"%s"' % c if ' ' in c else c for c in cmd))
    env = dict(os.environ, PYTHONIOENCODING='utf-8')  # as ferramentas imprimem em português
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, text=True, encoding='utf-8',
                         errors='replace', env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    for line in p.stdout:
        log(line.rstrip())
    if p.wait() != 0:
        raise RuntimeError('falhou (código %d): %s' % (p.returncode, cmd[1] if len(cmd) > 1 else cmd[0]))


def rebuild(cfg, log=print):
    """Lista de mods, patch base de cada jogo (menu com a página Mods) e os arquivos src/ dos mods com o menu do
    vitart por cima; depois o entries.tsv de cada mod."""
    mods = list_mods(cfg)
    write_modlist(cfg, mods)
    modlist = os.path.join(mods_dir(cfg), 'modlist.tsv')
    py = [cfg['python'], '-P', os.path.join(TOOLS, 'gow_menu_mod.py')]
    for game in ('gow1', 'gow2'):
        ps = psarc_path(cfg, game)
        if not os.path.exists(ps):
            continue
        log('== %s: patch base' % game)
        run(py + [ps, os.path.join(mods_dir(cfg), PSARC[game] + '.vtpatch'), '--modlist', modlist], log)
        for m in mods:
            if m.game != game or not m.files('src'):
                continue
            log('== %s: %s com o menu do vitart' % (game, ', '.join(m.files('src'))))
            tmp = tempfile.mkdtemp(prefix='modkit_')
            args = []
            for rel in sorted(m.files('src'), key=lambda r: '_VitaTV' in r):  # o normal antes do PS TV
                args += ['--wad', '%s=%s' % (rel, os.path.join(m.dir, 'src', *rel.split('/')))]
            run(py + [ps, '-', '--modlist', modlist, '--files-out', tmp] + args, log)
            for rel in BASE_FILES:
                src = os.path.join(tmp, *rel.split('/'))
                if os.path.exists(src) and (rel in m.files('src') or rel.replace('wad/_VitaTV/', 'wad/') in m.files('src')):
                    dst = os.path.join(m.dir, 'files', *rel.split('/'))
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copyfile(src, dst)
            shutil.rmtree(tmp, ignore_errors=True)
    for m in mods:
        ps = psarc_path(cfg, m.game)
        if not os.path.exists(ps):
            continue
        names = manifest(ps)
        lines = []
        for rel in m.files('files'):
            if rel in names:
                lines.append('%s\t%d\tfiles/%s' % (PSARC[m.game], names.index(rel) + 1, rel))
            else:
                log('aviso: %s não existe no %s (ignorado)' % (rel, PSARC[m.game]))
        open(os.path.join(m.dir, 'entries.tsv'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
        log('%s: %d arquivos em entries.tsv' % (m.id, len(lines)))
    log('pronto: %d mods' % len(mods))


# ------------------------------------------------------------------ conversão de mods do PS2
def convert_ps2(cfg, game, patch, iso, mid, name, author='', description='', log=print, keep_work=False):
    """Mod de PS2 distribuído como xdelta (solto ou dentro de um .zip) -> mod do vitart, instalado e com rebuild.
    iso: a ISO original do PS2 (.iso, ou o .7z que a contém). Só o GoW2 por enquanto."""
    if game != 'gow2':
        raise ValueError('a conversão de mods do PS2 ainda só existe para o GoW2 (docs/modding/README.md)')
    for k in ('xdelta3',):
        if not os.path.exists(cfg[k]):
            raise FileNotFoundError('%s não encontrado em %s (configure em Configurações)' % (k, cfg[k]))
    work = os.path.join(cfg['work'], safe_id(mid))
    os.makedirs(work, exist_ok=True)
    py = [cfg['python'], '-P']
    try:
        if iso.lower().endswith('.7z'):
            log('== extraindo a ISO do .7z (temporária)')
            run([cfg['sevenzip'], 'e', '-y', '-o' + os.path.join(work, 'iso'), iso, '*.iso'], log)
            iso = next(os.path.join(work, 'iso', f) for f in os.listdir(os.path.join(work, 'iso')) if f.lower().endswith('.iso'))
        ps2 = os.path.join(work, 'ps2')
        log('== aplicando o patch (fluxo, só os arquivos que mudam)')
        run(py + [os.path.join(TOOLS, 'ps2_xdelta_wads.py'), cfg['xdelta3'], patch, iso, ps2], log)
        log('== originais dos WADs mudados')
        import texpack_ps2
        os.makedirs(os.path.join(ps2, 'orig'), exist_ok=True)
        changed = [l.split('\t')[0] for l in open(os.path.join(ps2, 'mudados.tsv'))]
        with open(iso, 'rb') as f:
            toc = texpack_ps2.toc_any(f)
            for n in changed:
                if n.upper().endswith('.WAD') and n in toc:
                    size, sec = toc[n]
                    f.seek(sec * 2048)
                    open(os.path.join(ps2, 'orig', n), 'wb').write(f.read(size))
        for n in changed:  # vídeos e outros arquivos do PS2 não servem ao Vita
            if not n.upper().endswith('.WAD'):
                try:
                    os.remove(os.path.join(ps2, n))
                except OSError:
                    pass
        vita = os.path.join(work, 'vita')
        log('== convertendo os WADs para o formato do Vita')
        run(py + [os.path.join(TOOLS, 'gow2_mod_wad.py'), '--all', ps2, psarc_path(cfg, game), vita], log)
        files = {}
        for line in open(os.path.join(vita, 'wad_args.txt'), encoding='utf-8'):
            line = line.strip()
            if line.startswith('--wad '):
                dst, src = line[6:].split('=', 1)
                files[dst] = open(src, 'rb').read()
        mid = install_mod(cfg, mid, name, game, files, author=author, description=description, log=log)
        rebuild(cfg, log)
        return mid
    finally:
        if not keep_work:
            shutil.rmtree(work, ignore_errors=True)


def wad_variants(wad):
    """O WAD normal e o do PS TV (wad/_VitaTV/, o que o vitart carrega)."""
    base = wad.replace('wad/_VitaTV/', 'wad/', 1)
    return (base, base.replace('wad/', 'wad/_VitaTV/', 1))


def make_texture_mod(cfg, mid, name, game, changes, author='', description='', log=print):
    """changes: [(wad no psarc, registro '~...', RGBA)]. A troca vale nas duas versões do WAD (normal e _VitaTV)."""
    import numpy as np  # noqa: F401
    ps = psarc_path(cfg, game)
    names = manifest(ps)
    out = {}
    for wad, rec, rgba in changes:
        for variant in wad_variants(wad):
            if variant not in names:
                continue
            body = out.get(variant) or read_psarc_file(ps, variant)
            try:
                out[variant] = replace_texture(body, game, rec, rgba)
            except KeyError:
                log('%s: sem o registro %s (fica como está)' % (variant, rec))
    mid = install_mod(cfg, mid, name, game, out, author=author, description=description, log=log)
    rebuild(cfg, log)
    return mid


# ------------------------------------------------------------------ mods de texto (tradução)
TEXT_WAD = 'wad/_VitaTV/r_perma.wad_psp2'  # os textos que o vitart mostra (o modo PS TV)
# A fonte do GoW2 (a única do FLP_HUDA/FLP_ShellA, 132 glifos) usa os códigos das maiúsculas acentuadas para os
# ícones dos botões (medido: Á Â Ä Ç É Ê Ó Ô Ú saem como ícones; Í e as minúsculas saem certas) e não tem ã/õ/Ã/Õ.
FONT_FIX = {'gow2': str.maketrans({'Á': 'A', 'Â': 'A', 'Ä': 'A', 'Ã': 'A', 'À': 'A', 'Ç': 'C', 'É': 'E', 'Ê': 'E',
                                   'Ó': 'O', 'Ô': 'O', 'Õ': 'O', 'Ö': 'O', 'Ú': 'U', 'ã': 'ä', 'õ': 'ö'})}


def msgs_need(text):
    """(mensagens, linhas) que o jogo conta ao ler MSGS_TXT (0x81019f7e no GoW2): strtok por CR/LF, pula linhas
    vazias e '//', '*N*' (ou '*N*H'/'*N*T') abre uma mensagem; MSGS_COUNT e MSGS_LINES dimensionam as tabelas."""
    text = text.split(b'\0')[0]
    if text[:3] == b'\xef\xbb\xbf':
        text = text[3:]
    count = lines = 0
    pending = False
    for t in re.split(rb'[\r\n]+', text):
        if not t or t.startswith(b'//'):
            continue
        if t.startswith(b'*') and (t.endswith(b'*') or (len(t) > 2 and t[-2:-1] == b'*' and t[-1:] in b'HT')):
            pending = True
            continue
        if pending:
            count, pending = count + 1, False
        lines += 1
    return count, lines


def make_text_mod(cfg, mid, name, game, txt_path, wad=TEXT_WAD, author='', description='', log=print):
    """Mod de textos: o .txt (formato do MSGS_TXT: '*N*' e as linhas da mensagem, UTF-8) troca o registro MSGS_TXT
    do WAD, e MSGS_COUNT/MSGS_LINES crescem se a tradução precisar. Vale ao abrir o jogo (o r_perma só é lido aí)."""
    ps = psarc_path(cfg, game)
    names = manifest(ps)
    if wad not in names:
        raise ValueError('%s não existe no %s' % (wad, PSARC[game]))
    raw = open(txt_path, 'rb').read()
    if raw[:3] == b'\xef\xbb\xbf':
        raw = raw[3:]
    text = raw.decode('utf-8')  # o jogo lê UTF-8: falha cedo se o arquivo for outra codificação
    if game in FONT_FIX:
        fixed = text.translate(FONT_FIX[game])
        if fixed != text:
            log('%d letras trocadas por outras que a fonte do jogo tem (maiúsculas acentuadas, ã/õ)'
                % sum(a != b for a, b in zip(text, fixed)))
        raw = fixed.encode('utf-8')
    body =re.sub(rb'\r?\n', b'\r\n', raw.rstrip(b'\0'))
    if not body.endswith(b'\r\n'):
        body += b'\r\n'
    body += b'\0\0'  # o jogo percorre o texto com strtok: precisa do zero no fim
    recs = {r[2]: r for r in wad_records(read_psarc_file(ps, wad), game) if r[2].startswith('MSGS_')}
    if 'MSGS_TXT' not in recs:
        raise ValueError('%s não tem MSGS_TXT' % wad)
    count, lines = msgs_need(body)
    count, lines = max(count, recs['MSGS_COUNT'][5]), max(lines, recs['MSGS_LINES'][5])
    mid = install_mod(cfg, mid, name, game, {}, author=author, description=description, log=log)
    d = os.path.join(mods_dir(cfg), mid)
    os.makedirs(os.path.join(d, 'records'), exist_ok=True)
    open(os.path.join(d, 'records', 'MSGS_TXT.txt'), 'wb').write(body)
    idx = names.index(wad) + 1
    rows = ['%s\t%d\t%d\tMSGS_TXT\t0\trecords/MSGS_TXT.txt' % (PSARC[game], idx, recs['MSGS_TXT'][0]),
            '%s\t%d\t%d\tMSGS_COUNT\t0\t=%d' % (PSARC[game], idx, recs['MSGS_COUNT'][0], count),
            '%s\t%d\t%d\tMSGS_LINES\t0\t=%d' % (PSARC[game], idx, recs['MSGS_LINES'][0], lines)]
    open(os.path.join(d, 'records.tsv'), 'w', encoding='utf-8', newline='\n').write('\n'.join(rows) + '\n')
    log('%s: %d mensagens, %d linhas em %s' % (mid, count, lines, wad))
    rebuild(cfg, log)
    return mid


# ------------------------------------------------------------------ linha de comando
def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    cfg = load_config()
    a = sys.argv[1:]
    if not a or a[0] == 'list':
        for m in list_mods(cfg):
            print('[%s] %-20s %-6s %s (%d arquivos, %.1f MB)' % ('x' if m.enabled else ' ', m.id, m.game, m.name,
                                                              len(m.files()), m.size() / 1e6))
    elif a[0] in ('enable', 'disable'):
        set_enabled(cfg, a[1], a[0] == 'enable')
    elif a[0] == 'rebuild':
        rebuild(cfg)
    elif a[0] == 'convert-ps2':
        author = a[a.index('--author') + 1] if '--author' in a else ''
        convert_ps2(cfg, a[1], a[2], a[3], a[4], a[5], author=author)
    elif a[0] == 'textures':
        for t in wad_textures(read_psarc_file(psarc_path(cfg, a[1]), a[2]), a[1]):
            print('%-24s %4dx%-4d %s' % t)
    elif a[0] == 'export-tex':
        from PIL import Image
        Image.fromarray(texture_rgba(read_psarc_file(psarc_path(cfg, a[1]), a[2]), a[1], a[3])).save(a[4])
    elif a[0] == 'texture-mod':
        import numpy as np
        from PIL import Image
        game, mid, name = a[1], a[2], a[3]
        changes = []
        for spec in a[4:]:
            wad, rec, png = spec.split('=', 2)
            changes.append((wad, rec, np.array(Image.open(png).convert('RGBA'))))
        make_texture_mod(cfg, mid, name, game, changes)
    elif a[0] == 'text-mod':
        wad = a[a.index('--wad') + 1] if '--wad' in a else TEXT_WAD
        make_text_mod(cfg, a[2], a[3], a[1], a[4], wad=wad)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
