// Nomes de arquivos (§C.7.1) e criação segura (§C.11.5).
#pragma once

#include <ctime>
#include <string>

namespace framux {

// "AAAA-MM-DD HH-MM-SS" em hora local; com centis ≥ 0 acrescenta "-cc".
std::string file_timestamp(std::time_t t, int centis = -1);

// Cria `dir/stem<suffix>` com O_EXCL|O_NOFOLLOW; se já existir, tenta
// `dir/stem (2)<suffix>`, `(3)`… Devolve o fd (ou -1) e o caminho final.
int create_unique(const std::string &dir, const std::string &stem, const std::string &suffix, std::string *path,
                  std::string *err);

// Escreve tudo em fd e fecha. Devolve false em erro.
bool write_all_close(int fd, const std::string &data, std::string *err);

// Acrescenta a um arquivo de log (O_APPEND, sem seguir symlink).
bool append_file(const std::string &path, const std::string &data, std::string *err);

}  // namespace framux
