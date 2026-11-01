// Integração do Framux com a sessão do Windows (o que no Linux são o
// manifesto em /usr/share/vulkan, o environment.d e o autostart).
#pragma once

#ifdef _WIN32

#include <string>

namespace framux::win {

// Manifesto da camada Vulkan ao lado dos executáveis.
std::string layer_manifest_path();
// Registrado em HKCU\Software\Khronos\Vulkan\ImplicitLayers (ou HKLM, pelo
// instalador)?
bool layer_registered();
bool set_layer_registered(bool on, std::string *err);

// Modo automático: FRAMUX=1 e FRAMUX_AUTO=1 nas variáveis de ambiente do
// usuário (HKCU\Environment). Programas abertos depois herdam; o Steam
// precisa ser reiniciado.
bool auto_mode_enabled();
bool set_auto_mode(bool on, std::string *err);
// Ligado no registro mas ainda não no ambiente deste processo.
bool auto_mode_needs_restart();

// Abre a GUI ao entrar no Windows (HKCU\...\Run).
bool open_on_login_enabled();
bool set_open_on_login(bool on, std::string *err);

// O usuário está em "Usuários do log de desempenho" (S-1-5-32-559) ou é
// administrador? Necessário para ler os eventos de apresentação (ETW).
bool can_trace_presents();

// Executável ao lado deste (ex.: "framuxd.exe").
std::string sibling(const char *exe);
// Inicia o framuxd sem console, se não estiver rodando. Não bloqueia.
// De um processo elevado, inicia sem elevação (ver is_elevated).
bool spawn_daemon();

// Este processo roda como administrador (token elevado)? Um framuxd elevado
// não recebe os buffers compartilhados da camada Vulkan (o Windows impede um
// jogo comum de passar handles a um processo de nível mais alto), então
// gravação e screenshot de jogos Vulkan falham.
bool is_elevated();

}  // namespace framux::win

#endif
