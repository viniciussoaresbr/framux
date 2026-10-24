// Integração com o Steam Flatpak (com.valvesoftware.Steam).
//
// O sandbox não enxerga /usr do sistema nem $XDG_RUNTIME_DIR/framux. A
// integração copia a camada e o shim (64 e 32 bits) para a área do app em
// ~/.var/app, escreve ali os manifestos Vulkan e um `framux-run` para jogos
// OpenGL e acrescenta ao override do usuário o acesso ao socket e à config.
#pragma once

#include <string>

namespace framux::flatpak {

inline constexpr const char *kSteamId = "com.valvesoftware.Steam";

// ~/.var/app/com.valvesoftware.Steam
std::string steam_home();
// Cópias da camada/shim e o framux-run: <steam_home>/.local/share/framux.
std::string bundle_dir();
// Override do usuário: $XDG_DATA_HOME/flatpak/overrides/com.valvesoftware.Steam.
std::string overrides_path();

// Steam Flatpak instalado (no usuário ou no sistema).
bool steam_installed();

struct Status {
  bool steam = false;     // Steam Flatpak instalado
  bool enabled = false;   // integração feita
  bool layer64 = false;   // cópias de 64 bits presentes
  bool layer32 = false;   // cópias de 32 bits presentes
  bool outdated = false;  // alguma cópia difere da instalada (só com `deep`)
  bool auto_env = false;  // FRAMUX=1 e FRAMUX_AUTO=1 no override
  bool source32 = false;  // framux-layer.i686 instalado no sistema
};
// `deep` compara as cópias byte a byte com as instaladas (para `outdated`).
Status status(bool deep = false);

// Copia, escreve manifestos e framux-run e aplica o override; com
// `auto_mode`, o override também liga FRAMUX=1 FRAMUX_AUTO=1.
bool enable(bool auto_mode, std::string *err);
// Desfaz tudo o que enable() criou.
bool disable(std::string *err);
// Atualiza as cópias se a integração estiver feita (depois de atualizar o
// pacote). Sem integração, não faz nada.
bool sync(std::string *err);
// Liga/desliga FRAMUX=1 FRAMUX_AUTO=1 no override (modo automático).
bool set_auto(bool on, std::string *err);

// Opção de inicialização para jogos OpenGL no Steam Flatpak.
std::string run_command();

}  // namespace framux::flatpak
