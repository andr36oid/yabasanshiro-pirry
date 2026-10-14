/*
        Copyright 2026

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#ifndef _EPERF_H_
#define _EPERF_H_

/* Emulator-side counterpart of yglperf.h/.c. A single runtime switch,
 * g_yaba_perf_enabled (declared in yglperf.h/.c), controls both sides. When
 * disabled, each measuring point costs one global read and one branch.
 *
 * With YABA_PERF=1 one line per second is written to stderr, prefixed "EPERF:":
 *
 *    EPERF: n=<frames> sh2=<ms> vdp2cpu=<ms> rbgline=<ms> sndsync=<ms> rest=<ms> \
 *           dyn_comp=<n/s> dyn_new=<n/s> dyn_reuse=<n/s> dyn_evict=<n/s> \
 *           dyn_inval=<n/s> dyn_ppush=<n/s> dyn_prem=<n/s>
 *
 * All ms values are per-frame means over the elapsed 1 s window. The dyn_*
 * fields are events per second (deltas of the cumulative CompileBlocks counters
 * in DynarecSh2.h/.cpp), not means. In a healthy session dyn_new drops towards 0
 * after warm-up; a persistently high or rising value indicates thrashing of the
 * block ring cache. dyn_evict > 0 means the ring has wrapped at least once.
 * dyn_ppush vs. dyn_prem shows whether LookupParentTable grows.
 *
 * Thread mapping of the measuring points:
 *   - sh2      (yabause.c, SH2Exec block)        emulation thread
 *   - sndsync  (scsp.c, SyncSh2And68k)           emulation thread
 *   - vdp2cpu  (vdp2.cpp, Vdp2DrawScreens in vdp2VBlankOUT) runs on the separate
 *     VdpProc render thread; the emulation thread only posts the event and waits
 *     later (Vdp1JoinPendingRenders).
 *   - rbgline  (vidogl.c, Vdp2DrawRotation_in) runs on the VdpProc thread when the
 *     compute-shader path is used. It covers program selection, compilation,
 *     SSBO upload and dispatch submission, not GPU execution time (not reliably
 *     queryable under GLES 3.1 on Mali).
 *
 * sh2_ms/sndsync_ms are sums on the emulation thread (no lock needed).
 * vdp2cpu_ms/rbgline_ms are accumulated from another thread and protected by
 * g_eperf_xt_mtx (EPERF_XT_BEGIN/END, using a local stack t0). "rest" is
 * frame time minus all four; since vdp2cpu/rbgline overlap with the emulation
 * frame, a negative rest value is possible and is itself a measurement. */


/* Empty in release builds. EPERF_XT_BEGIN is a declaration plus statement in the
   diagnostic build, so its replacement must also be a statement. */

#define EPERF_BEGIN(fld)            do { } while (0)
#define EPERF_END(fld, accum)       do { } while (0)
#define EPERF_JOIN_COUNT()          do { } while (0)
#define EPERF_JOIN_BLOCK()          do { } while (0)
/* Empty in release builds, like the other EPERF macros. */
static void EPerfBlockNote(unsigned int befehle, unsigned int mem) { (void)befehle; (void)mem; }
#define EPERF_TPERF_SPR(feld)       do { } while (0)
#define EPERF_TPERF_LAY(bit)        do { } while (0)
#define EPERF_TPERF_ADD(feld, wert) do { } while (0)
#define EPERF_XT_BEGIN(t0var)       ((void)0)
#define EPERF_XT_END(t0var, accum)  do { } while (0)

static inline void EPerfFrameBegin(void) { }
static inline void EPerfFrameTick(void)  { }


#endif
