"""Remendo no código traduzido do GoW1: FPS acima de 60 sem acelerar o jogo.

    python tools/patch_gow1_dt.py <pasta gen/gow1>     (depois recompile arc_funcs_011.c e ligue a DLL)

A função 0x8110a16e calcula o dt do quadro (estrutura de tempo em 0x812b75dc):
    e8 = 60 / taxa                                       (inteiro; 1 na taxa 60)
    dt = max(e8, clamp(tempo real em quadros de 60 Hz, 1,0, 4,0)) / 60
O piso de 1,0 (constante 100,0 dividida por 100) põe pelo menos 1/60 s em cada quadro: a 120 FPS cada
quadro anda 1/60 s e o jogo fica 2x mais rápido. O remendo lê uma palavra livre do guest, 0x812b75d0
(o runtime grava nela, game_options.cpp): 0 = comportamento original; senão é o piso novo (float, em
centésimos de quadro, p. ex. 25,0 = 1/240 s) e o e8 sai da conta. O e4 (1/60) fica igual: um leitor faz
1/e4 para ter quadros por segundo.
"""
import sys

MARK = '/* vitart: piso do dt */'
patches = [
    # depois de gravar o e4 (= e8 / 60, do mesmo s0): só a conta do dt perde o e8
    ('L_8110a274:; /* vstr s1, [r0] */\n    ARC_SW((r0 + 0x0u), ARC_GSB(ctx, 1));\n',
     'L_8110a274:; /* vstr s1, [r0] */\n    ARC_SW((r0 + 0x0u), ARC_GSB(ctx, 1));\n'
     '    if (ARC_LW(0x812b75d0u)) ARC_SS(ctx, 0, 0.0f); ' + MARK + '\n'),
    ('L_8110a2b6:; /* movt r0, #0x42c8 */\n    r0 = (r0 & 0xffffu) | 0x42c80000u;\n',
     'L_8110a2b6:; /* movt r0, #0x42c8 */\n    { uint32_t vt_floor = ARC_LW(0x812b75d0u); ' + MARK + '\n'
     '      r0 = vt_floor ? vt_floor : ((r0 & 0xffffu) | 0x42c80000u); }\n'),
    # Qual caminho o jogo tomou neste quadro, em 0x812b75d8: 0 = tempo real (jogatina), 1 = passo fixo de
    # 1/60 (menus, vídeos, telas de carga). No passo fixo o jogo anima por quadro: o runtime limita a 60
    # quadros/s nesse modo mesmo com o VSync desligado (hle_sys.cpp), senão o menu principal acelerava.
    ('L_8110a282:; /* vmov s1, r5 */\n    ARC_SSB(ctx, 1, r5);\n',
     'L_8110a282:; /* vmov s1, r5 */\n    ARC_SSB(ctx, 1, r5);\n'
     '    ARC_SW(0x812b75d8u, 0u); ' + MARK + '\n'),
    ('L_8110a302:; /* vmov.f32 s0, s1 */\n    ARC_SSB(ctx, 0, ARC_GSB(ctx, 1));\n',
     'L_8110a302:; /* vmov.f32 s0, s1 */\n    ARC_SSB(ctx, 0, ARC_GSB(ctx, 1));\n'
     '    ARC_SW(0x812b75d8u, 1u); ' + MARK + '\n'),
]


def main():
    p = sys.argv[1].rstrip('/\\') + '/arc_funcs_011.c'
    s = open(p, encoding='utf-8').read()
    if MARK in s:
        print('já remendado')
        return
    for a, b in patches:
        assert s.count(a) == 1, 'trecho não encontrado: ' + a.split('\n')[0]
        s = s.replace(a, b)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('remendado:', p)


if __name__ == '__main__':
    main()
