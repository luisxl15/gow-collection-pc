"""Insere (ou remove) a casca de diagnóstico do diferencial de função num C gerado.

A casca chama arc_platform_log("@SNAP-ENTRY"/"@SNAP-EXIT") em volta da N-ésima chamada da função; o
runtime grava o estado em VITART_SNAP_DIR/entry e /exit, e tools/func_diff.py compara com o Unicorn.

uso: snap_wrap.py <dir_gen> <endereço_hex> <n_chamada>     (insere; troca a casca anterior)
     snap_wrap.py <dir_gen> --remove                         (remove todas)
"""
import glob
import os
import re
import sys

BEGIN = "/* @SNAP-WRAP begin */"
END = "/* @SNAP-WRAP end */"


def remove(path, s):
    m = re.search(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", s, re.S)
    if not m:
        return s
    name = re.search(r"void (\w+)_impl\(arc_ctx \*ctx\);", m.group(0)).group(1)
    s = s[:m.start()] + s[m.end():]
    s = s.replace("void %s_impl(arc_ctx *ctx) {" % name, "void %s(arc_ctx *ctx) {" % name, 1)
    return s


def main():
    gen = sys.argv[1]
    files = sorted(glob.glob(os.path.join(gen, "arc_funcs_*.c")))
    changed = []
    for f in files:
        s = open(f, encoding="utf-8").read()
        t = s
        while True:
            u = remove(f, t)
            if u == t:
                break
            t = u
        if t != s:
            open(f, "w", encoding="utf-8").write(t)
            changed.append(f)
    if sys.argv[2] == "--remove":
        print("removida de:", changed)
        return
    addr = int(sys.argv[2], 16)
    n = int(sys.argv[3])
    pat = re.compile(r"void (arc_%08xt?_\w+)\(arc_ctx \*ctx\) \{" % addr)
    for f in files:
        s = open(f, encoding="utf-8").read()
        m = pat.search(s)
        if not m:
            continue
        name = m.group(1)
        s = s.replace(m.group(0), "void %s_impl(arc_ctx *ctx) {" % name, 1)
        s += """%s
void arc_platform_log(const char *msg);
void %s_impl(arc_ctx *ctx);
void %s(arc_ctx *ctx) {
    static int calls;
    const int me = ++calls;
    if (me == %d) arc_platform_log("@SNAP-ENTRY %08x");
    %s_impl(ctx);
    if (me == %d) arc_platform_log("@SNAP-EXIT %08x");
}
%s
""" % (BEGIN, name, name, n, addr, name, n, addr, END)
        open(f, "w", encoding="utf-8").write(s)
        changed.append(f)
        print("casca em", name, "de", f)
        print("recompilar:", sorted(set(changed)))
        return
    sys.exit("função %08x não encontrada" % addr)


if __name__ == "__main__":
    main()
