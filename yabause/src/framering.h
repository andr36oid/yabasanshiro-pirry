/* framering.h: in-memory ring buffer for FRAMELOG diagnostics.
   Collects the (normally compiled-out) FRAMELOG points of vdp1.cpp/vdp2.cpp/ygles.c
   in a ring of 8192 entries. Dump: SIGUSR2 -> ~/.yabasanshiro/framering.txt
   (the handler is not async-signal-safe; accepted for diagnostics).
   Include after debug.h (FRAMELOG is #undef'ed here). Define FRAMERING_IMPL in
   exactly ONE C/C++ file first (storage + init + dump). */
#ifndef FRAMERING_H
#define FRAMERING_H

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#define FRAMERING_N   8192
#define FRAMERING_LEN 160

#ifdef __cplusplus
extern "C" {
#endif

extern int g_framering_on;
extern char g_framering_buf[FRAMERING_N][FRAMERING_LEN];
extern volatile unsigned int g_framering_idx;
void FrameRingInit(void);

static inline void FrameRingLog(const char *fmt, ...) {
  /* Kept as a second guard in case FrameRingLog is called directly instead of
     through the FRAMELOG macro. */
  if (!g_framering_on) return;
  unsigned int i = __sync_fetch_and_add(&g_framering_idx, 1) & (FRAMERING_N - 1);
  char *slot = g_framering_buf[i];
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  int n = snprintf(slot, FRAMERING_LEN, "%llu.%06lu t%04lx ",
                   (unsigned long long)ts.tv_sec, ts.tv_nsec / 1000,
                   (unsigned long)pthread_self() & 0xffff);
  if (n < 0 || n >= FRAMERING_LEN) return;
  va_list ap; va_start(ap, fmt);
  vsnprintf(slot + n, FRAMERING_LEN - n, fmt, ap);
  va_end(ap);
}

#ifdef FRAMERING_IMPL
int g_framering_on = 0;
char g_framering_buf[FRAMERING_N][FRAMERING_LEN];
volatile unsigned int g_framering_idx = 0;

static void FrameRingDumpSig(int sig) {
  (void)sig;
  const char *home = getenv("HOME");
  char path[512];
  snprintf(path, sizeof(path), "%s/.yabasanshiro/framering.txt", home ? home : "/tmp");
  FILE *f = fopen(path, "w");
  if (!f) return;
  unsigned int end = g_framering_idx;
  unsigned int start = (end > FRAMERING_N) ? end - FRAMERING_N : 0;
  for (unsigned int i = start; i < end; i++) {
    const char *s = g_framering_buf[i & (FRAMERING_N - 1)];
    if (s[0]) {
      fputs(s, f);
      if (!strchr(s, '\n')) fputc('\n', f);
    }
  }
  fclose(f);
}

void FrameRingInit(void) {
  /* Nothing to initialise: FRAMELOG is empty in release builds. */
  (void)0;
}
#endif /* FRAMERING_IMPL */

#ifdef __cplusplus
}
#endif

#undef FRAMELOG
#define FRAMELOG(...) do { } while (0)

#endif /* FRAMERING_H */
