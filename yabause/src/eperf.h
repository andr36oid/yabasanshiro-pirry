/*
        Copyright 2026 pirrypirrypirry

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

/* Emulator timing instrumentation hooks. They are compiled out: the call sites
   remain but expand to no-ops. */

#define EPERF_BEGIN(fld)            do { } while (0)
#define EPERF_END(fld, accum)       do { } while (0)
#define EPERF_JOIN_COUNT()          do { } while (0)
#define EPERF_JOIN_BLOCK()          do { } while (0)
static inline void EPerfBlockNote(unsigned int ops, unsigned int mem) { (void)ops; (void)mem; }
#define EPERF_TPERF_SPR(feld)       do { } while (0)
#define EPERF_TPERF_LAY(bit)        do { } while (0)
#define EPERF_TPERF_ADD(feld, wert) do { } while (0)
#define EPERF_XT_BEGIN(t0var)       ((void)0)
#define EPERF_XT_END(t0var, accum)  do { } while (0)

static inline void EPerfFrameBegin(void) { }
static inline void EPerfFrameTick(void)  { }


#endif
