// Janela do jogo, comum aos renderizadores (Vulkan e Direct3D 11): criação (com a troca de módulo sem janela
// nova piscando), F11 / Alt+Enter, modo sem bordas pedido pelo menu, aviso no título, mouse como toque.
// As mensagens só chegam à thread que criou a janela: create() e pump() rodam na thread que apresenta.
#pragma once
#include <cstdint>
#include <string>

namespace gvk::win {

bool create(const wchar_t *title);
void *hwnd(); // HWND
// Mensagens pendentes, aviso no título e o pedido de sem bordas do menu (a cada apresentação).
void pump();
// O tamanho da área cliente mudou (WM_SIZE, tela cheia) desde a última chamada.
bool take_resized();
void mark_resized();
// Depois de cada apresentação: na troca de módulo a janela aparece no 2º quadro e avisa o processo anterior.
void presented();

bool closed();
bool rect(int &x, int &y, int &w, int &h);
bool pointer(float &nx, float &ny);
bool focused();
void notify(const std::string &text);
void request_borderless(bool on);
bool borderless();

} // namespace gvk::win
