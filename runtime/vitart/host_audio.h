// Saída de som do vitart no PC: um fluxo WASAPI compartilhado (48 kHz estéreo), um mixador que soma as
// portas do sceAudioOut e uma fila por porta. A cadência do jogo vem da placa: sceAudioOutOutput espera
// enquanto a fila da porta estiver cheia, com ~30 ms de folga (o waveOut por porta somava 100-150 ms e
// dessincronizava a imagem do som nos vídeos).
#pragma once
#include <cstdint>

namespace hostaudio {

constexpr uint32_t kRate = 48000;

// Liga o fluxo (uma vez); false sem placa (VITART_MUTE ou falha do WASAPI): o chamador cadencia pelo relógio.
bool start();
int open_port(uint32_t freq);
void close_port(int port);
// Amostras de 16 bits (estéreo intercalado ou mono) com volume 0..32768 por canal; reamostra para 48 kHz.
void write(int port, const int16_t *src, uint32_t frames, bool stereo, int vol_l, int vol_r);
// Quadros (a 48 kHz) na fila da porta, ainda não entregues à placa.
uint32_t queued(int port);
// Para tudo (troca de módulo).
void silence();
struct Stats {
    uint64_t mixed = 0, underruns = 0; // quadros entregues; vezes em que uma porta tocando ficou sem dados
    uint32_t latency_ms = 0;           // buffer do WASAPI
};
Stats stats();

} // namespace hostaudio
