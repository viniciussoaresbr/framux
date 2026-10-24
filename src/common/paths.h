#pragma once

#include <string>
#include <string_view>

namespace framux {

std::string home_dir();
// $XDG_CONFIG_HOME/framux (ou ~/.config/framux).
std::string config_dir();
// $XDG_RUNTIME_DIR/framux. Vazio se XDG_RUNTIME_DIR não existir.
std::string runtime_dir();
std::string socket_path();
std::string stats_path();

// Diretório de usuário XDG (PICTURES, VIDEOS…), lido de user-dirs.dirs.
std::string xdg_user_dir(std::string_view name);

// Expande "~", "$HOME", "$XDG_PICTURES_DIR", "$XDG_VIDEOS_DIR" e outras
// variáveis de ambiente "$NOME" / "${NOME}".
std::string expand_path(std::string_view in);

// Troca /\:*?"<>| e caracteres de controle por '_' e remove "." / ".."
// de nomes (usado para <jogo> em nomes de arquivo, §C.7.1).
std::string sanitize_filename(std::string_view in);

// Cria o diretório e os pais (0755). Devolve false e preenche err em falha.
bool make_dirs(const std::string &path, std::string *err = nullptr);

}  // namespace framux
