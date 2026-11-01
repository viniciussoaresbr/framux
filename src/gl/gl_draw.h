// Overlay, screenshot e gravação no OpenGL, comuns a GLX, EGL e WGL.
#pragma once

#include <cstdint>

#include "common/ipc_proto.h"

namespace framux::gl {

// Chamado antes de cada troca de buffers do contexto atual. `ctx_key`
// identifica o contexto (nullptr = sem contexto utilizável: só conta o
// quadro), `drawable` a superfície/janela e `gpa` resolve funções do GL.
void on_swap(ipc::Api api, const void *ctx_key, uintptr_t drawable, uint32_t w, uint32_t h,
             void *(*gpa)(const char *));

}  // namespace framux::gl
