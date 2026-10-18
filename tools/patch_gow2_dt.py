"""Remendo no código traduzido do GoW2: FPS acima de 60 sem acelerar o jogo (o mesmo do GoW1).

    python tools/patch_gow2_dt.py <pasta gen/gow2>     (depois recompile arc_funcs_013.c e ligue a DLL)

A função 0x8113c742 calcula o dt do quadro (estrutura de tempo em 0x812df6dc; dt em 0x812df6e0):
    dt = max(60 / taxa, clamp(tempo real em quadros de 60 Hz, 1,0, 4,0)) / 60     (flags & 1 e não & 2)
    dt = (60 / taxa) / 60                                                           (senão: passo fixo)
O piso de 1,0 põe pelo menos 1/60 s em cada quadro: acima de 60 FPS o jogo acelerava. Com a palavra livre
0x812df6d0 != 0 (o runtime grava, game_options.cpp) o piso vira esse valor (float, em centésimos de quadro)
e o 60 / taxa sai do max. Com 0 é o código original. Em 0x812df6d4 fica qual caminho foi tomado (0 = tempo
real, 1 = passo fixo).
"""
import sys

MARK = '/* vitart: piso do dt */'
NL = '\n'
patches = [
    # início do caminho de tempo real: o max deixa de incluir o 60 / taxa
    ('L_8113c778:; /* vmov s1, r5 */' + NL + '    ARC_SSB(ctx, 1, r5);' + NL,
     'L_8113c778:; /* vmov s1, r5 */' + NL + '    ARC_SSB(ctx, 1, r5);' + NL +
     '    if (ARC_LW(0x812df6d0u)) ARC_SS(ctx, 0, 0.0f); ' + MARK + NL +
     '    ARC_SW(0x812df6d4u, 0u); ' + MARK + NL),
    # o piso do clamp (100,0 = um quadro de 60 Hz)
    ('L_8113c78e:; /* movt r0, #0x42c8 */' + NL + '    r0 = (r0 & 0xffffu) | 0x42c80000u;' + NL,
     'L_8113c78e:; /* movt r0, #0x42c8 */' + NL + '    { uint32_t vt_floor = ARC_LW(0x812df6d0u); ' + MARK + NL +
     '      r0 = vt_floor ? vt_floor : ((r0 & 0xffffu) | 0x42c80000u); }' + NL),
    # caminho de passo fixo
    ('L_8113c7cc:; /* movw r0, #0xf6e8 */' + NL,
     'L_8113c7cc:; /* movw r0, #0xf6e8 */' + NL + '    ARC_SW(0x812df6d4u, 1u); ' + MARK + NL),
]


def main():
    p = sys.argv[1].rstrip('/\\') + '/arc_funcs_013.c'
    s = open(p, encoding='utf-8').read()
    if MARK in s:
        print('já remendado')
        return
    for a, b in patches:
        assert s.count(a) == 1, 'trecho não encontrado: ' + a.split(NL)[0]
        s = s.replace(a, b)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('remendado:', p)


if __name__ == '__main__':
    main()
