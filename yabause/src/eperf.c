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

/* Cold path of the emulator-side performance counters (lazy init and once-per-second
 * output). The hot-path begin/end macros live in eperf.h and compile away when
 * profiling is disabled. The enable flag g_yaba_perf_enabled is set once by
 * YglPerfDetect() on the GL side; this file only reacts to it. */

#include <stdio.h>
#include <string.h>
#include "eperf.h"

