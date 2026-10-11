# Áudio do PS2 no lugar do áudio do Vita (God of War 1)

## O que foi medido

- ISO do PS2 (SCUS-97399): `SCUS_973.99`, `GODOFWAR.TOC`, `PART1.PAK` (4,26 GB). TOC = entradas de
  24 bytes (nome[16], tamanho, setor relativo ao PAK). `tools/ps2_audio.py` lê direto da ISO.
- Os **1102 `.at9`** do Vita (`wad/_sound/dvd_streams/PSP2/**.at9` no psarc: música, falas, ambientes)
  têm **todos** um par com o mesmo nome no PS2 (`.VPK` estéreo 44,1 kHz ou `.VAG` mono 22.050 Hz,
  PS-ADPCM). As subpastas de idioma (French/German/...) do Vita apontam para o mesmo nome em inglês.
- Controle do decodificador: `RAGEMIME` 3,50 s nos dois; `TIT1E1_2` 58,54 s (PS2) × 58,68 s (Vita).
  VPK: o tamanho no cabeçalho é por canal; intercalação 0x1000 por canal (sem salto nas fronteiras).
- Os efeitos sonoros dentro dos `.wad` (bancos do Scream) não entram nesta primeira etapa.

## Desenho da troca (sem codificador de ATRAC9)

Não existe codificador livre de ATRAC9, então o arquivo do Vita fica intacto e a troca acontece no
**nosso** decodificador (hle_ngs.cpp), por superquadro:

1. Ferramenta offline monta um índice: para cada `.at9`, o FNV-1a de 64 bits de cada superquadro
   (tamanho = nBlockAlign do fmt; amostras por superquadro = SamplesPerBlock da extensão do fmt) →
   (arquivo, índice do superquadro). Junto, para cada arquivo: amostras totais do Vita (fact), atraso
   do codificador, e onde está o par no PS2 (setor e tamanho dentro da ISO, formato, taxa, canais).
   O índice fica pequeno (~1 M entradas); o áudio do PS2 é lido **direto da ISO**, sem cópia.
2. Em tempo de execução, cada superquadro ATRAC9 que a voz vai decodificar é procurado pelo hash.
   Achou: em vez do ATRAC9, sai o trecho correspondente do PS2. A posição do Vita i (amostras a 48 kHz)
   vira PS2 = i × duração_PS2 / duração_Vita, reamostrado (cúbico) para 48 kHz. Os bytes consumidos e
   as amostras geradas continuam idênticos aos do ATRAC9, então o jogo (laços, posição de stream,
   callbacks) não percebe nada. Não achou: decodifica o ATRAC9 normalmente.
3. Silêncio repetido entre arquivos: preferir o mesmo arquivo no índice seguinte ao anterior.
4. O PS2 de cada arquivo em uso é decodificado inteiro para PCM (cache LRU de poucos arquivos).

Liga e desliga por opção (`VITART_PS2_AUDIO=<iso>`), para comparar os dois.

## Efeitos sonoros (medido em 11/10)

- Os 16 VAG do `r_perma.wad_psp2` são **idênticos byte a byte** aos do `R_PERM.WAD` do PS2, e 41 de 60
  trechos de 2 KB do banco Scream principal (SBlk v9 no Vita, v3 no PS2) também. Os efeitos já tocam
  as amostras do PS2; não há o que trocar neles.
- Nosso HE-ADPCM decodifica esses quadros como PS-ADPCM exato (coeficientes do PS2 ×128): 0 quadros
  com coeficiente desconhecido em 351 mil.
- Mix (VITART_NGS_LOG): saturação suave em 0,004–0,05% das amostras; o limitador corta >1 dB em 1–6%
  só nos picos de combate. O mix não distorce.
