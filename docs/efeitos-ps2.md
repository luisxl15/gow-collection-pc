# Efeitos do PS2 desligados no código: investigação

Pergunta: o porte do Vita desliga por código algum efeito que existia no PS2? Se sim, ligar.

Resposta curta: **não foi achado nenhum efeito desligado por código** no GoW1 nem no GoW2. Os efeitos que
faltavam na tela eram do **nosso** renderizador: 9 shaders do GoW2 que o tradutor USSE não sabia traduzir
(leitura de textura dependente `smp`, `vtstmsk`, `depthf`, `vpck` u16 repetido). Eles foram traduzidos no
commit 9ad9bfb: desfoque, borrão de movimento com o quadro anterior (`sLastFrameSampler`), sombra com
PCF (gather4), paleta, profundidade e pele.

## O que foi verificado

| Verificação | Ferramenta | Resultado |
|---|---|---|
| Comandos de script dos níveis (`SCR_*`) com função vazia (retorna direto) | `tools/re/scr_cmds.py` | GoW2: 137 comandos, nenhum vazio. GoW1: 77, nenhum vazio |
| Globais lidas e testadas que nenhum código grava (interruptor fixo) | `tools/re/dead_flags.py` | No código de render só aparecem as guardas de "shader já carregado" (estáticas do C++) |
| Shaders do motor (`EngineSupport/Shaders/*.fx`): quem registra | desmontagem | Todos (Blur, Lensflare, SkyboxBloom, FSWarp...) são registrados por construtores estáticos; `Blur.fx` e `Lensflare.fx` só não aparecem no log porque são carregados quando a fase usa |
| Efeitos de tela (`FSE_*`, `RFX_*`: Bloom, LayeredBloom, LayeredFog...) | desmontagem de 0x81128e80 (GoW2) | Registrados normalmente; a tabela 0x812df550 é a de efeitos ativos, não um interruptor |
| `CameraDOF` e `TimeWarpBlur` do GoW1 | desmontagem de 0x81034c3a | São grupos de ajuste do menu de depuração (Active, Focus Zone, Depth1/2, Alpha) presos a uma variável temporária; não há efeito de profundidade de campo por trás |
| Desenhos que o nosso renderizador pula | `hle_gxm.cpp` | Só linhas e pontos, que o censo nunca viu nos dois jogos |

## Como repetir

```bash
python tools/re/xref.py gow-dump/elf/gow2.elf          # índice movw/movt e bl (gera .xref.pkl)
python tools/re/scr_cmds.py gow-dump/elf/gow2.elf 0x81105c56           # GoW2: nome em r0, função em r1
python tools/re/scr_cmds.py gow-dump/elf/gow1.elf 0x81102aea r1 r2     # GoW1: nome em r1, função em r2
python tools/re/dead_flags.py gow-dump/elf/gow2.elf
```
