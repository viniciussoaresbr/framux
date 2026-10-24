// Imita o gameoverlayrenderer.so da Steam: intercepta dlopen e resolve o
// dlopen real com dlsym(RTLD_NEXT) a cada chamada. Pré-carregado depois do
// shim, isso fazia o dlsym do shim entrar em recursão infinita.
#define _GNU_SOURCE
#include <dlfcn.h>

__attribute__((visibility("default"))) void *dlopen(const char *file, int mode) {
  void *(*real)(const char *, int) = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
  return real ? real(file, mode) : 0;
}
