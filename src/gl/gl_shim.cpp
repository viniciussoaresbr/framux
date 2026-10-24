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

// ---------------------------------------------------------------------------
// Estado por contexto

// Funções do GL 1.x não têm PFN em glext.h; decltype das declarações de
// GL/gl.h dá o tipo sem criar referência à libGL.
struct GlFns {
  decltype(&::glGetString) GetString;
  PFNGLGETSTRINGIPROC GetStringi;
  decltype(&::glGetIntegerv) GetIntegerv;
  decltype(&::glGetFloatv) GetFloatv;
  decltype(&::glGetBooleanv) GetBooleanv;
  decltype(&::glIsEnabled) IsEnabled;
  decltype(&::glEnable) Enable;
  decltype(&::glDisable) Disable;
  decltype(&::glScissor) Scissor;
  decltype(&::glClearColor) ClearColor;
  decltype(&::glClear) Clear;
  decltype(&::glColorMask) ColorMask;
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
  PFNGLBINDFRAMEBUFFEREXTPROC BindFramebufferEXT;
  decltype(&::glReadBuffer) ReadBuffer;
  decltype(&::glReadPixels) ReadPixels;
  decltype(&::glPixelStorei) PixelStorei;
  PFNGLGENBUFFERSPROC GenBuffers;
  PFNGLDELETEBUFFERSPROC DeleteBuffers;
  PFNGLBINDBUFFERPROC BindBuffer;
  PFNGLBUFFERDATAPROC BufferData;
  PFNGLMAPBUFFERRANGEPROC MapBufferRange;
  PFNGLUNMAPBUFFERPROC UnmapBuffer;
  PFNGLFENCESYNCPROC FenceSync;
  PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
  PFNGLDELETESYNCPROC DeleteSync;
};

struct PendingRead {
  client::CaptureRequest req;
  GLuint pbo = 0;
  GLsync fence = nullptr;
  uint32_t width = 0, height = 0;
  uint64_t present_ns = 0;
};

struct Ctx {
  bool loaded = false;
  bool usable = false;
  bool es = false;
  int major = 0, minor = 0;
  bool has_fbo = false;        // GL_DRAW_FRAMEBUFFER_BINDING / GL_FRAMEBUFFER_BINDING
  bool split_fbo = false;      // binding de leitura e escrita separados (GL ≥ 3.0 / ES ≥ 3.0)
  bool fbo_ext = false;        // só EXT_framebuffer_object (GL 2.x)
  bool has_srgb = false;       // GL_FRAMEBUFFER_SRGB
  bool has_discard = false;    // GL_RASTERIZER_DISCARD
  bool has_pbo_async = false;  // PBO + fence sync
  GlFns f{};
  PendingRead pending;
  bool info_sent = false;
};

std::mutex g_mu;
std::unordered_map<const void *, Ctx> g_ctx;
std::unordered_map<uintptr_t, uint32_t> g_drawable_ids;

bool at_least(const Ctx &c, int maj, int min) { return c.major > maj || (c.major == maj && c.minor >= min); }

bool has_ext(const Ctx &c, const char *name) {
  if (!c.es && at_least(c, 3, 0) && c.f.GetStringi) {
    GLint n = 0;
    c.f.GetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
      const char *e = reinterpret_cast<const char *>(c.f.GetStringi(GL_EXTENSIONS, GLuint(i)));
      if (e && !std::strcmp(e, name)) return true;
    }
    return false;
  }
  const char *all = reinterpret_cast<const char *>(c.f.GetString(GL_EXTENSIONS));
  if (!all) return false;
  const size_t len = std::strlen(name);
  for (const char *p = all; (p = std::strstr(p, name)); p += len)
    if ((p == all || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) return true;
  return false;
}

template <class T>
void load(T &out, void *(*gpa)(const char *), const char *name) {
  out = reinterpret_cast<T>(gpa(name));
}

void *glx_gpa(const char *name) {
  return reinterpret_cast<void *>(real().glXGetProcAddressARB(reinterpret_cast<const GLubyte *>(name)));
}
void *egl_gpa(const char *name) { return reinterpret_cast<void *>(real().eglGetProcAddress(name)); }

void init_ctx(Ctx &c, void *(*gpa)(const char *)) {
  c.loaded = true;
  GlFns &f = c.f;
#define L(fn) load(f.fn, gpa, "gl" #fn)
  L(GetString);
  L(GetStringi);
  L(GetIntegerv);
  L(GetFloatv);
  L(GetBooleanv);
  L(IsEnabled);
  L(Enable);
  L(Disable);
  L(Scissor);
  L(ClearColor);
  L(Clear);
  L(ColorMask);
  L(BindFramebuffer);
  L(BindFramebufferEXT);
  L(ReadBuffer);
  L(ReadPixels);
  L(PixelStorei);
  L(GenBuffers);
  L(DeleteBuffers);
  L(BindBuffer);
  L(BufferData);
  L(MapBufferRange);
  L(UnmapBuffer);
  L(FenceSync);
  L(ClientWaitSync);
  L(DeleteSync);
#undef L
  if (!f.GetString || !f.GetIntegerv || !f.GetFloatv || !f.GetBooleanv || !f.IsEnabled || !f.Enable || !f.Disable ||
      !f.Scissor || !f.ClearColor || !f.Clear || !f.ColorMask)
    return;
  const char *ver = reinterpret_cast<const char *>(f.GetString(GL_VERSION));
  if (!ver) return;
  const char *p = ver;
  if (!std::strncmp(p, "OpenGL ES", 9)) {
    c.es = true;
    p += 9;
    while (*p && (*p < '0' || *p > '9')) ++p;
  }
  std::sscanf(p, "%d.%d", &c.major, &c.minor);

  if (c.es) {
    c.has_fbo = true;  // ES 2.0 já tem framebuffer objects
    c.split_fbo = at_least(c, 3, 0);
    c.has_discard = at_least(c, 3, 0);
    c.has_srgb = has_ext(c, "GL_EXT_sRGB_write_control");
    c.has_pbo_async = at_least(c, 3, 0);
  } else {
    c.split_fbo = at_least(c, 3, 0) || has_ext(c, "GL_ARB_framebuffer_object");
    c.fbo_ext = !c.split_fbo && has_ext(c, "GL_EXT_framebuffer_object");
    c.has_fbo = c.split_fbo || c.fbo_ext;
    c.has_discard = at_least(c, 3, 0);
    c.has_srgb = at_least(c, 3, 0) || has_ext(c, "GL_ARB_framebuffer_sRGB") || has_ext(c, "GL_EXT_framebuffer_sRGB");
    c.has_pbo_async = at_least(c, 3, 2) || (at_least(c, 3, 0) && has_ext(c, "GL_ARB_sync"));
  }
  if (c.has_fbo && !f.BindFramebuffer && !f.BindFramebufferEXT) c.has_fbo = c.split_fbo = false;
  if (c.has_pbo_async && (!f.GenBuffers || !f.BindBuffer || !f.BufferData || !f.MapBufferRange || !f.UnmapBuffer ||
                          !f.FenceSync || !f.ClientWaitSync || !f.DeleteSync))
    c.has_pbo_async = false;
  c.usable = true;
}

void bind_draw_fb(const Ctx &c, GLuint fb) {
  if (c.split_fbo)
    c.f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
  else if (c.fbo_ext)
    c.f.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
  else if (c.has_fbo)
    c.f.BindFramebuffer(GL_FRAMEBUFFER, fb);
}

void set_enabled(const Ctx &c, GLenum cap, GLboolean on) { on ? c.f.Enable(cap) : c.f.Disable(cap); }

// Estado salvo em volta do overlay.
struct SavedDrawState {
  GLboolean scissor_test, dither, srgb = GL_FALSE, discard = GL_FALSE;
  GLint scissor_box[4];
  GLfloat clear_color[4];
  GLboolean color_mask[4];
  GLint draw_fb = 0;
};

bool begin_draw(const Ctx &c, SavedDrawState &s) {
  const GlFns &f = c.f;
  if (c.has_fbo) f.GetIntegerv(c.split_fbo ? GL_DRAW_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &s.draw_fb);
  if (s.draw_fb != 0) bind_draw_fb(c, 0);
  if (!c.es) {
    // O draw buffer pertence ao framebuffer padrão; se o jogo desenha no
    // front buffer, não mexe (pular é mais seguro que trocar).
    GLint db = GL_BACK;
    f.GetIntegerv(GL_DRAW_BUFFER, &db);
    if (db != GL_BACK && db != GL_BACK_LEFT) {
      if (s.draw_fb != 0) bind_draw_fb(c, GLuint(s.draw_fb));
      return false;
    }
  }
  s.scissor_test = f.IsEnabled(GL_SCISSOR_TEST);
  s.dither = f.IsEnabled(GL_DITHER);
  if (c.has_srgb) s.srgb = f.IsEnabled(GL_FRAMEBUFFER_SRGB);
  if (c.has_discard) s.discard = f.IsEnabled(GL_RASTERIZER_DISCARD);
  f.GetIntegerv(GL_SCISSOR_BOX, s.scissor_box);
  f.GetFloatv(GL_COLOR_CLEAR_VALUE, s.clear_color);
  f.GetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);

  if (!s.scissor_test) f.Enable(GL_SCISSOR_TEST);
  if (s.dither) f.Disable(GL_DITHER);
  if (s.srgb) f.Disable(GL_FRAMEBUFFER_SRGB);
  if (s.discard) f.Disable(GL_RASTERIZER_DISCARD);
  f.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  return true;
}

void end_draw(const Ctx &c, const SavedDrawState &s) {
  const GlFns &f = c.f;
  f.ColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
  f.ClearColor(s.clear_color[0], s.clear_color[1], s.clear_color[2], s.clear_color[3]);
  f.Scissor(s.scissor_box[0], s.scissor_box[1], s.scissor_box[2], s.scissor_box[3]);
  set_enabled(c, GL_SCISSOR_TEST, s.scissor_test);
  set_enabled(c, GL_DITHER, s.dither);
  if (c.has_srgb) set_enabled(c, GL_FRAMEBUFFER_SRGB, s.srgb);
  if (c.has_discard) set_enabled(c, GL_RASTERIZER_DISCARD, s.discard);
  if (s.draw_fb != 0) bind_draw_fb(c, GLuint(s.draw_fb));
}

void clear_rects(const Ctx &c, const std::vector<ORect> &rects, OColor color, uint32_t w, uint32_t h) {
  c.f.ClearColor(color.r, color.g, color.b, color.a);
  for (const ORect &r : rects) {
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(int(w), r.x + r.w), y1 = std::min(int(h), r.y + r.h);
    if (x1 <= x0 || y1 <= y0) continue;
    c.f.Scissor(x0, int(h) - y1, x1 - x0, y1 - y0);  // origem do GL embaixo
    c.f.Clear(GL_COLOR_BUFFER_BIT);
  }
}

void draw_overlay(const Ctx &c, const OverlayDraw &d, uint32_t w, uint32_t h) {
  SavedDrawState s;
  if (!begin_draw(c, s)) return;
  if (d.has_box) clear_rects(c, {d.box}, d.box_color, w, h);
  clear_rects(c, d.outline, d.outline_color, w, h);
  clear_rects(c, d.text, d.text_color, w, h);
  end_draw(c, s);
}

// Estado de leitura salvo em volta do glReadPixels.
struct SavedReadState {
  GLint pack_buffer = 0, read_fb = 0, read_buffer = 0;
  GLint pack[8] = {};
};

const GLenum kPackDesktop[] = {GL_PACK_ALIGNMENT,  GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS,  GL_PACK_SKIP_ROWS,
                               GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST,  GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES};
const GLenum kPackEs3[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};

size_t pack_count(const Ctx &c) { return c.es ? (at_least(c, 3, 0) ? 4 : 1) : 8; }
const GLenum *pack_enums(const Ctx &c) { return c.es ? kPackEs3 : kPackDesktop; }

void begin_read(const Ctx &c, SavedReadState &s, bool use_pbo) {
  const GlFns &f = c.f;
  if (use_pbo) f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s.pack_buffer);
  if (c.split_fbo) {
    f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.read_fb);
    if (s.read_fb) f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  } else if (c.has_fbo) {
    f.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.read_fb);
    if (s.read_fb) bind_draw_fb(c, 0);
  }
  const bool has_read_buffer = (!c.es || at_least(c, 3, 0)) && f.ReadBuffer;
  if (has_read_buffer) {
    f.GetIntegerv(GL_READ_BUFFER, &s.read_buffer);
    f.ReadBuffer(GL_BACK);
  }
  const GLenum *pe = pack_enums(c);
  for (size_t i = 0; i < pack_count(c); ++i) f.GetIntegerv(pe[i], &s.pack[i]);
  for (size_t i = 0; i < pack_count(c); ++i) f.PixelStorei(pe[i], pe[i] == GL_PACK_ALIGNMENT ? 4 : 0);
}

void end_read(const Ctx &c, const SavedReadState &s, bool use_pbo) {
  const GlFns &f = c.f;
  const GLenum *pe = pack_enums(c);
  for (size_t i = 0; i < pack_count(c); ++i) f.PixelStorei(pe[i], s.pack[i]);
  if ((!c.es || at_least(c, 3, 0)) && f.ReadBuffer) f.ReadBuffer(GLenum(s.read_buffer));
  if (c.split_fbo) {
    if (s.read_fb) f.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(s.read_fb));
  } else if (c.has_fbo && s.read_fb) {
    bind_draw_fb(c, GLuint(s.read_fb));
  }
  if (use_pbo) f.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(s.pack_buffer));
}

int make_memfd(const void *data, size_t size) {
  const int fd = memfd_create("framux-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return -1;
  size_t done = 0;
  bool ok = ftruncate(fd, off_t(size)) == 0;
  while (ok && done < size) {
    const ssize_t w = pwrite(fd, static_cast<const char *>(data) + done, size - done, off_t(done));
    if (w <= 0)
      ok = false;
    else
      done += size_t(w);
  }
  if (!ok) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
  return fd;
}

void send_pixels(const client::CaptureRequest &req, const void *data, uint32_t w, uint32_t h, uint64_t present_ns) {
  Runtime &rt = Runtime::get();
  const size_t size = size_t(w) * h * 4;
  const int fd = make_memfd(data, size);
  if (fd < 0) {
    rt.send_capture_error(req.request_id, "falha ao copiar o quadro");
    return;
  }
  ipc::FrameImage img{};
  img.request_id = req.request_id;
  img.present_ns = present_ns;
  img.width = w;
  img.height = h;
  img.vk_format = 37;  // VK_FORMAT_R8G8B8A8_UNORM
  img.stride = w * 4;
  img.size = size;
  img.flags = ipc::kImgFlipY;
  rt.send_frame_image(img, fd);
}

// Lê o backbuffer: com PBO + fence o mapeamento acontece num quadro
// seguinte, sem travar o pipeline; sem eles, leitura síncrona.
void start_capture(Ctx &c, const client::CaptureRequest &req, uint32_t w, uint32_t h, uint64_t present_ns) {
  const GlFns &f = c.f;
  if (c.pending.req.request_id || !f.ReadPixels || !f.PixelStorei) {
    Runtime::get().send_capture_error(req.request_id, "captura OpenGL indisponível");
    return;
  }
  SavedReadState s;
  const bool pbo = c.has_pbo_async;
  begin_read(c, s, pbo);
  if (pbo) {
    if (!c.pending.pbo) f.GenBuffers(1, &c.pending.pbo);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, c.pending.pbo);
    f.BufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(size_t(w) * h * 4), nullptr, GL_STREAM_READ);
    f.ReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    c.pending.fence = f.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    c.pending.req = req;
    c.pending.width = w;
    c.pending.height = h;
    c.pending.present_ns = present_ns;
    end_read(c, s, pbo);
    return;
  }
  std::vector<uint8_t> buf(size_t(w) * h * 4);
  f.ReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
  end_read(c, s, pbo);
  send_pixels(req, buf.data(), w, h, present_ns);
}

void poll_capture(Ctx &c) {
  PendingRead &p = c.pending;
  if (!p.req.request_id || !p.fence) return;
  const GlFns &f = c.f;
  const GLenum st = f.ClientWaitSync(p.fence, 0, 0);
  if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) return;
  f.DeleteSync(p.fence);
  p.fence = nullptr;
  GLint prev = 0;
  f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev);
  f.BindBuffer(GL_PIXEL_PACK_BUFFER, p.pbo);
  const size_t size = size_t(p.width) * p.height * 4;
  if (const void *data = f.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(size), GL_MAP_READ_BIT)) {
    send_pixels(p.req, data, p.width, p.height, p.present_ns);
    f.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
  } else {
    Runtime::get().send_capture_error(p.req.request_id, "falha ao mapear o PBO");
  }
  f.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(prev));
  p.req = {};
}

// ---------------------------------------------------------------------------
// Present comum a GLX e EGL

void on_swap(ipc::Api api, const void *ctx_key, uintptr_t drawable, uint32_t w, uint32_t h,
             void *(*gpa)(const char *)) {
  Runtime &rt = Runtime::get();
  Ctx *c = nullptr;
  if (ctx_key) {
    std::lock_guard lk(g_mu);
    c = &g_ctx[ctx_key];
  }
  if (c && !c->loaded) init_ctx(*c, gpa);
  if (c && c->usable && !c->info_sent) {
    c->info_sent = true;
    const char *ver = reinterpret_cast<const char *>(c->f.GetString(GL_VERSION));
    const char *ren = reinterpret_cast<const char *>(c->f.GetString(GL_RENDERER));
    rt.set_device_info("", ver ? ver : "", ren ? ren : "");
  }
  if (!rt.init(api)) return;

  uint32_t id;
  {
    std::lock_guard lk(g_mu);
    uint32_t &slot = g_drawable_ids[drawable];
    if (!slot) slot = rt.new_swapchain_id();
    id = slot;
  }
  if (api == ipc::Api::GlGlx) {
    static uintptr_t last_window = 0;
    if (drawable != last_window) {
      last_window = drawable;
      rt.set_x11_window(uint32_t(drawable));
    }
  }

  client::PresentResult res;
  rt.on_present(id, w, h, res);
  const uint64_t present_ns = now_ns();
  if (!c || !c->usable || w == 0 || h == 0) {
    if (res.capture) rt.send_capture_error(res.capture->request_id, "sem contexto OpenGL utilizável");
    return;
  }
  poll_capture(*c);
  if (res.capture && !res.capture->include_overlay) start_capture(*c, *res.capture, w, h, present_ns);
  if (res.draw) draw_overlay(*c, *res.draw, w, h);
  if (res.capture && res.capture->include_overlay) start_capture(*c, *res.capture, w, h, present_ns);
}

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
