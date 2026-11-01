// libframux_gl.so — shim OpenGL carregado por LD_PRELOAD (§C.3.2).
//
// Intercepta glXSwapBuffers / glXSwapBuffersMscOML / eglSwapBuffers(+WithDamage),
// além de glXGetProcAddress(ARB), eglGetProcAddress e dlsym, para devolver os
// hooks a quem resolve os símbolos dinamicamente.
//
// O overlay é desenhado com glScissor + glClearColor + glClear por retângulo,
// salvando e restaurando só o estado que essas chamadas afetam. Nunca chama
// glGetError (o flag de erro pertence ao jogo) e só consulta enums suportados
// pela versão/extensões do contexto, para não gerar GL_INVALID_ENUM.

#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/runtime.h"
#include "common/log.h"
#include "gl/gl_draw.h"

#define FRAMUX_EXPORT extern "C" __attribute__((visibility("default")))

#ifndef EGL_KHR_swap_buffers_with_damage
typedef EGLBoolean (*PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)(EGLDisplay, EGLSurface, EGLint *, EGLint);
#endif

namespace framux::gl {

using client::Runtime;

namespace {

// ---------------------------------------------------------------------------
// Resolução dos símbolos reais

using DlsymFn = void *(*)(void *, const char *);

DlsymFn real_dlsym() {
  static DlsymFn fn = [] {
    void *p = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!p) p = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (!p) p = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
    return reinterpret_cast<DlsymFn>(p);
  }();
  return fn;
}

void *lib_symbol(const char *lib, const char *name) {
  if (void *p = real_dlsym()(RTLD_NEXT, name)) return p;
  void *h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
  if (!h) h = dlopen(lib, RTLD_LAZY);
  return h ? real_dlsym()(h, name) : nullptr;
}

template <class T>
T glx_fn(const char *name) {
  return reinterpret_cast<T>(lib_symbol("libGL.so.1", name));
}
template <class T>
T egl_fn(const char *name) {
  return reinterpret_cast<T>(lib_symbol("libEGL.so.1", name));
}

struct Real {
  decltype(&::glXSwapBuffers) glXSwapBuffers = nullptr;
  int64_t (*glXSwapBuffersMscOML)(Display *, GLXDrawable, int64_t, int64_t, int64_t) = nullptr;
  decltype(&::glXGetProcAddress) glXGetProcAddress = nullptr;
  decltype(&::glXGetProcAddressARB) glXGetProcAddressARB = nullptr;
  decltype(&::glXGetCurrentContext) glXGetCurrentContext = nullptr;
  decltype(&::glXGetCurrentDrawable) glXGetCurrentDrawable = nullptr;
  decltype(&::glXQueryDrawable) glXQueryDrawable = nullptr;
  decltype(&::eglSwapBuffers) eglSwapBuffers = nullptr;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC eglSwapBuffersWithDamageKHR = nullptr;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC eglSwapBuffersWithDamageEXT = nullptr;
  decltype(&::eglGetProcAddress) eglGetProcAddress = nullptr;
  decltype(&::eglGetCurrentContext) eglGetCurrentContext = nullptr;
  decltype(&::eglGetCurrentSurface) eglGetCurrentSurface = nullptr;
  decltype(&::eglQuerySurface) eglQuerySurface = nullptr;
  decltype(&::eglQueryAPI) eglQueryAPI = nullptr;
};

Real &real() {
  static Real r;
  return r;
}

void load_glx() {
  static std::once_flag once;
  std::call_once(once, [] {
    Real &r = real();
    r.glXSwapBuffers = glx_fn<decltype(r.glXSwapBuffers)>("glXSwapBuffers");
    r.glXGetProcAddress = glx_fn<decltype(r.glXGetProcAddress)>("glXGetProcAddress");
    r.glXGetProcAddressARB = glx_fn<decltype(r.glXGetProcAddressARB)>("glXGetProcAddressARB");
    r.glXGetCurrentContext = glx_fn<decltype(r.glXGetCurrentContext)>("glXGetCurrentContext");
    r.glXGetCurrentDrawable = glx_fn<decltype(r.glXGetCurrentDrawable)>("glXGetCurrentDrawable");
    r.glXQueryDrawable = glx_fn<decltype(r.glXQueryDrawable)>("glXQueryDrawable");
    if (r.glXGetProcAddressARB)
      r.glXSwapBuffersMscOML = reinterpret_cast<decltype(r.glXSwapBuffersMscOML)>(
          r.glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glXSwapBuffersMscOML")));
  });
}

void load_egl() {
  static std::once_flag once;
  std::call_once(once, [] {
    Real &r = real();
    r.eglSwapBuffers = egl_fn<decltype(r.eglSwapBuffers)>("eglSwapBuffers");
    r.eglGetProcAddress = egl_fn<decltype(r.eglGetProcAddress)>("eglGetProcAddress");
    r.eglGetCurrentContext = egl_fn<decltype(r.eglGetCurrentContext)>("eglGetCurrentContext");
    r.eglGetCurrentSurface = egl_fn<decltype(r.eglGetCurrentSurface)>("eglGetCurrentSurface");
    r.eglQuerySurface = egl_fn<decltype(r.eglQuerySurface)>("eglQuerySurface");
    r.eglQueryAPI = egl_fn<decltype(r.eglQueryAPI)>("eglQueryAPI");
    if (r.eglGetProcAddress) {
      r.eglSwapBuffersWithDamageKHR =
          reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(r.eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
      r.eglSwapBuffersWithDamageEXT =
          reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(r.eglGetProcAddress("eglSwapBuffersWithDamageEXT"));
    }
  });
}

void *glx_gpa(const char *name) {
  return reinterpret_cast<void *>(real().glXGetProcAddressARB(reinterpret_cast<const GLubyte *>(name)));
}
void *egl_gpa(const char *name) { return reinterpret_cast<void *>(real().eglGetProcAddress(name)); }

void glx_before_swap(Display *dpy, GLXDrawable drawable) {
  Real &r = real();
  const void *ctx = r.glXGetCurrentContext ? r.glXGetCurrentContext() : nullptr;
  // Overlay só se o drawable apresentado for o atual do contexto.
  if (ctx && r.glXGetCurrentDrawable && r.glXGetCurrentDrawable() != drawable) ctx = nullptr;
  unsigned int w = 0, h = 0;
  if (r.glXQueryDrawable) {
    r.glXQueryDrawable(dpy, drawable, GLX_WIDTH, &w);
    r.glXQueryDrawable(dpy, drawable, GLX_HEIGHT, &h);
  }
  on_swap(ipc::Api::GlGlx, ctx, drawable, w, h, glx_gpa);
}

void egl_before_swap(EGLDisplay dpy, EGLSurface surface) {
  Real &r = real();
  const void *ctx = r.eglGetCurrentContext ? r.eglGetCurrentContext() : nullptr;
  if (ctx && r.eglGetCurrentSurface && r.eglGetCurrentSurface(EGL_DRAW) != surface) ctx = nullptr;
  // Só contextos OpenGL/OpenGL ES (não OpenVG).
  if (ctx && r.eglQueryAPI && r.eglQueryAPI() != EGL_OPENGL_API && r.eglQueryAPI() != EGL_OPENGL_ES_API) ctx = nullptr;
  EGLint w = 0, h = 0;
  if (r.eglQuerySurface) {
    r.eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
    r.eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
  }
  on_swap(ipc::Api::GlEgl, ctx, reinterpret_cast<uintptr_t>(surface), uint32_t(w), uint32_t(h), egl_gpa);
}

}  // namespace

void *hook_for(const char *name);

}  // namespace framux::gl

using namespace framux::gl;

// ---------------------------------------------------------------------------
// Símbolos exportados

FRAMUX_EXPORT void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
  load_glx();
  if (!real().glXSwapBuffers) return;
  glx_before_swap(dpy, drawable);
  real().glXSwapBuffers(dpy, drawable);
}

FRAMUX_EXPORT int64_t glXSwapBuffersMscOML(Display *dpy, GLXDrawable drawable, int64_t target_msc, int64_t divisor,
                                           int64_t remainder) {
  load_glx();
  if (!real().glXSwapBuffersMscOML) return 0;
  glx_before_swap(dpy, drawable);
  return real().glXSwapBuffersMscOML(dpy, drawable, target_msc, divisor, remainder);
}

FRAMUX_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  load_egl();
  if (!real().eglSwapBuffers) return EGL_FALSE;
  egl_before_swap(dpy, surface);
  return real().eglSwapBuffers(dpy, surface);
}

FRAMUX_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n) {
  load_egl();
  if (!real().eglSwapBuffersWithDamageKHR) return EGL_FALSE;
  egl_before_swap(dpy, surface);
  return real().eglSwapBuffersWithDamageKHR(dpy, surface, rects, n);
}

FRAMUX_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n) {
  load_egl();
  if (!real().eglSwapBuffersWithDamageEXT) return EGL_FALSE;
  egl_before_swap(dpy, surface);
  return real().eglSwapBuffersWithDamageEXT(dpy, surface, rects, n);
}

FRAMUX_EXPORT __GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name) {
  load_glx();
  if (void *h = hook_for(reinterpret_cast<const char *>(name))) {
    // Só devolve o hook se a função existir de verdade.
    if (real().glXGetProcAddressARB && real().glXGetProcAddressARB(name)) return reinterpret_cast<__GLXextFuncPtr>(h);
  }
  return real().glXGetProcAddressARB ? real().glXGetProcAddressARB(name) : nullptr;
}

FRAMUX_EXPORT __GLXextFuncPtr glXGetProcAddress(const GLubyte *name) { return glXGetProcAddressARB(name); }

FRAMUX_EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
  load_egl();
  auto next = real().eglGetProcAddress ? real().eglGetProcAddress(name) : nullptr;
  if (next)
    if (void *h = hook_for(name)) return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(h);
  return next;
}

namespace framux::gl {

void *hook_for(const char *name) {
  if (!name || (name[0] != 'g' && name[0] != 'e')) return nullptr;
  struct {
    const char *name;
    void *fn;
  } static const hooks[] = {
      {"glXSwapBuffers", reinterpret_cast<void *>(&::glXSwapBuffers)},
      {"glXSwapBuffersMscOML", reinterpret_cast<void *>(&::glXSwapBuffersMscOML)},
      {"glXGetProcAddress", reinterpret_cast<void *>(&::glXGetProcAddress)},
      {"glXGetProcAddressARB", reinterpret_cast<void *>(&::glXGetProcAddressARB)},
      {"eglSwapBuffers", reinterpret_cast<void *>(&::eglSwapBuffers)},
      {"eglSwapBuffersWithDamageKHR", reinterpret_cast<void *>(&::eglSwapBuffersWithDamageKHR)},
      {"eglSwapBuffersWithDamageEXT", reinterpret_cast<void *>(&::eglSwapBuffersWithDamageEXT)},
      {"eglGetProcAddress", reinterpret_cast<void *>(&::eglGetProcAddress)},
  };
  for (const auto &h : hooks)
    if (!std::strcmp(name, h.name)) return h.fn;
  return nullptr;
}

namespace {

bool dlsym_hook_enabled() {
  static const bool on = [] {
    const char *v = std::getenv("FRAMUX_DLSYM");
    return !(v && !std::strcmp(v, "0"));
  }();
  return on;
}

// dlsym(RTLD_NEXT, …) é relativo a quem chama. Como este dlsym intercepta a
// chamada, o "quem chama" viraria o shim; emulamos a busca a partir do
// objeto do chamador para não quebrar outras bibliotecas pré-carregadas
// (ex.: o overlay da Steam).
//
// Sem dlopen nem dlsym por handle: o overlay da Steam intercepta dlopen e
// chama dlsym(RTLD_NEXT, "dlopen") lá dentro, o que voltaria para cá em
// recursão infinita; e o link_map de uma biblioteca carregada na partida não
// serve de handle até alguém dar dlopen nela. A busca vai direto na tabela
// de símbolos dinâmicos de cada objeto.

// Endereço de uma entrada do .dynamic: o ld.so às vezes já somou a base.
template <class T>
const T *dyn_ptr(const link_map *lm, ElfW(Addr) p) {
  return reinterpret_cast<const T *>(p < lm->l_addr ? p + lm->l_addr : p);
}

// Definição exportada (versão padrão, como o dlsym) de `name` em `lm`.
void *elf_lookup(const link_map *lm, const char *name) {
  const ElfW(Sym) *symtab = nullptr;
  const char *strtab = nullptr;
  const ElfW(Half) *versym = nullptr;
  const uint32_t *gnu_hash = nullptr, *sysv_hash = nullptr;
  for (const ElfW(Dyn) *d = lm->l_ld; d && d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB: symtab = dyn_ptr<ElfW(Sym)>(lm, d->d_un.d_ptr); break;
      case DT_STRTAB: strtab = dyn_ptr<char>(lm, d->d_un.d_ptr); break;
      case DT_VERSYM: versym = dyn_ptr<ElfW(Half)>(lm, d->d_un.d_ptr); break;
      case DT_GNU_HASH: gnu_hash = dyn_ptr<uint32_t>(lm, d->d_un.d_ptr); break;
      case DT_HASH: sysv_hash = dyn_ptr<uint32_t>(lm, d->d_un.d_ptr); break;
    }
  }
  if (!symtab || !strtab) return nullptr;
  auto match = [&](uint32_t i) -> void * {
    const ElfW(Sym) &s = symtab[i];
    if (s.st_shndx == SHN_UNDEF || std::strcmp(strtab + s.st_name, name)) return nullptr;
    const int bind = ELF64_ST_BIND(s.st_info), type = ELF64_ST_TYPE(s.st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK) return nullptr;
    if (ELF64_ST_VISIBILITY(s.st_other) != STV_DEFAULT) return nullptr;
    if (versym && (versym[i] & 0x8000)) return nullptr;  // versão oculta (não padrão)
    void *addr = reinterpret_cast<void *>(lm->l_addr + s.st_value);
    if (type == STT_GNU_IFUNC) return reinterpret_cast<void *(*)()>(addr)();
    return type == STT_FUNC || type == STT_OBJECT ? addr : nullptr;
  };
  if (gnu_hash) {
    const uint32_t nbuckets = gnu_hash[0], symoffset = gnu_hash[1], bloom_size = gnu_hash[2];
    if (!nbuckets) return nullptr;
    const uint32_t *buckets =
        gnu_hash + 4 + bloom_size * (sizeof(ElfW(Addr)) / sizeof(uint32_t));  // pula o filtro bloom
    const uint32_t *chain = buckets + nbuckets;
    uint32_t h = 5381;
    for (const char *c = name; *c; ++c) h = h * 33 + static_cast<uint8_t>(*c);
    uint32_t i = buckets[h % nbuckets];
    if (i < symoffset) return nullptr;
    for (;; ++i) {
      const uint32_t ch = chain[i - symoffset];
      if ((ch | 1) == (h | 1))
        if (void *p = match(i)) return p;
      if (ch & 1) return nullptr;
    }
  }
  if (sysv_hash) {
    const uint32_t nbucket = sysv_hash[0];
    const uint32_t *bucket = sysv_hash + 2, *chain = bucket + nbucket;
    uint32_t h = 0;
    for (const char *c = name; *c; ++c) {
      h = (h << 4) + static_cast<uint8_t>(*c);
      const uint32_t g = h & 0xf0000000;
      h ^= g >> 24;
      h &= ~g;
    }
    for (uint32_t i = bucket[h % nbucket]; i != STN_UNDEF; i = chain[i])
      if (void *p = match(i)) return p;
  }
  return nullptr;
}

void *dlsym_next_from(const void *caller, const char *name) {
  Dl_info info{};
  link_map *lm = nullptr;
  if (!dladdr1(caller, &info, reinterpret_cast<void **>(&lm), RTLD_DL_LINKMAP) || !lm)
    return real_dlsym()(RTLD_NEXT, name);
  for (lm = lm->l_next; lm; lm = lm->l_next)
    if (void *p = elf_lookup(lm, name)) return p;
  return nullptr;
}

}  // namespace
}  // namespace framux::gl

FRAMUX_EXPORT void *dlsym(void *handle, const char *name) {
  DlsymFn next = real_dlsym();
  if (!next) return nullptr;
  if (dlsym_hook_enabled()) {
    if (void *h = framux::gl::hook_for(name)) {
      // Devolve o hook só se o símbolo existe no handle pedido.
      void *orig = handle == RTLD_NEXT ? dlsym_next_from(__builtin_return_address(0), name) : next(handle, name);
      return orig ? h : nullptr;
    }
  }
  if (handle == RTLD_NEXT) return dlsym_next_from(__builtin_return_address(0), name);
  return next(handle, name);
}
