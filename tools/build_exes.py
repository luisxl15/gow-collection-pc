"""Gera os executáveis do vitart Troféus e do vitart Mod Studio (PyInstaller, um arquivo, sem console).

    python tools/build_exes.py [pasta de saída]     (padrão: ../gow-dump/rt, ao lado do vitart.exe)

O ícone (ômega no medalhão) sai de tools/vitart_tema.py. O Troféus leva o trofeus_guia.json dentro do exe; o
Mod Studio usa as ferramentas do repositório por subprocesso (vitart_modkit acha o repositório pela posição do
exe ou por VITART_REPO) e o Python instalado.
"""
import os
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import vitart_tema as T  # noqa: E402

APPS = [('vitart Trofeus', 'vitart_trofeus.py', ['trofeus_guia.json']),
        ('vitart Mod Studio', 'vitart_modstudio.py', [])]


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(TOOLS, '..', '..', 'gow-dump', 'rt'))
    work = tempfile.mkdtemp(prefix='vitart_exe_')
    ico = os.path.join(work, 'vitart.ico')
    T.app_icon(256).save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    for name, script, data in APPS:
        cmd = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--onefile', '--windowed', '--name', name,
               '--icon', ico, '--paths', TOOLS, '--distpath', out, '--workpath', os.path.join(work, 'build'),
               '--specpath', work, '--exclude-module', 'matplotlib', '--exclude-module', 'scipy']
        for d in data:
            cmd += ['--add-data', '%s%s.' % (os.path.join(TOOLS, d), os.pathsep)]
        cmd.append(os.path.join(TOOLS, script))
        print('==>', name)
        subprocess.run(cmd, check=True)
    print('ok:', out)


if __name__ == '__main__':
    main()
