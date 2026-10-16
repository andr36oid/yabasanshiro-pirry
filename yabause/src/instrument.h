/*  Copyright 2026 pirrypirrypirry

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
#ifndef YABA_INSTRUMENT_H
#define YABA_INSTRUMENT_H

/* Development instrumentation (performance counters and wall-clock timers).
   Configure with -DYABA_INSTRUMENT=ON to compile it in: the counters are then
   updated and vdp2.cpp prints one "INSTR:" line per second to stderr.
   Release builds leave the option off; INSTR(x) then expands to nothing and
   the timer blocks guarded by #ifdef YABA_INSTRUMENT are not compiled. */
#ifdef YABA_INSTRUMENT
#define INSTR(x) ((void)(x))
#else
#define INSTR(x) ((void)0)
#endif

#endif
