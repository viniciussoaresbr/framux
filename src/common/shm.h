// Memória compartilhada anônima no Windows (o equivalente ao memfd): uma
// seção sem nome, passada entre processos por DuplicateHandle (ipc_socket).
// Handles são guardados em `int`, como os fds no Linux: o Windows garante
// que handles cabem em 32 bits (compatibilidade com processos de 32 bits).
#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstdint>

namespace framux::shm {

// Seção de `size` bytes, leitura e escrita. -1 em erro.
int create(size_t size);
// Mapeia a seção inteira. *size recebe o tamanho mapeado (arredondado à
// página). nullptr em erro.
void *map(int handle, size_t *size, bool writable = true);
void unmap(void *p);
void close(int handle);

// Cria a seção, copia `data` e devolve o handle (-1 em erro).
int from_bytes(const void *data, size_t size);

}  // namespace framux::shm

#endif
