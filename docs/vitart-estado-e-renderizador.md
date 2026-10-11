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

## Tradutor USSE → SPIR-V: estado em 10/10/2026

Código: `vitarecomp/usse/`.
- `decode.py`: decodificador.
- `operands.py`: bancos, swizzles, máscaras e predicados.
- `disasm.py`: rebaixamento para operandos.
- `interp.py`: interpretador de referência.
- `translate.py`: emissor SPIR-V.
- `spirv.py`: construtor binário próprio.

Medição: `tools/usse_translate_all.py <captura>` traduz todos os programas e passa cada um no `spirv-val`
(vulkan1.1).

| etapa | programas traduzidos e válidos |
|---|---|
| F32 básico (vmad, vdp, v32nmad, vcomp) | 40 |
| + registradores F16 empacotados | 376 |
| + vmad2, vmov | 869 |
| + predicados (vtst), kill | 2.299 |
| + vpck f32/f16 | 2.306 |
| + vbw | 2.437 |
| + vldst (storage buffer), i32mad, vpck f32→u16, spec/smlsi | 3.111 |
| + repetição SLMSI (vmov, vpck, vbw) | 3.824 |
| + vdual | 4.785 |
| + vdp repetido (EXTERNAL) | **4.897 de 4.922 (99,5%)**, 0 inválidos |

Fatos medidos no caminho:
- Os ossos do skinning são matrizes 4x4 em vetor-linha (16 palavras por `vldst`), misturadas como
  A + w·(B − A).
- A projeção se comporta como coluna: 82.637 de 170.719 vértices caem na tela, contra 12.078 na melhor
  alternativa.
- Com isso, `tools/gxm_softraster.py` desenha o primeiro quadro (céu, mar e logo do título). As cores ainda
  são aproximadas.

**O que "válido" ainda NÃO garante:**
- A ligação de entradas e saídas é provisória:
  - PA recebe os atributos pelo índice de recurso;
  - SA recebe o buffer inteiro, e não pelos contêineres;
  - a saída é OUTPUT 0..3;
  - as leituras de textura não dependentes (que o hardware põe nos PA dos fragment shaders) não foram
    modeladas.
- `vlog`/`vexp` estão em base 2, o que ainda precisa ser validado.
- A semântica ainda não foi comparada com execução real.

Falta, por programa (primeira instrução não coberta): `smp` 15 (texturas dependentes) e `br` 10. As instruções repetidas sem modelo são recusadas de propósito: traduzi-las daria SPIR-V válido e
errado.

**Primeiro teste de fidelidade** (`tools/usse_interp_draw.py dump900 3 150`):
- o shader de skinning real roda inteiro no interpretador, com as entradas montadas pelas tabelas do GXP,
  e **sem nenhum erro de instrução**;
- mas o resultado ainda está errado: x (0..466) e w (≈1066) são plausíveis, y (≈1530) sai da tela;
- os desenhos 3, 5 e 9 deram as mesmas amostras, o que indica saída lida no lugar errado (a posição pode não
  estar em OUTPUT 0..3; ver as varyings do GXP) ou atributos ligados errado.

Diagnóstico até aqui (desenho 3, programa 0x860f27b0):
- as amostras iguais são legítimas: os desenhos 3 e 5 têm o mesmo primeiro vértice nos dados capturados;
- a saída de um vértice é OUTPUT = (−784, −6943, −375, 1066): x/w e w são plausíveis, mas y/w ≈ −6,5;
- os contêineres são (14: base 0, 56 palavras) e (19: base 56, 10 palavras), há um uniform buffer com
  `ldst_base_value = 220` bytes e 9 literais.

- O endereço do osso é `PA7.baixo × SA65 + SA56`, com SA65 = literal 0x40 = 64 bytes (osso 4x4, o que confirma o
  modelo) e SA56 = `ldst_base_value` = 220.
- `g_Transforms` neste programa começa na palavra 56 (224 bytes). O osso k é lido em 220 + 64k, uma palavra
  antes do esperado, qualquer que seja k.

**Pista principal:** o erro está na base do buffer ou no significado de `ldst_base_value`, não na semântica das
instruções. Hipóteses a testar:
- o buffer real começa 4 bytes antes do que o jogo recebe de `sceGxmReserveVertexDefaultUniformBuffer`;
- `ldst_base_value` é relativo a outra base.

O teste é ler com +4 e ver se as posições caem na tela.

**Resultado (medido, revisto em 11/10/2026):**
- o "−4 resolve" da primeira medição era **falso positivo**: o critério "cai na tela" aceitava vértices
  errados. A comparação vértice a vértice (interpretador contra o modelo numpy) mostra que **+4** é o
  deslocamento certo: x, y e w batem exatamente;
- mesmo assim y continuava fora da tela. A causa **não** estava no shader: os ossos que o jogo grava têm
  a linha de translação no padrão (a, a, b, −b) em vez de (tx, ty, tz, 1).

### Diferencial de função contra o Unicorn (como a causa foi achada)

Ferramentas novas:
- `VITART_WATCH=endereço,tamanho` (`VITART_WATCH_MAX=n`): watchpoint de escrita. Registra a função do
  guest, r0–r3/sp/lr, o valor anterior e o valor novo.
- `tools/snap_wrap.py <gen> <endereço> <n>`: põe uma casca na N-ésima chamada de uma função gerada; o
  runtime grava o estado na entrada e na saída (`VITART_SNAP_DIR`). `--remove` tira as cascas.
- `tools/func_diff.py <snap> <endereço>`: roda o ARM original no Unicorn a partir da entrada e compara
  registradores e cada byte escrito com o que a tradução deixou. `--reads` lista os blocos lidos.

A cadeia, medida elo a elo:
1. a paleta de ossos sai da 0x810d761a: o diferencial deu 0 bytes diferentes (2.556 bytes alterados). A
   função está certa; a entrada já vinha ruim;
2. a raiz da paleta = mundo do objeto × matriz de câmera; a de câmera vem da inversa afim calculada pela
   0x8110f2b4. A rotação é gravada direto dos registradores s, mas a translação passa por uma cadeia
   NEON de `vdup`/`vbif`/`vsli.64`;
3. o diferencial da 0x8110f2b4 acusou `vmov.i64 d19, #0xffffffff`: o ARM dá `0x00000000ffffffff`, a
   tradução dava `0xffffffffffffffff`. O Capstone devolve esse imediato como −1 (32 bits com sinal
   estendido). A máscara toda em 1 faz o `vbif` copiar a lane errada, o que produz exatamente
   (a, a, b, −b).

Correção: `neon.py::_expand_i64` lê imm8 = i:imm3:imm4 da codificação e expande cada bit num byte. O
jogo tem 55 dessas instruções e todas estavam erradas. O fuzz de NEON não gerava imediatos vetoriais;
o caso foi acrescentado em `tests/fuzz_arm/gen_fuzz_neon.py`.

Resultado (captura nova do quadro 900, `dump900b`): 0 de 219 desenhos com o padrão ruim, e o
rasterizador de prova passa a desenhar o Kratos, que antes sumia
(`docs/quadro900-antes-depois-vmov-i64.png`). O bug afetava as 55 instruções do jogo, não só o
skinning: lógica, física ou câmera que usem essa máscara também estavam erradas.

O próximo marco não é cobertura, é **fidelidade**:
1. ligar a E/S de verdade (contêineres de SA, endereços de buffer, literais, varyings e as leituras de
   textura não dependentes nos PA);
2. rodar o `interp.py` sobre os desenhos capturados e conferir as posições na tela;
3. rodar o SPIR-V numa GPU e comparar com o interpretador.

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

## Renderizador Vulkan (11/10/2026)

- `runtime/vitart/gxm_vk.cpp`: renderizador próprio. Um command buffer por quadro, anel de upload de
  256 MB, alvos por endereço da superfície de cor, profundidade como textura, cópia de alvo para
  render-to-texture, janela Win32 + swapchain.
- Ao vivo: `VITART_VULKAN=1` (shaders do cache `VITART_SPV_DIR`, gerado por `tools/usse_build_cache.py`);
  `VITART_VK_DUMP=N` grava o quadro N apresentado em PPM. Tela de título a 7,7 quadros/s.
- Replay: `tools/gxm_pack_frame.py` -> `gxm_replay.exe` (`--limit N` para bisseção, `--passes`).
- Depuração: `VITART_VK_NODEPTH=1`, `VITART_VK_NOBLEND=1`.
- Início de cena (`sceGxmBeginScene`) devolve a profundidade do alvo ao fundo, salvo "force load".

Medido no desempenho (média de 120 quadros): `vk_draw` 117 ms por quadro, dos quais só 35 ms no
`gvk::draw`; os ~80 ms restantes são conversão de atributos e índices no HLE. Apresentar: 61 ms (espera
da GPU). Próximos ganhos: converter atributos sem `attr_value` por componente, e não esperar a GPU na
apresentação (dois quadros em voo).

### Defeito aberto: buracos no rosto do Kratos (tela de título)

A malha principal do rosto (desenho 69, 19.734 índices, programa de skinning 0x861544d0) sai com
buracos grandes, e as passadas seguintes preenchem os buracos em manchas. Descartado, por medição:
- textura (decodifica igual à referência; alfa 255 em todo texel);
- teste de profundidade (buracos continuam com `VITART_VK_NODEPTH`), profundidade velha (uma passada
  só) e LESS coplanar;
- blend (continuam com `VITART_VK_NOBLEND`); o fragmento não tem `kill`;
- osso fora do buffer (índice máximo 11 -> 992 de 1.248 bytes).
Conclusão anterior CORRIGIDA por medição: com `USSE_DEBUG_VSOUT=1` (o vértice traduzido grava OUTPUT
0..3 após os uniforms) e `gxm_replay --debug-vs 69`, a saída da GPU bate com o `interp.py` em 600 de
600 vértices usados pelo desenho. Os "buracos" do desenho 69 isolado são normais: o rosto é dividido em
vários desenhos (67, 69, 71, 73...) que se completam, e os vértices 0..3688 do buffer pertencem a outros
desenhos. As manchas vêm de iluminação diferente entre partes (cor por vértice em texcoord1, calculada
no shader). Se houver erro, está na semântica de alguma instrução desse caminho (interp e SPIR-V
compartilham o rebaixamento), ou em uniforms de luz por desenho. Próximo passo: rodar só a parte de
iluminação (o6/o8) no interp para duas partes vizinhas e conferir cada instrução (vdual, vpck, f16).

Pista medida (11/10, 02:30): o desenho 71 sai muito mais claro (RGB médio 241/128) que 69/73/75 (≈75/22)
com os mesmos shaders. Os uniforms dos três são iguais, exceto as palavras 22/23 e 30/31 do default
uniform buffer (palavra 30: 0,155 / 1,97 / 0,083). `VITART_TRACE_UNIFORMS` mostra que `g_LightDir` é
enviado como dois vec3 (offsets 0 e 3), e nenhum parâmetro cobre as palavras 22/23 e 30/31: são
preenchimento com sobras do anel. Hipótese a testar: alguma instrução do caminho de iluminação (o6/o8)
lê o 4º componente onde o hardware usa só 3 (swizzle/máscara de vec3), e o brilho passa a depender de
lixo. Teste: zerar essas palavras no pacote e ver se as manchas somem; depois achar a instrução.

**Resolvido (11/10, 02:33):** a causa era o HLE de `sceGxmSetUniformDataF`. Em arrays de vetores float
(array > 1, componentes > 1), cada elemento ocupa componentes × 4 bytes alinhados a 8 (float3 = 16
bytes), e o deslocamento conta componentes dentro desses elementos. Copiar contíguo punha o 2º vec3 de
`g_LightDir` uma palavra antes; o shader lia a palavra 30 (sobra do anel) como z da luz. Prova:
zerar as palavras 22/23/30/31 no pacote (`GXM_ZERO_VU`) já removia as manchas; com a correção, o rosto
sai liso ao vivo (`docs/ao-vivo-vulkan-titulo-corrigido.png`).
