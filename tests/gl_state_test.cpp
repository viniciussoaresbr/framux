// §C.12 teste 16: overlay OpenGL num contexto core com estado "sujo".
// Roda com LD_PRELOAD=libframux_gl.so num servidor X (Xvfb + llvmpipe):
//  - o estado que o overlay toca é idêntico antes e depois do swap;
//  - o flag de erro do jogo (glGetError) não é alterado;
//  - o overlay foi de fato desenhado (pixel verde-limão, a cor padrão, no canto superior esquerdo).

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

struct State {
  GLboolean scissor, dither, srgb, discard;
  GLint box[4], draw_fb, read_fb, pack_align, pack_row, pbo;
  GLfloat clear[4];
  GLboolean mask[4];
};

State capture() {
  State s{};
  s.scissor = glIsEnabled(GL_SCISSOR_TEST);
  s.dither = glIsEnabled(GL_DITHER);
  s.srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  s.discard = glIsEnabled(GL_RASTERIZER_DISCARD);
  glGetIntegerv(GL_SCISSOR_BOX, s.box);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.draw_fb);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.read_fb);
  glGetIntegerv(GL_PACK_ALIGNMENT, &s.pack_align);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &s.pack_row);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s.pbo);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, s.clear);
  glGetBooleanv(GL_COLOR_WRITEMASK, s.mask);
  return s;
}

}  // namespace

int main() {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    std::fprintf(stderr, "sem DISPLAY\n");
    return 77;
  }
  const int fb_attrs[] = {GLX_X_RENDERABLE,
                          True,
                          GLX_DRAWABLE_TYPE,
                          GLX_WINDOW_BIT,
                          GLX_RENDER_TYPE,
                          GLX_RGBA_BIT,
                          GLX_RED_SIZE,
                          8,
                          GLX_GREEN_SIZE,
                          8,
                          GLX_BLUE_SIZE,
                          8,
                          GLX_DOUBLEBUFFER,
                          True,
                          None};
  int n = 0;
  GLXFBConfig *fbc = glXChooseFBConfig(dpy, DefaultScreen(dpy), fb_attrs, &n);
  if (!fbc || n == 0) return 77;
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
  XSetWindowAttributes swa{};
  swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
  Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 640, 480, 0, vi->depth, InputOutput, vi->visual,
                             CWColormap, &swa);
  XMapWindow(dpy, win);
  XSync(dpy, False);

  auto create = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
      glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));
  const int ctx_attrs[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB,    3,   GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB,
      GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
  GLXContext ctx = create(dpy, fbc[0], nullptr, True, ctx_attrs);
  if (!ctx) return 77;
  glXMakeCurrent(dpy, win, ctx);
  std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

  // Estado "sujo" típico de um jogo no fim do quadro.
  GLuint fbo, tex, pbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glGenBuffers(1, &pbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
  glEnable(GL_SCISSOR_TEST);
  glScissor(3, 5, 7, 11);
  glEnable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_DITHER);
  glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
  glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, 17);

  // Fundo azul no framebuffer padrão, para o overlay contrastar.
  for (int frame = 0; frame < 3; ++frame) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Restaura o estado sujo antes do swap.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
    while (glGetError() != GL_NO_ERROR) {
    }
    glEnable(0x7fff);  // gera GL_INVALID_ENUM pendente, como um jogo com bug

    const State before = capture();
    glXSwapBuffers(dpy, win);
    const State after = capture();
    CHECK(glGetError() == GL_INVALID_ENUM);  // o erro do jogo sobreviveu ao overlay
    CHECK(glGetError() == GL_NO_ERROR);      // e o overlay não gerou outros
    CHECK(std::memcmp(&before, &after, sizeof before) == 0);
  }

  // O conteúdo apresentado fica no servidor X: lê a janela com XGetImage.
  glFinish();
  XSync(dpy, False);
  XImage *xi = XGetImage(dpy, win, 0, 0, 64, 64, AllPlanes, ZPixmap);
  int counter = 0;
  for (int y = 0; xi && y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const unsigned long p = XGetPixel(xi, x, y);
      const unsigned r = (p & xi->red_mask) >> 16, g = (p & xi->green_mask) >> 8, b = p & xi->blue_mask;
      if (r > 150 && g > 200 && b < 100) ++counter;  // #C6F432
    }
  if (xi) XDestroyImage(xi);
  std::printf("pixels do contador no canto: %d\n", counter);
  CHECK(counter > 10);

  glXMakeCurrent(dpy, None, nullptr);
  glXDestroyContext(dpy, ctx);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  std::printf("%s\n", failures ? "FALHOU" : "ok");
  return failures ? 1 : 0;
}
