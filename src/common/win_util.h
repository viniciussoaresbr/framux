// Utilitários do Windows: conversão UTF-8 ↔ UTF-16 (o código trabalha em
// UTF-8; a API do Windows, em UTF-16) e nomes de objetos do usuário.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <string_view>

namespace framux::win {

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);

// Variável de ambiente em UTF-8 (vazio se não existir).
std::string env(const char *name);

// Mensagem de GetLastError() (ou do código dado) em UTF-8.
std::string error_text(uint32_t code = 0);

// Sufixo único por usuário e sessão de logon ("<sessão>-<usuário>"), usado
// nos nomes dos pipes e da memória compartilhada.
std::string user_tag();

// Caminho do executável/DLL que contém `addr` (nullptr = o executável).
std::string module_path(const void *addr = nullptr);
// Pasta de module_path(), sem a barra final.
std::string module_dir(const void *addr = nullptr);

}  // namespace framux::win

#endif
