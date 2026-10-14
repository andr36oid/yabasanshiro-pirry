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
#ifndef _YGLPERF_H_
#define _YGLPERF_H_

/* GL timing instrumentation hooks. They are compiled out: the call sites
   remain but expand to no-ops. */

#define YGLPERF_BEGIN(fld)       do { } while (0)
#define YGLPERF_END(fld, accum)  do { } while (0)
#define YGLPERF_INC(field)       do { } while (0)

static inline void YglPerfDetect(void)    { }
static inline void YglPerfFieldTick(void) { }


#endif
