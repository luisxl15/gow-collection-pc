# vitart: estado do runtime e plano do renderizador

O runtime próprio (`runtime/vitart`) roda o God of War 1 do PS Vita nativamente no Windows, sem emulador.

## O que funciona (medido em 10/10/2026)

- O jogo chega ao laço principal e fica estável a **30,0 FPS** por mais de 120 s, com cerca de 150 desenhos e
  4 cenas por quadro.
- Inicializa o SCREAM 7.4.17 (áudio do jogo), o NGS (9 racks, 139 vozes) e a porta de áudio de 48 kHz, e lê os
  streams `.at9` de dentro do `.psarc` sem erros.
- Framebuffer de 720x408 (`sceDisplaySetFrameBuf`), viewport com 360/360/204/−204 e ABI hard-float confirmada:
  os floats chegam em s0..s5.
- A captura de quadro (`VITART_DUMP_FRAME=N VITART_DUMP_DIR=dir`) grava `textures.txt`, `draws.jsonl`,
  `draws.bin` e a memória do jogo (`mem_*.bin`).
  - `tools/gxm_tex_dump.py`: texturas → PNG (conferido: logo, fonte, Kratos e céu corretos).
  - `tools/gxp_info.py`: lista os programas GXP e seus parâmetros.
  - `tools/usse_coverage.py`: decodifica os 4.922 programas (139.308 instruções); **0 instruções sem classe**.

## O que ainda não existe

- **Desenho.** O `hle_gxm.cpp` grava o estado e conclui as notificações na hora, mas não rasteriza.
- **Síntese de som.** O NGS cumpre o contrato da API, mas não gera amostras.
- Gravação de saves (`sceAppUtilSaveDataDataSave` só registra a chamada).

## Por que um tradutor USSE (e não shaders genéricos)

`tools/gxm_softraster.py` tentou shaders genéricos escolhidos pela assinatura de parâmetros. Funciona nos
retângulos de tela, mas falha nos modelos com skinning, que saem com coordenadas na casa dos milhões. A
desmontagem mostra o motivo: o shader de vértice calcula o endereço do osso com
`i32mad` (índice × passo + base) e lê a matriz 3x4 da memória com `vldst`, usando dois índices de 16 bits
empacotados. Isso só se reproduz executando o código USSE.

Medidas que orientam o tradutor:
- 2.461 programas de vértice e 2.461 de fragmento, quase sem desvios (20 `br` no total).
- Mistura de instruções: vmad 45.673, vpck 23.850, v32nmad 14.916, v16nmad 10.074, phas 6.481, spec 5.546,
  vmov 5.177, vdp 4.708, vmad2 3.678, vldst 3.500, nop 3.315, i32mad 2.328, vbw 2.269, vdual 2.015, vtst 1.580,
  kill 1.560, smlsi 1.436, vcomp 985, smp 196, br 20, vtstmsk 1.
- Cada programa tem duas fases: a **secundária** roda uma vez por desenho e prepara os registradores SA a
  partir dos uniforms; a **primária** roda por vértice ou pixel.

## Próximos passos (em ordem)

1. **Interpretador USSE em Python** (`vitarecomp/usse/interp.py`), como referência semântica:
   - bancos de registradores: TEMP, PA, SA, OUTPUT, FPINTERNAL, SPECIAL→GLOBAL/FPCONSTANT, IMMEDIATE, INDEX;
   - registradores duplos (número << 1) nas instruções F32 vetoriais;
   - TEMP alto → FPINTERNAL;
   - swizzles vec3/vec4/escalar, padrão e estendidos;
   - carga dos uniforms nos SA pelos contêineres do GXP (`container_offset`, `buffer_flags`).

   **Teste que pode falhar:** executar o shader de vértice real sobre os vértices capturados (`draws.bin`)
   tem de dar posições dentro do viewport de 720x408 para os modelos.
2. **Emissor SPIR-V** com a mesma semântica, com cache por hash de programa e permutação. Validar
   comparando com o interpretador, vértice a vértice.
3. **Backend Vulkan no `hle_gxm.cpp`**: superfícies por endereço (alvo de render = textura quando os
   endereços coincidem), blend do `SceGxmBlendInfo` (4 bytes), profundidade, viewport e apresentação do
   framebuffer de exibição.

Referência de ISA: as codificações de bits (`vitarecomp/usse/encodings.json`) foram extraídas da tabela do
decodificador do Vita3K por `tools/usse_extract_encodings.py`. A semântica de cada instrução é implementada
por nós; o Vita3K não entra no produto.
