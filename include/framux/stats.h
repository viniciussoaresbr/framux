/* Interface pública de estatísticas do Framux (§C.3.3).
 *
 * O framuxd mantém $XDG_RUNTIME_DIR/framux/stats com este struct, atualizado
 * a até 10 Hz, para widgets, LCDs, OBS e scripts. Abra com O_RDONLY e mmap
 * com PROT_READ. Leitura consistente: repita enquanto `seq` for ímpar ou
 * mudar entre o início e o fim da cópia (seqlock).
 *
 * Também há o sinal D-Bus io.github.framux.Daemon1.StatsChanged (1 Hz).
 */
#ifndef FRAMUX_STATS_H
#define FRAMUX_STATS_H

#include <stdint.h>

#define FRAMUX_STATS_VERSION 1

enum framux_stats_flags {
  FRAMUX_STATS_BENCHMARK = 1u << 0,
  FRAMUX_STATS_RECORDING = 1u << 1,
  FRAMUX_STATS_LOOP = 1u << 2,
};

enum framux_stats_api {
  FRAMUX_API_NONE = 0,
  FRAMUX_API_VULKAN = 1,
  FRAMUX_API_GL_GLX = 2,
  FRAMUX_API_GL_EGL = 3,
};

struct framux_stats {
  uint32_t version;         /* FRAMUX_STATS_VERSION */
  uint32_t size;            /* sizeof(struct framux_stats) */
  uint32_t seq;             /* seqlock: ímpar durante a escrita */
  float fps;                /* 0 se o último quadro tiver mais de 999 ms */
  uint64_t frame_count;     /* quadros da sessão ativa */
  uint64_t last_present_ms; /* CLOCK_MONOTONIC do último present, em ms */
  float frametime_ms;
  uint32_t api; /* enum framux_stats_api */
  char game[64];
  uint32_t flags; /* enum framux_stats_flags */
  float bench_elapsed_s;
  float bench_limit_s; /* 0 = sem limite */
  int32_t pid;         /* processo da sessão ativa (0 = nenhuma) */
  uint32_t _reserved[8];
};

#endif
