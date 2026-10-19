/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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


#include <stdio.h>
#include <stdlib.h>
#include "../instrument.h"

/* Diagnostic checks in the hot execution path are fixed to constant false, so
   the compiler removes them. Development counters are compiled in only with
   YABA_INSTRUMENT (instrument.h). */
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <stdint.h>
#include <time.h>  /* clock_gettime/struct timespec */
#include <core.h>
#include <unordered_map>

#include "sh2core.h"
#include "debug.h"
#include "yabause.h"
#include "bios.h"
extern "C" {
#include "scu.h"
}

#include "DynarecSh2.h"
#include "opcodes.h"
#if defined(WEBINTERFACE)
#define DEBUG_CPU
#endif
//#define DEBUG_CPU
//#define EXECUTE_STAT
//#define BUILD_INFO
//#define LOG printf


CompileBlocks * CompileBlocks::instance_ = NULL;

/* The creation branch, deliberately not inline; see getInstance() in
   DynarecSh2.h. Runs exactly once per program run. */
__attribute__((noinline)) CompileBlocks * CompileBlocks::createInstance() {
  if (instance_ == NULL) {
    instance_ = new CompileBlocks();
  }
  return instance_;
}
DynarecSh2 * DynarecSh2::CurrentContext = NULL;
extern "C" unsigned long long g_sync_cycles[2];   /* DynarecSh2CInterface.cpp */



/* The counters must be defined outside the anonymous namespace (which starts
   just below), otherwise they get internal linkage and vdp2.cpp cannot link to
   them. */
/* Not gated: these are the idle jumps per CPU, and the bgm:l46m ratio is the
   criterion for an idle-loop failure. They cost far less than the block entry
   counters. alignas keeps master and slave off the same cache line. */
alignas(64) unsigned int g_ls46_m = 0;
alignas(64) unsigned int g_ls46_s = 0;

/* Defined before `namespace {`, otherwise internal linkage and a link error.
   Mode: 0 off, 1 always on, 2 adaptive (decided once per second in vdp2.cpp,
   ZserstAdaptSekunde). */
volatile int g_zserst_adapt_an = 0;
int ZserstModus() { return 2; }


/* Entries dropped because the CHAIN2 / NACHPRUEF tables were full (counted
   only with YABA_INSTRUMENT). */
unsigned int g_c2_verworfen = 0, g_np_verworfen = 0;

namespace {


/* Four asm_list entries had their write_count the wrong way round (the fourth
   argument of opdesc() is the number of write accesses of the instruction):
   MOVLP 0x6006 (mov.l @Rm+,Rn) is a read, MOVWM 0x2005 (mov.w Rm,@-Rn) a
   write, MOVBL4 0x8400 (mov.b @(d,Rm),R0) a read, MOVBS4 0x8000 (mov.b
   R0,@(d,Rn)) a write. write_memory_counter decides whether a loop may be
   skipped: a block that writes is rejected. Reads counted as writes reject
   pure wait loops; writes counted as write-free let a loop with side effects
   pass as harmless. The table itself is left untouched; this function
   corrects the value. */

inline unsigned OPFIX_write(unsigned op, unsigned aus_tabelle) {
  if ((op & 0xF00F) == 0x6006) return 0;  /* mov.l @Rm+,Rn    = read  */
  if ((op & 0xF00F) == 0x2005) return 1;  /* mov.w Rm,@-Rn    = write */
  if ((op & 0xFF00) == 0x8400) return 0;  /* mov.b @(d,Rm),R0 = read  */
  if ((op & 0xFF00) == 0x8000) return 1;  /* mov.b R0,@(d,Rn) = write */
  return aus_tabelle;
}

/* Sign fix for the branch target of bra/bsr. The 12-bit displacement is
   sign-extended in "temp", but the assignment to jumppc used the unsigned raw
   value, so every backward bra looked like a forward jump to the idle-loop
   detection. Every wait loop closed by bra was affected; loops closed by
   bt/bf use the 8-bit path with (signed char) and were correct. This is a fix,
   not a diagnostic tool, hence no NODIAG guard. */
inline bool BRAFIX_enabled() { return true; }

/* The adjacency condition of the two-block cycle (`adj_luecklos`) is
   deliberately not relaxed: relaxing it was measured harmful in several
   titles. */


inline bool CHAIN2_enabled() { return true; }
/* Partner ranges of the two-block idle cycles, deliberately kept outside
   struct Block: two extra u32 in Block cost several fps in every game even
   with the fix off, because Block contains a 4 KB code buffer and is walked
   millions of times (size and alignment change the cache behaviour). Only the
   actually marked blocks are stored here, so the table is almost always
   resolved at the first entry; chain2_letzter remembers the last hit, because
   the same loop is queried millions of times in a row. */
struct Chain2Eintrag { u32 block_b; u32 partner_b; u32 partner_e; };
static const unsigned CHAIN2_MAX = 64;

static Chain2Eintrag chain2_tab[CHAIN2_MAX];
static unsigned chain2_anzahl = 0;

/* Counters without a gate (they are not in the hot path): g_ls46_m/s count the
   only real idle-jump return (loopskip_cnt_ counts eight returns, including
   BIOS calls and undecoded cases); g_c2_verworfen counts cycles silently
   dropped because chain2_tab (CHAIN2_MAX entries) was full; g_np_verworfen
   the same for nachpruef_tab. The definitions stand before `namespace {`: inside the
   anonymous namespace they would get internal linkage and vdp2.cpp could not
   link to them. */
static unsigned chain2_letzter = 0;

/* Remove an already registered cycle again. Removing the chain2_tab entry is
   enough: without it Chain2ImPartner() returns false, so the runtime check
   does not see the program counter "in block". The BLOCK_LOOP flag may stay. */
static void Chain2VergessenPartner(u32 adr)
{
  unsigned d = 0;
  for (unsigned i = 0; i < chain2_anzahl; i++) {
    if (chain2_tab[i].partner_b == adr || chain2_tab[i].block_b == adr) {
      continue;
    }
    chain2_tab[d++] = chain2_tab[i];
  }
  chain2_anzahl = d;
  chain2_letzter = 0;
}

/* Like Chain2Merken, but an EXISTING entry is left untouched. Chain2Merken
   overwrites partner_b/partner_e if block_b is already in the table, which the
   late check must not do (a candidate that has a valid partner via the normal
   path would get the range of another target). chain2_tab also only holds
   CHAIN2_MAX entries, so non-firing late additions would displace real
   cycles. */
static void Chain2MerkenNurNeu(u32 block_b, u32 partner_b, u32 partner_e) {
  for (unsigned i = 0; i < chain2_anzahl; i++)
    if (chain2_tab[i].block_b == block_b) return;  /* already present: do not touch */
  if (chain2_anzahl < CHAIN2_MAX) {
    chain2_tab[chain2_anzahl].block_b   = block_b;
    chain2_tab[chain2_anzahl].partner_b = partner_b;
    chain2_tab[chain2_anzahl].partner_e = partner_e;
    chain2_anzahl++;
  }
  else INSTR(g_c2_verworfen++);
}

/* Real cycle test instead of adjacency. Block 1 ends at one address and block
   2 begins further on; `adj_luecklos` asks whether block 1 falls through into
   block 2, which fails for a cycle that jumps over a gap. Adjacency was only
   ever a proxy for "the two blocks form a closed cycle". Here block 1 must end
   with a jump whose target is block 2; that block 2 jumps back to block 1 is
   already known at the call site. So the cycle is verified in both
   directions, which is stricter than adjacency: a coincidentally adjacent
   block without a branch back is rejected. Not relaxed, as relaxing the
   condition was measured harmful. No new field in struct Block (it costs
   speed) and no new table; the final instruction of block 1 is read and
   decoded once, when block 2 is translated. */
/* `page->e_addr = addr-2` is the address of a block's LAST instruction. The
   runtime idle check therefore uses `<=` (SysReg[3] <= pBlock->e_addr), as the
   branch back of a wait loop sits there. This one condition covers the single
   instruction self loop, the adjacent two-block cycle and the cycle over a
   jump. IN_INFINITY_LOOP then only does SET_COUNT(targetcnt): skipping lasts
   to the end of the current time slice, after which the check is repeated.
   BLOCK_LOOP is set in several places: the SLEEP opcode, two fixed
   instruction patterns (tst #imm,r0 / bt and tst r0,r0 / bt.s), the
   single-block case (jumppc inside the block, write-free), the two-block case
   (CHAIN2/ZYKEL2) and the late check. */
/* RANDFIX2: the partner range check includes the last instruction (<= instead
   of <); see Chain2ImPartner. */
static bool RANDFIX2_enabled() { return true; }


/* ZYKSLAVE: see the long comment in Execute(). */
/* ZSERST: see the explanation at the point of effect in Execute(). */
static bool ZSERST_enabled() {
  const int m = ZserstModus();
  return m == 1 || (m == 2 && g_zserst_adapt_an);
}
static bool ZYKSLAVE_enabled() { return true; }

static bool ZYKEL2_enabled() { return true; }

/* Does the block b_addr..e_addr end with a jump to `ziel`? The last two
   instruction slots are checked: for a delayed branch e_addr is the delay slot
   and the branch instruction precedes it. Only PC-relative branches; jmp/rts
   through a register cannot be resolved statically and deliberately give
   false (better to miss a cycle than to skip a wrong one). */
/* A loop with a wandering read address is not idle. Example: a game sorts
   polygons by insertion into a linked list:
       mov.l @(4,r2),r1 ; mov.l @(4,r1),r1 ; cmp/hs r5,r1 ; bt out
       mov r2,r3 ; bra loop ; mov.l @r3,r2     (delay slot)
   This is real work whose progress lives only in registers. Detection only
   checks write_memory_counter == 0, so without this test the loop would be
   classified as endless, and IN_INFINITY_LOOP => SET_COUNT(targetcnt) would
   discard the rest of the time slice.
   Distinguishing feature: a load whose base register is itself loaded from
   memory within the cycle reads a different address on every pass, whereas a
   real wait loop polls the same address. The register chain is circular, so a
   fixed-point iteration over several rounds is needed.
   Deliberately conservative: only `mov Rm,Rn` carries the marker on, `@Rm+`
   has no special case, and when in doubt the answer is false (the previous
   behaviour). No new field in struct Block (that measurably cost speed).
   This runs in EmmitCode for every translated block, so its cost matters; the
   block words are read only once (see below). */
/* Fixed level 4: cycle search plus constant-base relaxation. */
static int WANDERFIX_stufe() { return 4; }
/* Explicit extern "C" linkage; the reader in vdp2.cpp declares it identically.
   Free-running and never reset: the reader takes per-second differences. */
extern "C" unsigned long long g_wanderfix_abgelehnt = 0;  /* number of rejected cycles */

/* True if the range [b_addr..e_addr] contains a load whose base register is
   loaded from memory in the same range (or copied by mov from such a
   register). */
static bool SchleifeWandert(u32 b_addr, u32 e_addr)
{
  if (e_addr < b_addr) return false;
  if ((e_addr - b_addr) > 256) return false;  /* safety net */

  /* Read the block words once into a local array. The backwards search per
     load would otherwise call MappedMemoryReadWord up to ~8300 times per call,
     and this function runs in EmmitCode for every translated block. Nothing
     writes within one call, so every read site sees the same value as before;
     no predicate, order or exit condition changed. ZykelSpringtNach() still
     reads directly. */
  const u32 worte = ((e_addr - b_addr) >> 1) + 1;
  if (worte > 129) return false;  /* belt and braces */
  u16 w[129];
  for (u32 i = 0; i < worte; i++)
    w[i] = MappedMemoryReadWord(b_addr + (i << 1), NULL);

  /* Wandering detection. A load wanders only if its address depends
     circularly on itself: r is loaded from memory via a base that (directly
     or via copies) derives from r again (e.g. r2 = *r3, r3 = r2).
     Post-increment (`@Rm+`) is the trivial self-cycle. The dependency graph of
     one loop pass is built, and the question is whether the base register of a
     load lies on a cycle containing at least one load edge. The callers check
     `WANDERFIX_stufe() && SchleifeWandert(...)`, so there is no gate here. */
  {
    int  abh[16];  /* abh[r] = register r was last derived from */
    bool per_laden[16];  /* ... through a memory access? */
    for (int i = 0; i < 16; i++) { abh[i] = -1; per_laden[i] = false; }

    /* Pass 1: collect the last assignment per register */
    for (u32 a = b_addr; a <= e_addr; a += 2) {
      const u16 op = w[(a - b_addr) >> 1];
      const int n = (op >> 8) & 0xF;
      const int m = (op >> 4) & 0xF;

      if ((op & 0xF00F) == 0x6004 || (op & 0xF00F) == 0x6005
       || (op & 0xF00F) == 0x6006) {            /* mov.X @Rm+,Rn  */
        abh[m] = m; per_laden[m] = true;  /* Rm increments itself */
        abh[n] = m; per_laden[n] = true;
      }
      else if ((op & 0xF00F) == 0x6000 || (op & 0xF00F) == 0x6001
            || (op & 0xF00F) == 0x6002          /* mov.X @Rm,Rn   */
            || (op & 0xF000) == 0x5000          /* mov.l @(d,Rm),Rn */
            || (op & 0xF00F) == 0x000C || (op & 0xF00F) == 0x000D
            || (op & 0xF00F) == 0x000E) {       /* mov.X @(R0,Rm),Rn */
        abh[n] = m; per_laden[n] = true;
      }
      else if ((op & 0xFF00) == 0x8400
            || (op & 0xFF00) == 0x8500) {       /* mov.X @(d,Rm),R0 */
        abh[0] = m; per_laden[0] = true;
      }
      else if ((op & 0xF00F) == 0x6003) {       /* mov Rm,Rn (Kopie) */
        abh[n] = m; per_laden[n] = false;
      }
      /* Arithmetic on the base register is also an edge. Example:
         `add #4,r2 ; mov.l @r2,r1 ; cmp/eq ; bf` scans an array, but without
         an edge abh[2] would stay at its old value and the block would be
         skipped as idle. Same treatment as `mov.X @Rm+,Rn`, which sets
         abh[m] = m. It only takes effect if the wandering register is the base
         of a load, so a loop counter (`add #1,r0 ; mov.l @r5,r1 ; tst ; bt`)
         with a constant base is still skipped. SLEEP and the fixed wait
         patterns bypass this check. Only applied at stage 5. */
      else if (WANDERFIX_stufe() >= 5
            && ((op & 0xF000) == 0x7000            /* add #imm,Rn        */
             || (op & 0xF00F) == 0x300C            /* add Rm,Rn          */
             || (op & 0xF00F) == 0x3008            /* sub Rm,Rn          */
             || (op & 0xF0FF) == 0x4000            /* shll Rn            */
             || (op & 0xF0FF) == 0x4010)) {        /* dt Rn              */
        abh[n] = n; per_laden[n] = true;   /* Rn modifies itself */
      }
      /* A register set to a constant breaks the chain. Example:
         `mov.l @(disp,PC),r0` before `mov.l @r0,r0` makes r0 the same fixed
         address on every pass, so the loop waits instead of wandering. The
         loop above only knows a few load forms and would otherwise leave
         abh[n] at its old value. Only cases where the target certainly becomes
         a constant; `add #imm,Rn` does not belong here (Rn stays dependent on
         itself). */
      else if (WANDERFIX_stufe() >= 3
            && ((op & 0xF000) == 0xE000          /* mov #imm,Rn        */
             || (op & 0xF000) == 0xD000          /* mov.l @(disp,PC),Rn */
             || (op & 0xF000) == 0x9000)) {      /* mov.w @(disp,PC),Rn */
        abh[n] = -1; per_laden[n] = false;
      }
      else if (WANDERFIX_stufe() >= 3
            && (op & 0xFF00) == 0xC700) {        /* mova @(disp,PC),R0 */
        abh[0] = -1; per_laden[0] = false;
      }
      /* anything else: no address dependency that we track */
    }
    /* abh[] only holds the LAST assignment per register (a snapshot at block
       end, not a sequence), so the assignment above is overwritten by a later
       load. The real check is the constant-before-load test below. */

    /* wandert[r]: trace back from register r; if the path returns to r and at
       least one load was on it, it is a cycle. */
    unsigned int wandert = 0;
    for (int r = 0; r < 16; r++) {
      int  k = r;
      bool lade_kante = false;
      for (int schritt = 0; schritt < 16; schritt++) {
        if (per_laden[k]) lade_kante = true;
        k = abh[k];
        if (k < 0) break;  /* chain ends: no cycle */
        if (k == r) { if (lade_kante) wandert |= (1u << r); break; }
      }
    }
    if (wandert == 0) return false;

    /* Pass 2: does a load use a wandering base register? */
    for (u32 a = b_addr; a <= e_addr; a += 2) {
      const u16 op = w[(a - b_addr) >> 1];
      const int m = (op >> 4) & 0xF;
      int basis = -1;
      if ((op & 0xF00F) == 0x6000 || (op & 0xF00F) == 0x6001
       || (op & 0xF00F) == 0x6002
       || (op & 0xF00F) == 0x6004 || (op & 0xF00F) == 0x6005
       || (op & 0xF00F) == 0x6006
       || (op & 0xF000) == 0x5000
       || (op & 0xF00F) == 0x000C || (op & 0xF00F) == 0x000D
       || (op & 0xF00F) == 0x000E
       || (op & 0xFF00) == 0x8400 || (op & 0xFF00) == 0x8500) basis = m;
      else continue;

      /* Is the base register set to a constant earlier in the block? Only here
         the position `a` is known; abh[] cannot tell, as it has no ordering.
         Example: `mov.l @(disp,PC),r0` followed by `mov.l @r0,r0` reads the
         same address on every pass, so the loop waits on a memory location
         instead of wandering. Without this check it would be classified as
         real work and no longer skipped. Conservative: only PC-relative loads,
         `mov #imm` and `mova`; `add #imm,Rn` stays out, because Rn then stays
         dependent on itself. Looks backwards only, down to b_addr: what comes
         after the load only matters in the next pass. */
      if (basis >= 0 && (wandert & (1u << basis)) && WANDERFIX_stufe() >= 4) {
        bool konst_davor = false;
        for (u32 d = b_addr; d < a; d += 2) {
          const u16 od = w[(d - b_addr) >> 1];
          const int zn = (od >> 8) & 0xF;
          if (((od & 0xF000) == 0xD000 || (od & 0xF000) == 0x9000
            || (od & 0xF000) == 0xE000) && zn == basis) { konst_davor = true; }
          else if ((od & 0xFF00) == 0xC700 && basis == 0) { konst_davor = true; }
          /* If the register is reloaded from memory afterwards, the constant no
             longer counts. All load forms that pass 1 recognises must be
             covered here; a missing one would let a genuinely wandering cycle
             through as idle. For `@Rm+` the base register itself is also
             invalidated if it is Rm, hence the second condition on zm. */
          else if ((((od & 0xF00F) == 0x6000 || (od & 0xF00F) == 0x6001
                  || (od & 0xF00F) == 0x6002
                  || (od & 0xF00F) == 0x6004 || (od & 0xF00F) == 0x6005
                  || (od & 0xF00F) == 0x6006
                  || (od & 0xF000) == 0x5000
                  || (od & 0xF00F) == 0x000C || (od & 0xF00F) == 0x000D
                  || (od & 0xF00F) == 0x000E) && zn == basis)
                || (((od & 0xF00F) == 0x6004 || (od & 0xF00F) == 0x6005
                  || (od & 0xF00F) == 0x6006) && ((od >> 4) & 0xF) == basis)
                || (((od & 0xFF00) == 0x8400 || (od & 0xFF00) == 0x8500)
                  && basis == 0))
               { konst_davor = false; }
        }
        if (konst_davor) {
          continue;  /* this load does not indicate wandering */
        }
      }

      if (basis >= 0 && (wandert & (1u << basis))) {
        return true;
      }
    }
    return false;
  }

  /* No cycle found: the loop really waits and may be skipped. */
  return false;
}

/* A loop without memory reads whose comparison register is advanced by the
   loop itself is not idle. Example: a normalisation loop that halves r3/r5
   until they fit a range and terminates by its own work. Treating it as a
   two-block cycle and skipping it would burn the rest of the time slice on
   every pass. The wander check only looks at the base register of a load, and
   for two-block cycles it checks each block alone, so it misses this.
   Criterion (both blocks together):
     (1) no data-memory read instruction (any unknown instruction means "do not
         reject");
     (2) a conditional branch;
     (3) a register read by a compare (cmp/*, tst, dt) is advanced by the cycle
         itself (shift/rotate, add, sub, dt, neg) and not reset (mov,
         mov #imm, literal load = chain breakers).
   Such a loop cannot wait for anything external. Poll loops, `bf $`/`bra $`
   and tst #imm,r0/bt are unaffected. Cost: only when translating a cycle
   candidate, O(n) over at most 2x65 words. */
static int RECHENZYK_an() {
  static int v = -1;
  if (v < 0) {
    v = 1; /* always enabled */
  }
  return v;
}
/* Returns 0 if an unknown or reading instruction is found => do not reject the cycle. */
static int RechenZykScan(u32 b, u32 e, unsigned *vgl, unsigned *fort, unsigned *neu, int *bed)
{
  if (e < b || (e - b) > 128) return 0;
  for (u32 a = b; a <= e; a += 2) {
    const u16 op = MappedMemoryReadWord(a, NULL);
    const int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
    switch (op >> 12) {
      case 0x0:
        if (op == 0x0009 || op == 0x0008 || op == 0x0018) break;        /* nop, clrt, sett */
        if ((op & 0xF0FF) == 0x0029) { *neu |= 1u << n; break; }        /* movt Rn */
        return 0;
      case 0x2:
        if ((op & 0xF) == 0x8) { *vgl |= (1u << n) | (1u << m); break; } /* tst Rm,Rn */
        if ((op & 0xF) >= 0x9 && (op & 0xF) <= 0xB) break;              /* and/xor/or Rm,Rn */
        return 0;
      case 0x3:
        switch (op & 0xF) {
          case 0x0: case 0x2: case 0x3: case 0x6: case 0x7:
            *vgl |= (1u << n) | (1u << m); break;                         /* cmp/eq,hs,ge,hi,gt */
          case 0x8: case 0xC: *fort |= 1u << n; break;                    /* sub, add Rm,Rn */
          default: return 0;
        }
        break;
      case 0x4:
        switch (op & 0xFF) {
          case 0x10: *vgl |= 1u << n; *fort |= 1u << n; break;            /* dt Rn */
          case 0x11: case 0x15: *vgl |= 1u << n; break;                   /* cmp/pz, cmp/pl */
          case 0x00: case 0x01: case 0x04: case 0x05: case 0x08: case 0x09:
          case 0x18: case 0x19: case 0x20: case 0x21: case 0x24: case 0x25:
          case 0x28: case 0x29:
            *fort |= 1u << n; break;  /* shift/rotate instructions */
          default: return 0;
        }
        break;
      case 0x6:
        switch (op & 0xF) {
          case 0x3: *neu |= 1u << n; break;                               /* mov Rm,Rn */
          case 0xB: if (n == m) *fort |= 1u << n; else *neu |= 1u << n; break; /* neg */
          case 0xC: case 0xD: case 0xE: case 0xF:
            if (n != m) *neu |= 1u << n; break;                           /* extu/exts */
          default: return 0;  /* loads etc. */
        }
        break;
      case 0x7: if ((op & 0xFF) != 0) *fort |= 1u << n; break;           /* add #imm,Rn */
      case 0x8:
        switch (n) {
          case 0x8: *vgl |= 1u; break;                                    /* cmp/eq #imm,R0 */
          case 0x9: case 0xB: case 0xD: case 0xF: *bed = 1; break;        /* bt, bf, bt/s, bf/s */
          default: return 0;  /* mov.X @(d,Rm) etc. */
        }
        break;
      case 0x9: case 0xD: *neu |= 1u << n; break;  /* PC-relative literals */
      case 0xE: *neu |= 1u << n; break;                                   /* mov #imm,Rn */
      case 0xA: break;                                                    /* bra */
      case 0xC: if (n == 0x8) { *vgl |= 1u; break; } return 0;            /* tst #imm,R0 */
      default: return 0;
    }
  }
  return 1;
}
static bool RechenZykAb(u32 b1, u32 e1, u32 b2, u32 e2)
{
  unsigned vgl = 0, fort = 0, neu = 0; int bed = 0;
  if (!RechenZykScan(b1, e1, &vgl, &fort, &neu, &bed)) return false;
  if (b2 != 0 && !RechenZykScan(b2, e2, &vgl, &fort, &neu, &bed)) return false;
  if (!bed) return false;
  fort &= ~neu;
  if ((vgl & fort) == 0) return false;
  return true;
}
/* Trampoline switch; takes effect in ZykelSpringtNach. */
static bool Trampolin_an(void) {
  static int v = -1;
  /* always enabled */
  if (v < 0) v = 1;
  return v != 0;
}
static bool ZykelSpringtNach(u32 b_addr, u32 e_addr, u32 ziel)
{
  if (e_addr < b_addr) return false;
  for (int k = 0; k < 2; k++) {
    const u32 a = (k == 0) ? (e_addr - 2) : e_addr;
    if (a < b_addr || a > e_addr) continue;
    /* second argument = cycle counter; read exactly as CompileBlock reads the
       opcodes, so this sees what is in the block */
    const u16 op = MappedMemoryReadWord(a, NULL);
    s32 disp;
    if ((op & 0xF000) == 0xA000) {                      /* bra   disp12 */
      disp = (op & 0x800) ? (s32)((op & 0xFFF) | 0xFFFFF000) : (s32)(op & 0xFFF);
    } else if ((op & 0xFF00) == 0x8900 || (op & 0xFF00) == 0x8B00    /* bt   / bf   */
            || (op & 0xFF00) == 0x8D00 || (op & 0xFF00) == 0x8F00) { /* bt.s / bf.s */
      disp = (s32)(signed char)(op & 0x00FF);
      /* fall-through path of a non-delayed bt/bf (the block ends exactly here) leads to `bra target ; nop` */
      if (Trampolin_an() && a == e_addr && ((op & 0xFF00) == 0x8900 || (op & 0xFF00) == 0x8B00)) {
        const u16 t0 = MappedMemoryReadWord(e_addr + 2, NULL);
        const u16 t1 = MappedMemoryReadWord(e_addr + 4, NULL);
        if ((t0 & 0xF000) == 0xA000 && t1 == 0x0009) {
          const s32 td = (t0 & 0x800) ? (s32)((t0 & 0xFFF) | 0xFFFFF000) : (s32)(t0 & 0xFFF);
          if ((u32)(e_addr + 2 + 4 + (td << 1)) == ziel) {
            return true;
          }
        }
      }
    } else {
      continue;
    }
    if ((u32)(a + 4 + (disp << 1)) == ziel) return true;
  }
  return false;
}

static void Chain2Merken(u32 block_b, u32 partner_b, u32 partner_e) {
  for (unsigned i = 0; i < chain2_anzahl; i++) {
    if (chain2_tab[i].block_b == block_b) {
      chain2_tab[i].partner_b = partner_b;
      chain2_tab[i].partner_e = partner_e;
      return;
    }
  }
  if (chain2_anzahl < CHAIN2_MAX) {
    chain2_tab[chain2_anzahl].block_b   = block_b;
    chain2_tab[chain2_anzahl].partner_b = partner_b;
    chain2_tab[chain2_anzahl].partner_e = partner_e;
    chain2_anzahl++;
  }
  else INSTR(g_c2_verworfen++);
}

/* Second chance for loops whose partner block was missing at translation
   time. The detection looks the partner up in LookupTable while compiling; if
   it is not there yet, the cycle is dropped and never checked again, even if
   the partner is translated moments later. Fix: remember the candidate and
   complete it when the partner is translated. The conditions stay as strict
   as before: both blocks must be free of writes. */
#define NACHPRUEF_MAX 256
struct NachpruefEintrag { u32 kandidat_b; u32 ziel; };
static NachpruefEintrag nachpruef_tab[NACHPRUEF_MAX];
static unsigned nachpruef_anzahl = 0;

static bool NACHPRUEF_enabled() {
  static int v = -1;
  if (v < 0) {
    v = 1; /* always enabled */
  }
  return v != 0;
}

static void NachpruefVormerken(u32 kandidat_b, u32 ziel) {
  for (unsigned i = 0; i < nachpruef_anzahl; i++)
    if (nachpruef_tab[i].kandidat_b == kandidat_b && nachpruef_tab[i].ziel == ziel) return;
  if (nachpruef_anzahl < NACHPRUEF_MAX) {
    nachpruef_tab[nachpruef_anzahl].kandidat_b = kandidat_b;
    nachpruef_tab[nachpruef_anzahl].ziel       = ziel;
    nachpruef_anzahl++;
  }
  else INSTR(g_np_verworfen++);
}

static void NachpruefVergessen(u32 adr) {
  unsigned d = 0;
  for (unsigned i = 0; i < nachpruef_anzahl; i++)
    if (nachpruef_tab[i].kandidat_b != adr && nachpruef_tab[i].ziel != adr)
      nachpruef_tab[d++] = nachpruef_tab[i];
  nachpruef_anzahl = d;
}

/* True if pc lies in the partner range of block block_b. */
static inline bool Chain2ImBereich(u32 b, u32 e, u32 pc, bool einschliesslich) {
  if (pc < b) return false;
  return einschliesslich ? (pc <= e) : (pc < e);
}

static bool Chain2ImPartner(u32 block_b, u32 pc) {
  if (chain2_anzahl == 0) return false;
  /* partner_e is the address of the partner block's LAST instruction, so the
     range runs up to and including it; `<` would exclude the branch back, on
     which the PC of a wait loop rests. */
  const bool bis_einschliesslich = RANDFIX2_enabled();
  unsigned i = chain2_letzter;
  if (i < chain2_anzahl && chain2_tab[i].block_b == block_b) {
    return Chain2ImBereich(chain2_tab[i].partner_b, chain2_tab[i].partner_e,
                           pc, bis_einschliesslich);
  }
  for (i = 0; i < chain2_anzahl; i++) {
    if (chain2_tab[i].block_b == block_b) {
      chain2_letzter = i;
      return Chain2ImBereich(chain2_tab[i].partner_b, chain2_tab[i].partner_e,
                             pc, bis_einschliesslich);
    }
  }
  return false;
}


} // namespace



/* Forward declaration; the definition (DynarecSh2GetDisasmebleString(), which
   uses SH2Disasm()) follows further down in this file. */
int DynarecSh2GetDisasmebleString(string & out, u32 from, u32 to);









/* Start address (b_addr) of the last SH2 block regularly executed through the
   JIT (declared in bios.h). Maintained in Execute() right before the dynaFunc
   call, so that a later wild jump in BiosHandleFunc() can identify the game
   block it came from. Diagnostic build only. */

#if !defined(_WINDOWS)
#if defined(__arm__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{ 
    const int syscall = 0xf0002;
      __asm __volatile (
     "mov   r0, %0\n"      
     "mov   r1, %1\n"
     "mov   r7, %2\n"
     "mov     r2, #0x0\n"
     "svc     0x00000000\n"
     :
     : "r" (begin), "r" (end), "r" (syscall)
     : "r0", "r1", "r7"
    );
}
#elif defined(__aarch64__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{
    // Don't rely on GCC's __clear_cache implementation, as it caches
    // icache/dcache cache line sizes, that can vary between cores on
    // big.LITTLE architectures.
    uint64_t addr, ctr_el0;
    static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
    size_t isize, dsize;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    isize = 4 << ((ctr_el0 >> 0) & 0xf);
    dsize = 4 << ((ctr_el0 >> 16) & 0xf);

    // use the global minimum cache line size
    icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
    dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

    addr = (uint64_t)begin & ~(uint64_t)(dsize - 1);
    for (; addr < (uint64_t)end; addr += dsize)
        // use "civac" instead of "cvau", as this is the suggested workaround for
        // Cortex-A53 errata 819472, 826319, 827319 and 824069.
            __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    addr = (uint64_t)begin & ~(uint64_t)(isize - 1);
    for (; addr < (uint64_t)end; addr += isize)
            __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}
#else
void cacheflush(uintptr_t begin, uintptr_t end, int flag ){
  __builtin___clear_cache((void*)begin,(void*)end);
}
#endif
#endif

i_desc opcode_list[] =
{
  { ZERO_F,	"clrt",						0xffff, 0x8,	0, sh2_CLRT},
  { ZERO_F,   "clrmac",					0xffff, 0x28,	0, sh2_CLRMAC},
  { ZERO_F,   "div0u",					0xffff, 0x19,	0, sh2_DIV0U},
  { ZERO_F,   "nop",						0xffff, 0x9,	0, sh2_NOP},
  { ZERO_F,   "rte",						0xffff, 0x2b,	0, sh2_RTE},
  { ZERO_F,   "rts",						0xffff, 0xb,	0, sh2_RTS},
  { ZERO_F,   "sett",						0xffff, 0x18,	0, sh2_SETT},
  { ZERO_F,   "sleep",					0xffff, 0x1b,	0, sh2_SLEEP},
  { N_F,      "cmp/pl r%d",				0xf0ff, 0x4015,	0, sh2_CMP_PL},
  { N_F,      "cmp/pz r%d",				0xf0ff, 0x4011,	0, sh2_CMP_PZ},
  { N_F,      "dt r%d",					0xf0ff, 0x4010,	0, sh2_DT},
  { N_F,      "movt r%d",					0xf0ff, 0x0029,	0, sh2_MOVT},
  { N_F,      "rotl r%d",					0xf0ff, 0x4004,	0, sh2_ROTL},
  { N_F,      "rotr r%d",					0xf0ff, 0x4005,	0, sh2_ROTR},
  { N_F,      "rotcl r%d",				0xf0ff, 0x4024,	0, sh2_ROTCL},
  { N_F,      "rotcr r%d",				0xf0ff, 0x4025,	0, sh2_ROTCR},
  { N_F,      "shal r%d",					0xf0ff, 0x4020,	0, sh2_SHL},
  { N_F,      "shar r%d",					0xf0ff, 0x4021,	0, sh2_SHAR},
  { N_F,      "shll r%d",					0xf0ff, 0x4000,	0, sh2_SHL},
  { N_F,      "shlr r%d",					0xf0ff, 0x4001,	0, sh2_SHLR},
  { N_F,      "shll2 r%d",				0xf0ff, 0x4008,	0, sh2_SHLL2},
  { N_F,      "shlr2 r%d",				0xf0ff, 0x4009,	0, sh2_SHLR2},
  { N_F,      "shll8 r%d",				0xf0ff, 0x4018,	0, sh2_SHLL8},
  { N_F,      "shlr8 r%d",				0xf0ff, 0x4019,	0, sh2_SHLR8},
  { N_F,      "shll16 r%d",				0xf0ff, 0x4028,	0, sh2_SHLL16},
  { N_F,      "shlr16 r%d",				0xf0ff, 0x4029,	0, sh2_SHLL16},
  { N_F,      "stc sr, r%d",				0xf0ff, 0x0002,	0, sh2_STC_SR},
  { N_F,      "stc gbr, r%d",				0xf0ff, 0x0012,	0, sh2_STC_GBR},
  { N_F,      "stc vbr, r%d",				0xf0ff, 0x0022,	0, sh2_STC_VBR},
  { N_F,      "sts mach, r%d",			0xf0ff, 0x000a,	0, sh2_STS_MACH},
  { N_F,      "sts macl, r%d",			0xf0ff, 0x001a,	0, sh2_STS_MACL},
  { N_F,      "sts pr, r%d",				0xf0ff, 0x002a,	0, sh2_STS_PR},
  { N_F,      "tas.b @r%d",				0xf0ff, 0x401b,	0, sh2_TAS_B},
  { N_F,      "stc.l sr, @-r%d",			0xf0ff, 0x4003,	0, sh2_STC_SR_DEC},
  { N_F,      "stc.l gbr, @-r%d",			0xf0ff, 0x4013,	0, sh2_STC_GBR_DEC},
  { N_F,      "stc.l vbr, @-r%d",			0xf0ff, 0x4023,	0, sh2_STC_VBR_DEC},
  { N_F,      "sts.l mach, @-r%d",		0xf0ff, 0x4002,	0, sh2_STS_MACH_DEC},
  { N_F,      "sts.l macl, @-r%d",		0xf0ff, 0x4012,	0, sh2_STS_MACL_DEC},
  { N_F,      "sts.l pr, @-r%d",			0xf0ff, 0x4022,	0, sh2_STS_PR_DEC},
  { M_F,      "ldc r%d, sr",				0xf0ff, 0x400e,	0, sh2_LDC_SR},
  { M_F,      "ldc r%d, gbr",				0xf0ff, 0x401e,	0, sh2_LDC_GBR},
  { M_F,      "ldc r%d, vbr",				0xf0ff, 0x402e,	0, sh2_LDC_VBR},
  { M_F,      "lds r%d, mach",			0xf0ff, 0x400a,	0, sh2_LDS_MACH},
  { M_F,      "lds r%d, macl",			0xf0ff, 0x401a,	0, sh2_LDS_MACL},
  { M_F,      "lds r%d, pr",				0xf0ff, 0x402a,	0, sh2_LDS_PR},
  { M_F,      "jmp @r%d",					0xf0ff, 0x402b,	0, sh2_JMP},
  { M_F,      "jsr @r%d",					0xf0ff, 0x400b,	0, sh2_JSR}, 
  { M_F,      "ldc.l @r%d+, sr",			0xf0ff, 0x4007,	0, sh2_LDC_SR_INC},
  { M_F,      "ldc.l @r%d+, gbr",			0xf0ff, 0x4017,	0, sh2_LDC_GBR_INC},
  { M_F,      "ldc.l @r%d+, vbr",			0xf0ff, 0x4027,	0, sh2_LDC_VBR_INC},
  { M_F,      "lds.l @r%d+, mach",		0xf0ff, 0x4006,	0, sh2_LDS_MACH_INC},
  { M_F,      "lds.l @r%d+, macl",		0xf0ff, 0x4016,	0, sh2_LDS_MACL_INC},
  { M_F,      "lds.l @r%d+, pr",			0xf0ff, 0x4026,	0, sh2_LDS_PR_INC},
  { M_F,      "braf r%d",					0xf0ff, 0x0023,	0, sh2_BRAF},
  { M_F,      "bsrf r%d",					0xf0ff, 0x0003,	0, sh2_BSRF},
  { NM_F,     "add r%d, r%d",				0xf00f, 0x300c,	0, sh2_ADD},
  { NM_F,     "addc r%d, r%d",			0xf00f, 0x300e,	0, sh2_ADDC},
  { NM_F,     "addv r%d, r%d",			0xf00f, 0x300f,	0, sh2_ADDV},
  { NM_F,     "and r%d, r%d",				0xf00f, 0x2009,	0, sh2_AND},
  { NM_F,     "cmp/eq r%d, r%d",			0xf00f, 0x3000,	0, sh2_CMP_EQ},
  { NM_F,     "cmp/hs r%d, r%d",			0xf00f, 0x3002,	0, sh2_CMP_HS},
  { NM_F,     "cmp/ge r%d, r%d",			0xf00f, 0x3003,	0, sh2_CMP_GE},
  { NM_F,     "cmp/hi r%d, r%d",			0xf00f, 0x3006,	0, sh2_CMP_HI},
  { NM_F,     "cmp/gt r%d, r%d",			0xf00f, 0x3007,	0, sh2_CMP_GT},
  { NM_F,     "cmp/str r%d, r%d",			0xf00f, 0x200c,	0, sh2_CMP_STR},
  { NM_F,     "div1 r%d, r%d",			0xf00f, 0x3004,	0, sh2_DIV1},
  { NM_F,     "div0s r%d, r%d",			0xf00f, 0x2007,	0, sh2_DIV0S},
  { NM_F,     "dmuls.l r%d, r%d",			0xf00f, 0x300d,	0, sh2_DMULS_L},
  { NM_F,     "dmulu.l r%d, r%d",			0xf00f, 0x3005,	0, sh2_DMULU_L},
  { NM_F,     "exts.b r%d, r%d",			0xf00f, 0x600e,	0, sh2_EXTS_B},
  { NM_F,     "exts.w r%d, r%d",			0xf00f, 0x600f,	0, sh2_EXTS_W},
  { NM_F,     "extu.b r%d, r%d",			0xf00f, 0x600c,	0, sh2_EXTU_B},
  { NM_F,     "extu.w r%d, r%d",			0xf00f, 0x600d,	0, sh2_EXTU_W},
  { NM_F,     "mov r%d, r%d",				0xf00f, 0x6003,	0, sh2_MOVR},
  { NM_F,     "mul.l r%d, r%d",			0xf00f, 0x0007,	0, sh2_MUL_L},
  { NM_F,     "muls.w r%d, r%d",			0xf00f, 0x200f,	0, sh2_MULS},
  { NM_F,     "mulu.w r%d, r%d",			0xf00f, 0x200e,	0, sh2_MULU},
  { NM_F,     "neg r%d, r%d",				0xf00f, 0x600b,	0, sh2_NEG},
  { NM_F,     "negc r%d, r%d",			0xf00f, 0x600a,	0, sh2_NEGC},
  { NM_F,     "not r%d, r%d",				0xf00f, 0x6007,	0, sh2_NOT},
  { NM_F,     "or r%d, r%d",				0xf00f, 0x200b,	0, sh2_OR},
  { NM_F,     "sub r%d, r%d",				0xf00f, 0x3008,	0, sh2_SUB},
  { NM_F,     "subc r%d, r%d",			0xf00f, 0x300a,	0, sh2_SUBC},
  { NM_F,     "subv r%d, r%d",			0xf00f, 0x300b,	0, sh2_SUBV},
  { NM_F,     "swap.b r%d, r%d",			0xf00f, 0x6008,	0, sh2_SWAP_B},
  { NM_F,     "swap.w r%d, r%d",			0xf00f, 0x6009,	0, sh2_SWAP_W},
  { NM_F,     "tst r%d, r%d",				0xf00f, 0x2008,	0, sh2_TST},
  { NM_F,     "xor r%d, r%d",				0xf00f, 0x200a,	0, sh2_XOR},
  { NM_F,     "xtrct r%d, r%d",			0xf00f, 0x200d,	0, sh2_XTRCT},
  { NM_F,     "mov.b r%d, @r%d",			0xf00f, 0x2000,	0, sh2_MOVB},
  { NM_F,     "mov.w r%d, @r%d",			0xf00f, 0x2001,	0, sh2_MOVW},
  { NM_F,     "mov.l r%d, @r%d",			0xf00f, 0x2002,	0, sh2_MOVL},
  { NM_F,     "mov.b @r%d, r%d",			0xf00f, 0x6000,	0, sh2_MOVB_MEM},
  { NM_F,     "mov.w @r%d, r%d",			0xf00f, 0x6001,	0, sh2_MOVW_MEM},
  { NM_F,     "mov.l @r%d, r%d",			0xf00f, 0x6002,	0, sh2_MOVL_MEM},
  { NM_F,     "mac.l @r%d+, @r%d+",		0xf00f, 0x000f,	0, sh2_MAC_L},
  { NM_F,     "mac.w @r%d+, @r%d+",		0xf00f, 0x400f,	0, sh2_MAC_W},
  { NM_F,     "mov.b @r%d+, r%d",			0xf00f, 0x6004,	0, sh2_MOVB_INC},
  { NM_F,     "mov.w @r%d+, r%d",			0xf00f, 0x6005,	0, sh2_MOVW_INC},
  { NM_F,     "mov.l @r%d+, r%d",			0xf00f, 0x6006,	0, sh2_MOVL_INC},
  { NM_F,     "mov.b r%d, @-r%d",			0xf00f, 0x2004,	0, sh2_MOVB_DEC},
  { NM_F,     "mov.w r%d, @-r%d",			0xf00f, 0x2005,	0, sh2_MOVW_DEC},
  { NM_F,     "mov.l r%d, @-r%d",			0xf00f, 0x2006,	0, sh2_MOVL_DEC},
  { NM_F,     "mov.b r%d, @(r0, r%d)",	0xf00f, 0x0004,	0, sh2_MOVB_R0},
  { NM_F,     "mov.w r%d, @(r0, r%d)",	0xf00f, 0x0005,	0, sh2_MOVW_R0},
  { NM_F,     "mov.l r%d, @(r0, r%d)",	0xf00f, 0x0006,	0, sh2_MOVL_R0},
  { NM_F,     "mov.b @(r0, r%d), r%d",	0xf00f, 0x000c,	0, sh2_MOVB_R0_MEM},
  { NM_F,     "mov.w @(r0, r%d), r%d",	0xf00f, 0x000d,	0, sh2_MOVW_R0_MEM},
  { NM_F,     "mov.l @(r0, r%d), r%d",	0xf00f, 0x000e,	0, sh2_MOVL_R0_MEM},
  { MD_F,     "mov.b @(0x%03X, r%d), r0",	0xff00, 0x8400,	0, sh2_MOVB_DISP_R0},
  { MD_F,     "mov.w @(0x%03X, r%d), r0", 0xff00, 0x8500,	0, sh2_MOVW_DISP_R0},
  { ND4_F,    "mov.b r0, @(0x%03X, r%d)", 0xff00, 0x8000,	0, sh2_MOVB_R0_DISP},
  { ND4_F,    "mov.w r0, @(0x%03X, r%d)", 0xff00, 0x8100,	0, sh2_MOVW_R0_DISP},
  { NMD_F,    "mov.l r%d, @(0x%03X, r%d)",0xf000, 0x1000,	0, sh2_MOVL_DISP_MEM},
  { NMD_F,    "mov.l @(0x%03X, r%d), r%d",0xf000, 0x5000,	0, MOVLL4},
  { D_F,      "mov.b r0, @(0x%03X, gbr)",	0xff00, 0xc000,	1, sh2_MOVB_R0_GBR},
  { D_F,      "mov.w r0, @(0x%03X, gbr)",	0xff00, 0xc100,	2, sh2_MOVW_R0_GBR},
  { D_F,      "mov.l r0, @(0x%03X, gbr)",	0xff00, 0xc200,	4, sh2_MOVL_R0_GBR},
  { D_F,      "mov.b @(0x%03X, gbr), r0",	0xff00, 0xc400,	1, sh2_MOVB_GBR_R0},
  { D_F,      "mov.w @(0x%03X, gbr), r0",	0xff00, 0xc500,	2, sh2_MOVW_GBR_R0},
  { D_F,      "mov.l @(0x%03X, gbr), r0",	0xff00, 0xc600,	4, sh2_MOVL_GBR_R0},
  { D_F,      "mova @(0x%03X, pc), r0",	0xff00, 0xc700,	4, sh2_MOVA},
  { D_F,      "bf 0x%08X",				0xff00, 0x8b00,	4, sh2_BF},
  { D_F,      "bf/s 0x%08X",				0xff00, 0x8f00,	5, sh2_BF_S},
  { D_F,      "bt 0x%08X",				0xff00, 0x8900,	5, sh2_BT},
  { D_F,      "bt/s 0x%08X",				0xff00, 0x8d00,	5, sh2_BT_S},
  { D12_F,    "bra 0x%08X",				0xf000, 0xa000,	5, sh2_BRA},
  { D12_F,    "bsr 0x%08X",				0xf000, 0xb000,	0, sh2_BSR},
  { ND8_F,    "mov.w @(0x%03X, pc), r%d", 0xf000, 0x9000,	0, sh2_MOV_DISP_W},
  { ND8_F,    "mov.l @(0x%03X, pc), r%d",	0xf000, 0xd000,	2, sh2_MOV_DISP_L},
  { I_F,      "and.b #0x%02X, @(r0, gbr)",0xff00, 0xcd00,	4, sh2_AND_B},
  { I_F,      "or.b #0x%02X, @(r0, gbr)", 0xff00, 0xcf00,	0, sh2_OR_B},
  { I_F,      "tst.b #0x%02X, @(r0, gbr)",0xff00, 0xcc00,	0, sh2_TST_B},
  { I_F,      "xor.b #0x%02X, @(r0, gbr)",0xff00, 0xce00,	0, sh2_XOR_B},
  { I_F,      "and #0x%02X, r0",			0xff00, 0xc900,	0, sh2_ANDI},
  { I_F,      "cmp/eq #0x%02X, r0",		0xff00, 0x8800,	0, sh2_CMP_EQ_IMM},
  { I_F,      "or #0x%02X, r0",			0xff00, 0xcb00,	0, sh2_ORI},
  { I_F,      "tst #0x%02X, r0",			0xff00, 0xc800,	0, sh2_TST_R0},
  { I_F,      "xor #0x%02X, r0",			0xff00, 0xca00,	0, sh2_XORI},
  { I_F,      "trapa #0x%X",				0xff00, 0xc300,	0, sh2_TRAPA},
  { NI_F,     "add #0x%02X, r%d",			0xf000, 0x7000,	0, sh2_ADDI},
  { NI_F,     "mov #0x%02X, r%d",			0xf000, 0xe000,	0, sh2_MOVI},
  { 0,        NULL,						0,      0,		0, 0}
};


void DumpInstX( int i, u32 pc, u16 op  )
{
  char buf[256];
  //sprintf( buf, "%08X : ", pc );

  if (opcode_list[i].format == ZERO_F)
    sprintf( buf,"%s", opcode_list[i].mnem);
  else if (opcode_list[i].format == N_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == M_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == NM_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op >> 8) & 0xf);
  else if (opcode_list[i].format == MD_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, op & 0xf, (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == ND4_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf), (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == NMD_F)
  {
    if ((op & 0xf000) == 0x1000)
      sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op & 0xf) * 4, (op >> 8) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 4, (op >> 4) & 0xf, (op >> 8) & 0xf);
  }
  else if (opcode_list[i].format == D_F)
  {
    if (opcode_list[i].dat <= 4)
    {
      if ((op & 0xff00) == 0xc700)
      {
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4);
//				LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + PC);
      }
      else
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat);
    }
    else
    {
      if (op & 0x80)  /* sign extend */
        sprintf( buf,opcode_list[i].mnem, (((op & 0xff) + 0xffffff00) * 2) + pc + 4);
      else
        sprintf( buf,opcode_list[i].mnem, ((op & 0xff) * 2) + pc + 4);
    }        
  }
  else if (opcode_list[i].format == D12_F)
  {
    if (op & 0x800)         /* sign extend */
      sprintf( buf,opcode_list[i].mnem, ((op & 0xfff) + 0xfffff000) * 2 + pc + 4);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xfff) * 2 + pc + 4);
  }
  else if (opcode_list[i].format == ND8_F)
  {
    if ((op & 0xf000) == 0x9000)    /* .W */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + pc);
    }
    else  /* .L */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + ((pc) & 0xfffffffc));
    }
  }
  else if (opcode_list[i].format == I_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff);
  else if (opcode_list[i].format == NI_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff, (op >> 8) & 0xf);
  else
  {
    sprintf( buf,"unrecognized\n");
    return;
  }
  LOG("%08X:%04X %s\n", pc , op, buf );
  return;
}


#define opdesc(op, y, c, d)	x86op_desc(x86_##op, &op##_size, &op##_src, &op##_dest, &op##_off1, &op##_imm, &op##_off3, y, c, d)

#define opNULL			x86op_desc(0,0,0,0,0,0,0,0,0,0)

#if defined(_WINDOWS)
#define PROLOGSIZE		     27    
#define EPILOGSIZE		      3
#define SEPERATORSIZE	     10
#define SEPERATORSIZE_NORMAL 7
#define SEPERATORSIZE_DEBUG  24
#define SEPERATORSIZE_DELAY  7
#define SEPERATORSIZE_DELAY_SLOT  27
#define SEPERATORSIZE_DELAY_AFTER  10 
#define SEPERATORSIZE_DELAYD_DEBUG 34
#define DELAYJUMPSIZE	     17
#define DALAY_CLOCK_OFFSET 6
#define DALAY_CLOCK_OFFSET_DEBUG 6
#define NORMAL_CLOCK_OFFSET 6
#define NORMAL_CLOCK_OFFSET_DEBUG 3
#elif defined(AARCH64)
#define PROLOGSIZE		     (11*4)    
#define SEPERATORSIZE_NORMAL (2*4)
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  (18*4)
#define SEPERATORSIZE_DELAY_SLOT  (15*4)
#define SEPERATORSIZE_DELAY_AFTER  (12*4)
#define DALAY_CLOCK_OFFSET (2*4)
#define DALAY_CLOCK_OFFSET_DEBUG (10*4)
#define SEPERATORSIZE_DELAYD_DEBUG (20*4)
#define EPILOGSIZE		      (10*4)
#define DELAYJUMPSIZE	     (11*4)
#else // ARMv7
#define PROLOGSIZE		     16    
#define SEPERATORSIZE_NORMAL 8
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  36
#define SEPERATORSIZE_DELAY_SLOT  36
#define SEPERATORSIZE_DELAY_AFTER  20
#define DALAY_CLOCK_OFFSET 8
#define DALAY_CLOCK_OFFSET_DEBUG 8
#define SEPERATORSIZE_DELAYD_DEBUG 60
#define EPILOGSIZE		      12
#define DELAYJUMPSIZE	     32
#endif


#define MININSTRSIZE    3
#define MAXINSTRSIZE	416
#define MAXJUMPSIZE		46

#define SAFEPAGESIZE	MAXBLOCKSIZE - MAXINSTRSIZE - SEPERATORSIZE_DELAY_SLOT - SEPERATORSIZE_DELAY_AFTER - SEPERATORSIZE_NORMAL - EPILOGSIZE
#define MAXINSTRCNT		MAXBLOCKSIZE/(MININSTRSIZE+SEPERATORSIZE_NORMAL)

#define opinit(x)	extern const unsigned short x##_size; \
                    extern const unsigned char x##_src, x##_dest, x##_off1, x##_imm, x##_off3; \
            void x86_##x();


extern "C" {

opinit(CLRT);
opinit(CLRMAC);
opinit(NOP);
opinit(DIV0U);
opinit(SETT);
opinit(SLEEP);
opinit(STS_MACH);
opinit(STS_MACL);
opinit(STS_MACH_DEC);
opinit(STS_MACL_DEC);
opinit(STS_PR);
opinit(STC_SR);
opinit(STC_VBR);
opinit(STC_GBR);
opinit(LDS_PR);
opinit(LDS_MACH);
opinit(LDS_MACL);
opinit(LDS_PR_INC);
opinit(LDS_MACH_INC);
opinit(LDS_MACL_INC);
opinit(LDC_SR);
opinit(LDC_VBR);
opinit(LDCGBR);
opinit(LDC_SR_INC);
opinit(LDC_VBR_INC);
opinit(LDC_GBR_INC);
opinit(STC_SR_MEM);
opinit(STC_VBR_MEM);
opinit(STC_GBR_MEM);
opinit(STSMPR);
opinit(CMP_EQ);
opinit(CMP_HI);
opinit(CMP_GE);
opinit(CMP_HS);
opinit(CMP_GT);
opinit(DT);
opinit(CMP_PL);
opinit(CMP_PZ);
opinit(ROTL);
opinit(ROTR);
opinit(ROTCL);
opinit(ROTCR);
opinit(SHL);
opinit(SHLR);
opinit(SHAR);
opinit(SHLL2);
opinit(SHLR2);
opinit(SHLL8);
opinit(SHLR8);
opinit(SHLL16);
opinit(SHLR16);
opinit(AND);
opinit(OR);
opinit(XOR);
opinit(NOT);
opinit(XTRCT);
opinit(ADD);
opinit(ADDC);
opinit(SUB);
opinit(SUBC);
opinit(NEG);
opinit(NEGC);
opinit(TST);
opinit(TSTI);
opinit(AND_B);
opinit(OR_B);
opinit(TST_B);
opinit(XOR_B);
opinit(ADDI);
opinit(ANDI);
opinit(ORI);
opinit(XORI);
opinit(MOVI);
opinit(CMP_EQ_IMM);
opinit(SWAP_B);
opinit(SWAP_W);
opinit(EXTUB);
opinit(EXTU_W);
opinit(EXTS_B);
opinit(EXTS_W);
opinit(BT)
opinit(BF);
opinit(BF_S);
opinit(JMP);
opinit(JSR);
opinit(BRA);
opinit(BSR);
opinit(BSRF);
opinit(BRAF);
opinit(RTE);
opinit(RTS);
opinit(MOVA);
opinit(MOVT);
opinit(MOVBL);
opinit(MOVWL);
opinit(MOVL_MEM_REG);
opinit(MOVBS);
opinit(MOVWS);
opinit(MOVLS);
opinit(MOVWI);
opinit(MOVLI);
opinit(MOVBS4);
opinit(MOVWS4);
opinit(MOVLS4);
opinit(MOVR);
opinit(MOVBP);
opinit(MOVWP);
opinit(MOVLP);
opinit(MOVBL0);
opinit(MOVWL0);
opinit(MOVLL0);
opinit(MOVBS0);
opinit(MOVWS0);
opinit(MOVLS0);
opinit(MOVBSG);
opinit(MOVLSG);
opinit(MOVWL4);  // 0x8500
opinit(MOVLL4);
opinit(MOVLM);
opinit(TAS);
opinit(ADDV);
opinit(CMPSTR);
opinit(DIV0S);
opinit(DMULS);
opinit(DMULU);
opinit(MULL);
opinit(MULS);
opinit(MULU);
opinit(SUBV);
opinit(MAC_L);
opinit(MOVBM);
opinit(MOVWM);
opinit(MOVBL4);
opinit(MOVWSG);
opinit(MOVBLG);
opinit(MOVWLG);
opinit(MOVLLG);
opinit(TRAPA);
opinit(DIV1);
opinit(MAC_W);


void prologue(void);
void epilogue(void);
void seperator(void);
void seperator_normal(void);
void seperator_delay(void);
void seperator_delay_slot(void);
void seperator_delay_after(void);

void seperator_d_normal(void);
void seperator_d_delay(void);

void PageJump(void); // jumps to a different page
void PageFlip(void); // "flips" the page
/* Block chaining in assembler: at translation time the exit templates
   epilogue_k/PageFlip_k (same size as epilogue/PageFlip) are used. They jump
   into kette_stub, which enters the next HWRAM block directly when C would do
   nothing else (no BLOCK_LOOP, no acceptable interrupt, time slice not full,
   target entry present); otherwise the old epilogue returns to C. Only helps
   SH2-bound titles. Always off in the diagnostic build, where per-block
   counters run in C. */
/* Chaining stage: 0 off, 1 = block exits via epilogue_k/PageFlip_k, 2 = the
   delayed branch exits (seperator_delay_slot_k/_after_k) also go through
   kette_stub. */
static int KetteStufe(void) {
  return 2;   /* fixed: chaining stage 2 */
}
static int KetteAn(void) { return KetteStufe() >= 1; }

#if defined(AARCH64)
void internal_jmp(void);
void internal_delay_jmp(void);
#else
void internal_jmp(void){ printf("nope"); }
void internal_delay_jmp(void){ printf("nope"); }
#endif
int internal_jmp_size = 18*4;
int internal_delay_jmp_size = 15*4;
const intptr_t internal_jmp_to_offset = 4*4;
const intptr_t internal_delay_jmp_to_offset = 4*4;
 
extern x86op_desc asm_list[];

}

/* Hot counters: DZ(x) is INSTR(x) (instrument.h) and compiles away unless
   YABA_INSTRUMENT is set; each counter is cache-line aligned so that master
   and slave, running on different cores, do not share a line (false sharing). */
  #define DZ(x) INSTR(x)

extern "C" { alignas(64) unsigned int g_blk_eintritte = 0;

/* Why does a block end? Three counters, updated at translation time (rare
   after warm-up): branch instruction, LDC SR, or MAXBLOCKSIZE reached. */
unsigned long long g_abbr_branch = 0;   /* block ended by a branch instruction */
unsigned long long g_abbr_ldcsr  = 0;   /* LDC SR / LDC.L SR                    */
unsigned long long g_abbr_size   = 0;   /* MAXBLOCKSIZE reached                 */
}  /* closes the extern "C" block opened at g_blk_eintritte */
/* Per-CPU split of the block entry counter. g_blk_eintritte stays the sum
   (blkm + blks == blk). */
extern "C" { alignas(64) unsigned int g_blk_m = 0; }
extern "C" { alignas(64) unsigned int g_blk_s = 0; }  /* own cache line per CPU */

/* Why does the CHAIN2 catch not apply? Master counters: g_c2zs_m (the
   ZYKSLAVE restriction blocks it), g_c2np_m (Chain2ImPartner says no),
   g_c2noadj_m (no BLOCK_LOOP_ADJ), with g_c2kand_m (candidates) and g_c2tr_m
   (hits) as references. Master only, since the ZYKSLAVE restriction only
   applies there. Must have C linkage. */
extern "C" {
  alignas(64) unsigned int g_c2kand_m  = 0;  /* master group, own cache line */
  unsigned int g_c2zs_m    = 0;
  unsigned int g_c2np_m    = 0;
  unsigned int g_c2tr_m    = 0;
  unsigned int g_c2noadj_m = 0;
}
/* The same counters for the slave. g_c2zs_s must stay 0, because the ZYKSLAVE
   restriction requires !is_slave_. */
extern "C" {
  alignas(64) unsigned int g_c2kand_s  = 0;  /* slave group, own cache line */
  unsigned int g_c2np_s    = 0;
  unsigned int g_c2tr_s    = 0;
  unsigned int g_c2noadj_s = 0;
}

x86op_desc asm_list[] =
{
  opdesc(CLRT,0,1,0),       
  opdesc(CLRMAC,0,1,0),
  opdesc(DIV0U,0,1,0),
  opdesc(NOP,0,1,0),
  opdesc(RTE,4,4,0),
  opdesc(RTS,4,2,0),
  opdesc(SETT,0,1,0),
  opdesc(SLEEP,0,8,0),
  opdesc(CMP_PL,0,1,0),
  opdesc(CMP_PZ,0,1,0),
  opdesc(DT,0,1,1),
  opdesc(MOVT,0,1,0),
  opdesc(ROTL,0,1,0),
  opdesc(ROTR,0,1,0),
  opdesc(ROTCL,0,1,0),
  opdesc(ROTCR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHAR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHLR,0,1,0),
  opdesc(SHLL2,0,1,0),
  opdesc(SHLR2,0,1,0),
  opdesc(SHLL8,0,1,0),
  opdesc(SHLR8,0,1,0),
  opdesc(SHLL16,0,1,0),
  opdesc(SHLR16,0,1,0),
  opdesc(STC_SR,0xFF,1,1),
  opdesc(STC_GBR,0xFF,1,1),
  opdesc(STC_VBR,0xFF,1,1),
  opdesc(STS_MACH,0xFF,1,0),
  opdesc(STS_MACL,0xFF,1,0),
  opdesc(STS_PR,0xFF,1,0),
  opdesc(TAS,0,4,1),         // 0x401b
  opdesc(STC_SR_MEM,0xFF,2,1),
  opdesc(STC_GBR_MEM,0xFF,2,1),
  opdesc(STC_VBR_MEM,0xFF,2,1),
  opdesc(STS_MACH_DEC,0xFF,2,1),
  opdesc(STS_MACL_DEC,0xFF,2,1),
  opdesc(STSMPR,0xFF,1,1),     // 0x4022
  opdesc(LDC_SR,0xFF,1,0),
  opdesc(LDCGBR,0xFF,1,0),
  opdesc(LDC_VBR,0xFF,1,0),
  opdesc(LDS_MACH,0xFF,1,0),
  opdesc(LDS_MACL,0xFF,1,0),
  opdesc(LDS_PR,0xFF,1,0),
  opdesc(JMP, 3,2,0),
  opdesc(JSR, 4,2,0),
  opdesc(LDC_SR_INC,0xFF,3,0),  
  opdesc(LDC_GBR_INC,0xFF,3,0),
  opdesc(LDC_VBR_INC,0xFF,3,0),
  opdesc(LDS_MACH_INC,0xFF,1,0),
  opdesc(LDS_MACL_INC,0xFF,1,0),
  opdesc(LDS_PR_INC,0xFF,1,0),
  opdesc(BRAF,4,2,0),
  opdesc(BSRF,4,2,0),
  opdesc(ADD,0,1,1),
  opdesc(ADDC,0,1,1),
  opdesc(ADDV,0,1,1),  // 0x300F
  opdesc(AND,0,1,0),
  opdesc(CMP_EQ,0,1,0),
  opdesc(CMP_HS,0,1,0),
  opdesc(CMP_GE,0,1,0),
  opdesc(CMP_HI,0,1,0),
  opdesc(CMP_GT,0,1,0),
  opdesc(CMPSTR,0,1,0), // 0x200C
  opdesc(DIV1,0,1,0),   // 0x3004
  opdesc(DIV0S,0,1,0),  // 0x2007
  opdesc(DMULS,0,4,0),  // 0x300D
  opdesc(DMULU,0,4,0),  // 0x3005
  opdesc(EXTS_B,0,1,0),
  opdesc(EXTS_W,0,1,0),
  opdesc(EXTUB,0,1,0),
  opdesc(EXTU_W,0,1,0),
  opdesc(MOVR,0,1,0),  // 0x6003
  opdesc(MULL,0,4,0),  // 0x0007
  opdesc(MULS,0,3,0),  // 0x200f
  opdesc(MULU,0,3,0),  // 0x200e
  opdesc(NEG,0,1,0),
  opdesc(NEGC,0,1,0),
  opdesc(NOT,0,1,0),
  opdesc(OR,0,1,0),
  opdesc(SUB,0,1,0),
  opdesc(SUBC,0,1,1),
  opdesc(SUBV,0,1,1),  // 0x300B
  opdesc(SWAP_B,0,1,0),
  opdesc(SWAP_W,0,1,0),
  opdesc(TST,0,1,0),
  opdesc(XOR,0,1,0),
  opdesc(XTRCT,0,1,0),
  opdesc(MOVBS,0,1,1),
  opdesc(MOVWS,0,1,1),
  opdesc(MOVLS,0,1,1),
  opdesc(MOVBL,0,1,0), // 6000
  opdesc(MOVWL,0,1,0), // 6001
  opdesc(MOVL_MEM_REG,0,1,0),
  opdesc(MAC_L,0,3,0),  // 0x000f
  opdesc(MAC_W,0,3,0),  // 0x400f
  opdesc(MOVBP,0,1,0),  // 6004
  opdesc(MOVWP,0,1,0),  // 6005
  opdesc(MOVLP,0,1,1),  // 6006
  opdesc(MOVBM,0,1,1),  // 0x2004,
  opdesc(MOVWM,0,1, 0),  // 0x2005,
  opdesc(MOVLM,0,1,1),  // 0x2006
  opdesc(MOVBS0,0,1,1), // 0x0004
  opdesc(MOVWS0,0,1,1), // 0x0005
  opdesc(MOVLS0,0,1,1), // 0x0006
  opdesc(MOVBL0,0,1, 0), // 0x000C
  opdesc(MOVWL0,0,1, 0), // 0x000D 
  opdesc(MOVLL0,0,1, 0), // 0x000E
  opdesc(MOVBL4,0,1,1), // 0x8400
  opdesc(MOVWL4,0,1, 0), // 0x8500
  opdesc(MOVBS4,0,1, 0), // 0x8000
  opdesc(MOVWS4,0,1,1), // 0x8100
  opdesc(MOVLS4,0,1,1), // 0x1000
  opdesc(MOVLL4,0,1, 0), // 0x5000 ,1
  opdesc(MOVBSG,0,1,1), // 0xC000
  opdesc(MOVWSG,0,1,1), // 0xc100
  opdesc(MOVLSG,0,1,1), // 0xC200
  opdesc(MOVBLG,0,1, 0), // 0xC400
  opdesc(MOVWLG,0,1, 0), // 0xC500
  opdesc(MOVLLG,0,1, 0), // 0xC600
  opdesc(MOVA,0,1, 0),
  opdesc(BF,1,3, 0),
  opdesc(BF_S,3,2, 0),
  opdesc(BT,1,3, 0),
  opdesc(BT,3,2, 0),
  opdesc(BRA,2,2, 0),
  opdesc(BSR,2,2, 0),
  opdesc(MOVWI,0,1, 0),
  opdesc(MOVLI,0, 1, 0),
  opdesc(AND_B,0,3,1),
  opdesc(OR_B,0,3,1),
  opdesc(TST_B,0, 1, 0),
  opdesc(XOR_B,0,3,1),
  opdesc(ANDI,0,1, 0),
  opdesc(CMP_EQ_IMM,0,1, 0),
  opdesc(ORI,0,1, 0),
  opdesc(TSTI,0,1, 0),  // C800
  opdesc(XORI,0,1, 0),
  opdesc(TRAPA,5,8,1), // 0xc300
  opdesc(ADDI,0,1, 1),
  opdesc(MOVI,0,1, 0),
  opNULL
}; 

/* Rarely used part of setDirty (see DynarecSh2.h), kept out of line: it only
   runs when the address actually has a parent entry. The common case, no
   entry, leaves setDirty inline before this. */
__attribute__((noinline))
void CompileBlocks::setDirtySlow(u32 addr) {
  bool diag_ = DegenDiagOn(); /* read once per call */
    for (auto it = LookupParentTable[addr].begin(); it != LookupParentTable[addr].end(); it++) {
      if (LookupTable[*it] != NULL) {
        for (u32 i = adress_mask(LookupTable[*it]->b_addr) ; i <= adress_mask(LookupTable[*it]->e_addr); i++ ) {
          if (i != addr) {
            LookupParentTable[i].remove(*it);
            PraesenzNachRemove(i);
            /* counts calls, not removed elements (list::remove() returns void
               before C++20); a proxy for the parent table cleanup rate */
            if (diag_) diag_parent_removed_++;
          }
        }
         LOG("%d %08X is removed", LookupTable[*it]->id, (*it) << 1);
        remove_count_++;
        if (diag_) diag_invalidations_++;
        self_modify_block[ (((*it) << 1) | 0x06000000) ] = LookupTable[*it]->id;
        LookupTable[*it] = NULL;
      }
    }
  LookupParentTable[addr].clear();
  PraesenzLoeschen(addr);
}

void CompileBlocks::Init()
{
  memset((void*)dCode, 0, sizeof(Block)*NUMOFBLOCKS);
  memset(LookupTable, 0, sizeof(LookupTable));
  memset(LookupTableRom, 0, sizeof(LookupTableRom));
  memset(LookupTableLow, 0, sizeof(LookupTableLow));
  memset(LookupTableC, 0, sizeof(LookupTableC));
  blockCount = 0;
  LastMakeBlock = 0;
  g_CompleBlock = dCode;
  for (int i = 0; i < NUMOFBLOCKS; i++ ) {
    g_CompleBlock[i].id = i;
  }
  for (int i = 0; i < (0x100000>>1) ; i++) {
    LookupParentTable[i].clear();
    PraesenzLoeschen(i);
  }
  return;
}

int CompileBlocks::opcodeIndex(u16 op)
{
  /*register*/ int i = 0;
  while (((op & opcode_list[i].mask) != opcode_list[i].bits) && opcode_list[i].mnem != 0) i++;
  return i;
}

void CompileBlocks::FindOpCode(u16 opcode, u8 * instindex)
{
  *instindex = opcodeIndex(opcode);
  return;
}

void CompileBlocks::BuildInstructionList()
{
  u32 optest;
  memset(dsh2_instructions, 0, sizeof(u8)*MAX_INSTSIZE);
  for (optest = 0; optest <= 0xFFFF; optest++) {
    FindOpCode(optest, &dsh2_instructions[optest]);
  }
}


Block * CompileBlocks::CompileBlock(u32 pc, addrs * ParentT = NULL, bool is_slave)
{
  bool uebhash_reuse_ = false;
  compile_count_++;
  /* read once per compile */
  bool diag_ = DegenDiagOn();
  if (diag_) diag_compiles_total_++;

  auto block_index = self_modify_block.find(pc);
  if( block_index != self_modify_block.end()  ){
      blockCount = block_index->second ;
      self_modify_block.erase(pc);
      uebhash_reuse_ = true;
      LOG( "hit! last_modified_block = %d, last_modified_pc = %08X",blockCount, pc );
      if (diag_) diag_compiles_reuse_++;
  }else{
    blockCount = LastMakeBlock;
    LastMakeBlock++;
    LastMakeBlock &= (NUMOFBLOCKS-1);
    if (diag_) diag_compiles_new_++;
  }

  /* On the self-modifying path blockCount is the id of the same block, so
     b_addr == pc, and setDirtySlow() has just cleared its entries; a cleanup
     here would remove nothing. */
  if (g_CompleBlock[blockCount].b_addr != 0x00 &&
      g_CompleBlock[blockCount].b_addr != pc) {
    if (diag_) diag_evictions_++;
    /* pending candidates for this block expire with it */
    if (NACHPRUEF_enabled()) {
      NachpruefVergessen(g_CompleBlock[blockCount].b_addr);
      /* chain2_tab entries could point into an address range that now holds
         different code */
      Chain2VergessenPartner(g_CompleBlock[blockCount].b_addr);
    }

    if ((g_CompleBlock[blockCount].b_addr & 0xFF000000) == 0xC0000000) {
      LookupTableC[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
    }
    else {
      switch (g_CompleBlock[blockCount].b_addr & 0x0FF00000) {
      case 0x00000000:
        if (yabsys.emulatebios) {
          //return NULL; do nothing
        }
        else {
          LookupTableRom[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        }
        break;
      case 0x00200000:
        LookupTableLow[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        break;
      case 0x06000000:
        /*case 0x06100000:*/
        /* Before unlinking the evicted block from LookupTable, remove its whole
           address range [b_addr..e_addr] from all affected LookupParentTable[]
           lists, not only the entry at the exact old b_addr. blockCount is also
           the stable block id. */
        {
          u32 old_b = adress_mask(g_CompleBlock[blockCount].b_addr);
          u32 old_e = adress_mask(g_CompleBlock[blockCount].e_addr);
          for (u32 i = old_b; i <= old_e; i++) {
            /* The lists in LookupParentTable hold word addresses, not ring
               indices, so the entry is removed by address. */
            LookupParentTable[i].remove(old_b);
            PraesenzNachRemove(i);
            if (diag_) diag_parent_removed_++;
          }
        }
        LookupTable[(g_CompleBlock[blockCount].b_addr & 0x000FFFFF) >> 1] = NULL;
        //LOG("%d, %08X is removed due to overflow", blockCount, g_CompleBlock[blockCount].b_addr);
        break;
      default:
        break;
      }
    }
  }

  g_CompleBlock[blockCount].b_addr = pc;


  //LOG("%d,%08X is compiled",blockCount,pc );
  if (EmmitCode(&g_CompleBlock[blockCount], ParentT, is_slave) != 0) {
    return NULL;
  }
  (void)uebhash_reuse_;

  return &g_CompleBlock[blockCount];
}

void CompileBlocks::ShowStatics() {
  //LOG("Compile\t%d\t%d\t%d\n", compile_count_, exec_count_, remove_count_);
  compile_count_ = 0;
  exec_count_ = 0;
  remove_count_ = 0;
}

// memo DirectMemoryAccess
// MOVLI,MOVWI



void CompileBlocks::opcodePass(x86op_desc *op, u16 opcode, u8 *ptr)
{
#if defined(AARCH64)
  // multiply source and dest regions by 4 (size of register) 

  if (*(op->src) != 0xFF) // C
  {
    *(u32*)(ptr + *(op->src)) |= ((u32)((opcode >> 4) & 0xf) << (2+5));
  }

  if (*(op->dest) != 0xFF) // B
  {
    *(u32*)(ptr + *(op->dest)) |= ((u32)((opcode >> 8) & 0xf) << (2+5)) ;
  }

  if (*(op->off1) != 0xFF){
    *(u32*)(ptr + *(op->off1)) |= ((u32)(opcode & 0xf) << 5) ;
  }

  if (*(op->imm) != 0xFF){
    *(u32*)(ptr + *(op->imm)) |= ((u32)opcode & 0xff)<<5;
  }

  if (*(op->off3) != 0xFF) {
    *(u32*)(ptr + *(op->off3)) |= ((u32)opcode & 0xfff) << 5;
  }

#else

  if (*(op->src) != 0xFF) // C
    *(ptr + *(op->src)) = (u8)(((opcode >> 4) & 0xf) << 2);

  if (*(op->dest) != 0xFF) // B
    *(ptr + *(op->dest)) = (u8)(((opcode >> 8) & 0xf) << 2);

  if (*(op->off1) != 0xFF)
    *(ptr + *(op->off1)) = (u8)(opcode & 0xf);

  if (*(op->imm) != 0xFF)
    *(ptr + *(op->imm)) = (u8)(opcode & 0xff);

#if _WINDOWS
  if (*(op->off3) != 0xFF)
    *(u16*)(ptr + *(op->off3)) = (u16)(opcode & 0xfff);
#else  
  if (*(op->off3) != 0xFF) {
    *(ptr + *(op->off3)) = (u8)((opcode >> 8) & 0x0f);
    *(ptr + *(op->off3) + 4) = (u8)(opcode & 0xff);
}
#endif  

#endif
}

#define Y_MAX(a, b) ((a) > (b) ? (a) : (b))
#define Y_MIN(a, b) ((a) < (b) ? (a) : (b))

/* Self-modifying code inside the running block: a game may patch the
   immediate of an instruction in the same block it is running in (the store
   and the patched instruction have no branch in between). Immediates are baked
   in at translation time and a store only invalidates the next lookup, so the
   running block would use the stale value.
   Fix: while translating, track which registers hold a known address. If a
   store targets the block's own code (>= block start, < instruction + 0x200)
   the block ends right after the store, and the following block is translated
   from the patched memory. No cost in the generated code; a false alarm only
   ends a block earlier. */
static int SmcEndeAn(void) {
  return 1;   /* fixed: always on */
}
static int SleepEndeAn(void) {
  return 1;   /* fixed: always on */
}
static inline void SmcZielPruefen(u32 t, u32 ia, u32 start, bool &ende) {
  const u32 t2 = t & 0xDFFFFFFFu, s2 = start & 0xDFFFFFFFu, i2 = ia & 0xDFFFFFFFu;
  if (t2 >= s2 && t2 < i2 + 0x200u) ende = true;
}
/* Tracks known register values of a block (translation time only). ia = address of the instruction. */
static inline void SmcVerfolge(u16 op, u32 ia, u32 start, u32 *w, u32 &bek, bool &ende, u32 &ziel) {
  const int n = (op >> 8) & 15, m = (op >> 4) & 15;
  const u32 bn = 1u << n, bm = 1u << m;
  switch (op >> 12) {
  case 0xD: w[n] = MappedMemoryReadLong(((ia + 4) & ~3u) + (u32)(op & 0xFF) * 4, NULL); bek |= bn; return;
  case 0xE: w[n] = (u32)(s32)(s8)(op & 0xFF); bek |= bn; return;
  case 0x7: if (bek & bn) w[n] += (u32)(s32)(s8)(op & 0xFF); return;
  case 0x6:
    if ((op & 15) == 3) { if (bek & bm) { w[n] = w[m]; bek |= bn; } else bek &= ~bn; return; }
    bek &= ~(bn | bm); return;  /* load/arithmetic instructions; @Rm+ changes Rm */
  case 0x2:
    if ((op & 15) <= 2) { if (bek & bn) { SmcZielPruefen(w[n], ia, start, ende); if (ende) ziel = w[n]; } return; }
    bek &= ~bn; return;  /* @-Rn, and/or/xor/xtrct change Rn */
  case 0x1:                                                     /* mov.l Rm,@(disp,Rn) */
    if (bek & bn) { const u32 t = w[n] + (u32)(op & 15) * 4; SmcZielPruefen(t, ia, start, ende); if (ende) ziel = t; }
    return;
  case 0x8: {
    const int sub = (op >> 8) & 15, rn = (op >> 4) & 15;
    if (sub == 0 || sub == 1) {                                 /* mov.b/w R0,@(disp,Rn) */
      if (bek & (1u << rn)) { const u32 t = w[rn] + (u32)(op & 15) * (sub + 1); SmcZielPruefen(t, ia, start, ende); if (ende) ziel = t; }
    } else if (sub == 4 || sub == 5) bek &= ~1u;                /* mov.b/w @(disp,Rn),R0 */
    return; }
  case 0x3: case 0x4: case 0x5: case 0x9: bek &= ~bn; return;
  case 0xC: bek &= ~1u; return;
  default: bek = 0; return;  /* anything else: nothing is known any more */
  }
}

int CompileBlocks::EmmitCode(Block *page, addrs * ParentT, bool is_slave )
{
  int i, j, jmp = 0, count = 0;
  u16 op, temp;
  u32 addr = page->b_addr;
  u32 start_addr = page->b_addr;
  u8 *ptr, *startptr;
  u32 instruction_counter = 0;
  u32 write_memory_counter = 0;
  /* tracking of stores into the block's own code, see SmcVerfolge */
  const int smc_an = SmcEndeAn();
  u32 smc_wert[16]; u32 smc_bekannt = 0; bool smc_ende = false; u32 smc_ziel = 0;
  u32 calsize;
  /* The address map is static and reused: clear() keeps the buckets, so
     nothing is allocated per translation. CompileBlock only runs on the
     emulation thread. */
  static std::unordered_map<u32, uintptr_t> addr_map;
  addr_map.clear();

  startptr = ptr = page->code;

  i = 0;
  j = 0;
  count = 0;  
  /* The code buffer is not cleared before translation: only the emitted part
     is executed and cache-flushed, and a block ends with the epilogue. */
  memcpy((void*)ptr, (void*)prologue, PROLOGSIZE);
  ptr += PROLOGSIZE;
  int MaxSize = 0;

  void * nomal_seperator;
  u32 nomal_seperator_size;
  u8 nomal_seperator_counter_offset;
  void * delay_seperator;
  u32 delay_seperator_size;
  u8 delayslot_seperator_counter_offset;

  if (debug_mode_) {
    nomal_seperator = (void*)seperator_d_normal;
    nomal_seperator_size = SEPERATORSIZE_DEBUG;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET_DEBUG;
    delay_seperator = (void*)seperator_d_delay;
    delay_seperator_size = SEPERATORSIZE_DELAYD_DEBUG;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET_DEBUG;
  }
  else {
    nomal_seperator = (void*)seperator_normal;
    nomal_seperator_size = SEPERATORSIZE_NORMAL;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET;
    delay_seperator = (KetteStufe() >= 2) ? (void*)seperator_delay_slot_k : (void*)seperator_delay_slot;
    delay_seperator_size = SEPERATORSIZE_DELAY_SLOT;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET;
  }
  
  page->flags = 0;

#ifdef BUILD_INFO  
  if( show_code_ ) LOG("*********** start block %08X *************\n", addr );
#endif
  //LOG("Compile %08X\n", addr );
  //MaxSize = MAXBLOCKSIZE - MAXINSTRSIZE- delay_seperator_size - SEPERATORSIZE_DELAY_AFTER - nomal_seperator_size - EPILOGSIZE;
  //while (ptr - startptr < MaxSize) {
  while (1) {
    // translate the opcode and insert code
    op = MappedMemoryReadWord(addr, NULL);
#ifdef SET_DIRTY
    if (ParentT) {
      /* Check whether the entry is already present instead of using
         push_back()+unique() on an unsorted list: unique() only removes
         adjacent duplicates, so a start_addr pushed again later (typical after
         re-translation following ring eviction) would stay in the list. */
      u32 keepaddr = adress_mask(addr);
      u32 parent_val = adress_mask(start_addr);
      bool already_present = false;
      for (auto pit = ParentT[keepaddr].begin(); pit != ParentT[keepaddr].end(); ++pit) {
        if (*pit == parent_val) { already_present = true; break; }
      }
      if (!already_present) {
        ParentT[keepaddr].push_back(parent_val);
        PraesenzSetzen(keepaddr);
        if (DegenDiagOn()) diag_parent_pushes_++;
      }
    }
#endif

    addr_map[addr] = (uintptr_t)ptr;

    i = dsh2_instructions[op];
    if (asm_list[i].func == 0) {
      LOG("bad instruction code @ PC=%08X code=%04X\n", addr, op );
      return -1;  // bad instruction code
    }

    // CheckSize
    u8 delay = asm_list[i].delay;
#if defined(AARCH64)
    if ( delay == 0 || delay == 0xFF) {
      calsize  = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + Y_MAX(internal_jmp_size,DELAYJUMPSIZE) + EPILOGSIZE;
    } else {
      u32 op2 = memGetWord(addr+2);
      u32 delayop = dsh2_instructions[op2];
      calsize = (ptr - startptr) + *asm_list[i].size + *asm_list[delayop].size + 
      delay_seperator_size + Y_MAX(internal_delay_jmp_size,SEPERATORSIZE_DELAY_AFTER) + EPILOGSIZE;
    }
#else    
    if ( delay == 0 || delay == 0xFF) {
      calsize  = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = (ptr - startptr) + *asm_list[i].size + nomal_seperator_size + DELAYJUMPSIZE + EPILOGSIZE;
    } else {
      u32 op2 = MappedMemoryReadWord(addr+2,NULL);
      u32 delayop = dsh2_instructions[op2];
      calsize = (ptr - startptr) + *asm_list[i].size + *asm_list[delayop].size + delay_seperator_size + SEPERATORSIZE_DELAY_AFTER + EPILOGSIZE;
    }
#endif
    if (calsize >= MAXBLOCKSIZE) {
      INSTR(g_abbr_size++);
      break; // no space is available
    }

    if (0x1b == op) { // SLEEP
      page->flags |= BLOCK_LOOP;
    }

    // Inifinity Loop Detection
    if (count == 0 && (op & 0xF00F) == 0x6000) { // mov ? R0
      u32 loopcheck = memGetLong(addr + 2);
      if ((loopcheck & 0xFF00FFFF) == 0xC80089FC) { // test, bf
        page->flags |= BLOCK_LOOP;
      }
      if( (loopcheck&0xF00FFFFF) == 0x20088DFC ){ // slave waits intrrupt capture
          page->flags |= BLOCK_LOOP;
      }
    }

//#ifdef BUILD_INFO  
//    LOG("compiling %08X, 0x%04X @ 0x%08X\n", startptr, op, addr);
//#endif    

    addr += 2;

#ifdef BUILD_INFO
    if(show_code_) DumpInstX( i, addr-2, op  );
#endif

    instruction_counter++;
    asm_list[i].build_count++;
    write_memory_counter += OPFIX_write(op, asm_list[i].write_count);
    if (smc_an) SmcVerfolge(op, addr - 2, start_addr, smc_wert, smc_bekannt, smc_ende, smc_ziel);

    u32 jumppc = 0xFFFFFFFF;
    uintptr_t jumpptr = 0xFFFFFFFF;
    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {
      if (asm_list[i].delay == 1) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
      else if (asm_list[i].delay == 2) {
        temp = (op & 0xfff) << 1;
        if (temp & 0x1000)
          temp |= 0xfffff000;
        if (BRAFIX_enabled()) {
          /* The sign extension above has no effect because temp is only 16 bits
             wide (the same variable then receives the delay-slot instruction),
             so a separate 32-bit value is used here. */
          s32 bra_disp = (op & 0x800) ? (s32)((op & 0xfff) | 0xfffff000)
                                      : (s32)(op & 0xfff);
          jumppc = addr + (u32)(bra_disp << 1) + 2;
        } else {
          jumppc = addr + ((signed)(op & 0xfff) << 1) + 2;
        }
      }
      else if (asm_list[i].delay == 3) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
#if defined(AARCH64)
      if ( addr_map.count(jumppc) != 0 && write_memory_counter != 0 ) {
        jumpptr = addr_map[jumppc];
        //LOG("jumpptr = %lx\n", jumpptr );
      }
#endif      
    }


    // Regular Opcode ( No Delay Branch )
    if (asm_list[i].delay == 0) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      count++;
      opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
      u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
      *counterpos |= (asm_list[i].cycle << 10);
#else
      u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
      *counterpos = asm_list[i].cycle;
#endif

      ptr += *(asm_list[i].size) + nomal_seperator_size;
    }

    // No Intrupt Func ToDo: Never end block these functions
    else if (asm_list[i].delay == 0xFF ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      count++;
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size) + nomal_seperator_size;
    }

    // Normal Jump
    else if (asm_list[i].delay == 1 || asm_list[i].delay == 5 ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      if (jumpptr != 0xFFFFFFFF ) {
        intptr_t offset = *(asm_list[i].size) + nomal_seperator_size;
        memcpy((void*)(ptr + offset), (void*)internal_jmp, internal_jmp_size);
        count++;
        opcodePass(&asm_list[i], op, ptr);
        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_jmp_to_offset);

        u32 * jumppos = (u32*)(ptr + offset + internal_jmp_to_offset);
        LOG("nomal jump_offset = %08X- %08X = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | ((((jump_offset)>>2)&0x7FFFF)<<5);
        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);

#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= ((asm_list[i].cycle) << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + internal_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
        memcpy((void*)(ptr + *(asm_list[i].size) + nomal_seperator_size), (KetteAn() && !debug_mode_) ? (void*)PageFlip_k : (void*)PageFlip, DELAYJUMPSIZE);
        count++;
        opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + DELAYJUMPSIZE;
      }
    }

    // Jmp With Delay Operation
    else { 

      u32 cycle = asm_list[i].cycle;
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)delay_seperator, delay_seperator_size);
      count++;
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size) + delay_seperator_size;

      // Get NExt instruction
      temp = MappedMemoryReadWord(addr,NULL);
#ifdef SET_DIRTY
      if (ParentT) {
        /* same duplicate check as above */
        u32 keepaddr = adress_mask(addr);
        u32 parent_val = adress_mask(start_addr);
        bool already_present = false;
        for (auto pit = ParentT[keepaddr].begin(); pit != ParentT[keepaddr].end(); ++pit) {
          if (*pit == parent_val) { already_present = true; break; }
        }
        if (!already_present) {
          ParentT[keepaddr].push_back(parent_val);
          PraesenzSetzen(keepaddr);
          if (DegenDiagOn()) diag_parent_pushes_++;
        }
      }
#endif
      addr += 2;
      /* Same lookup from the table filled at startup instead of a linear
         search. dsh2_instructions is filled in BuildInstructionList() via
         FindOpCode(), so the result is identical. */
      j = dsh2_instructions[temp & 0xFFFF];
      write_memory_counter += OPFIX_write(temp, asm_list[j].write_count);

#ifdef BUILD_INFO
      if(show_code_) DumpInstX( j, addr-2, temp  );
#endif
      if (asm_list[j].func == 0) {
        LOG("Unimplemented Opcode (0x%4x) at 0x%8x\n", temp, addr-2);
        return -1;
        break;
      }
      
      asm_list[j].build_count++;
      write_memory_counter += OPFIX_write(temp, asm_list[j].write_count);

      cycle += asm_list[j].cycle;
      
      intptr_t offset = 0;
      memcpy((void*)ptr, (void*)(asm_list[j].func), *(asm_list[j].size));
      offset = *(asm_list[j].size);

      // internal loop
      if (jumpptr != 0xFFFFFFFF) {

        u32 cpsize = internal_delay_jmp_size;
        memcpy((void*)(ptr + offset), (void*)internal_delay_jmp, internal_delay_jmp_size);
        count++;
        opcodePass(&asm_list[j], temp, ptr);

        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_delay_jmp_to_offset);
        u32 * jumppos = (u32*)(ptr + offset + internal_delay_jmp_to_offset);

        LOG("delay jump_offset = %lX - %lX = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);

        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | (((jump_offset>>2)&0x7FFFF)<<5);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + internal_delay_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
        memcpy((void*)(ptr + offset), (KetteStufe() >= 2 && !debug_mode_) ? (void*)seperator_delay_after_k : (void*)seperator_delay_after, SEPERATORSIZE_DELAY_AFTER);
        count++;
        opcodePass(&asm_list[j], temp, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + SEPERATORSIZE_DELAY_AFTER;
      }
    }

    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {


      // jump to inside and no write is happend
      if (jumppc >= start_addr &&  jumppc < addr ) {
        if (write_memory_counter == 0
            && !(WANDERFIX_stufe()
                 && (SchleifeWandert(page->b_addr, addr - 2)
                     ? (INSTR(++g_wanderfix_abgelehnt), true) : false))
            && !(RECHENZYK_an() && RechenZykAb(page->b_addr, addr - 2, 0, 0))) {
          page->flags |= BLOCK_LOOP;
#ifdef BUILD_INFO 
              LOG("InfinityLoop block %08X 0x%04X  from 0x%08X to 0x%08X\n", start_addr, op, addr - 2, jumppc);
#endif
        }
      }
      else if (jumppc < start_addr && write_memory_counter == 0 ) {

        Block * tmp = NULL; 
        if ( (jumppc&0x0FF00000) == 0x06000000 && (start_addr & 0x0FF00000) == 0x06000000) {
          tmp = LookupTable[(jumppc & 0x000FFFFF) >> 1];
        }else if ((jumppc & 0x0FF00000) == 0x00200000 && (start_addr & 0x0FF00000) == 0x00200000) {
          tmp = LookupTableLow[(jumppc & 0x000FFFFF) >> 1];
        }
        else if ((jumppc & 0x0FF00000) == 0x00000000 && (start_addr & 0x0FF00000) == 0x00000000) {
          tmp = LookupTableRom[(jumppc & 0x000FFFFF) >> 1];
        }
        /* Partner not translated yet: remember the candidate instead of
           dropping it. It has already been checked to be write-free. */
        if (tmp == NULL && NACHPRUEF_enabled()) {
          NachpruefVormerken(page->b_addr, jumppc);
        }
        const bool adj_luecklos = (tmp != NULL) && ((tmp->e_addr+2) == page->b_addr);
        /* Same cycle, but jumped over a gap. Only if tmp is write-free, so the
           strictness of the normal path is kept. */
        bool zykel_geschlossen = false;
        if (ZYKEL2_enabled() && tmp != NULL && !adj_luecklos
            && (tmp->flags & BLOCK_WRITE) == 0) {
          zykel_geschlossen = ZykelSpringtNach(tmp->b_addr, tmp->e_addr, page->b_addr);
        }
        if (tmp != NULL && (tmp->flags&BLOCK_WRITE) == 0
            && (adj_luecklos || zykel_geschlossen)
            && !(WANDERFIX_stufe()
                 && ((SchleifeWandert(page->b_addr, addr - 2)
                   || SchleifeWandert(tmp->b_addr, tmp->e_addr))
                     ? (INSTR(++g_wanderfix_abgelehnt), true) : false))
            && !(RECHENZYK_an() && RechenZykAb(page->b_addr, addr - 2, tmp->b_addr, tmp->e_addr))) {
          page->flags |= BLOCK_LOOP;
          // Flag the two-block adjacency case. BLOCK_LOOP_ADJ is the hard
          // condition for the adjacent-block idle detection in Execute(); it
          // also counts how often the case is compiled.
          page->flags |= BLOCK_LOOP_ADJ;
          // Record the partner block's range. The cycle runs page -> (branch
          // back) -> tmp -> (contiguous) -> page, so after page has run the PC
          // lies in the partner, not in the block itself. Both blocks have been
          // checked to be free of writes at this point.
          /* only the cycle case gets its own bit; adj_luecklos (fall-through)
             is unaffected */
          if (!adj_luecklos && zykel_geschlossen) page->flags |= BLOCK_LOOP_ZYK;
          Chain2Merken(page->b_addr, tmp->b_addr, tmp->e_addr);
        }

      }
      write_memory_counter = 0;
      //if( (op&0xFF00) == 0x8900) continue;  // BT
      //if( (op&0xFF00) == 0x8B00) continue;  // BF
      /* The two commented lines above would keep translating across not-taken
         BT/BF instead of ending the block. That would need the PageFlip
         fall-through path in dynalib_arm64.s to check the time-slice end,
         which it does not do. */
      INSTR(g_abbr_branch++);
      break;
    }

    if (op == 0x001B && SleepEndeAn()) {  /* the block ends after SLEEP, the PC stays on SLEEP */
      break;
    }

    if ( (op & 0xF0FF) == 0x400e || (op & 0xF0FF) == 0x4007) // sh2_LDC_SR
    {
      INSTR(g_abbr_ldcsr++);   /* unavoidable: SR changes the interrupt mask, so the block must end here */
      break;
    }

    /* A store into the block's own code ends the block right after it. */
    if (smc_ende) {
      break;
    }

  }
  page->e_addr = addr-2;
  /* Collect block metadata at translation time (cheap, as translations are
     rare once warmed up): blk_befehle = mean block size in SH2 instructions,
     blk_mem = share of memory-access instructions. */
  /* Extent and flags of the finished block. */
  memcpy((void*)ptr, (KetteAn() && !debug_mode_) ? (void*)epilogue_k : (void*)epilogue, EPILOGSIZE);
  ptr += EPILOGSIZE;


  /* Only the counter since the last branch counts. */
  if (write_memory_counter > 0) {
    page->flags |= BLOCK_WRITE;
  }

  /* This block was just completed. If it was the missing partner of an
     earlier rejected candidate, the cycle is accepted now. The check is as
     strict as in the normal path: both blocks must be free of writes. */
  if (NACHPRUEF_enabled() && (page->flags & BLOCK_WRITE) == 0) {
    unsigned d = 0;
    for (unsigned i = 0; i < nachpruef_anzahl; i++) {
      bool erledigt = false;
      if (nachpruef_tab[i].ziel == page->b_addr) {
        const u32 kb = nachpruef_tab[i].kandidat_b;
        Block * kand = NULL;
        if ((kb & 0x0FF00000) == 0x06000000)      kand = LookupTable[(kb & 0x000FFFFF) >> 1];
        else if ((kb & 0x0FF00000) == 0x00200000) kand = LookupTableLow[(kb & 0x000FFFFF) >> 1];
        else if ((kb & 0x0FF00000) == 0x00000000) kand = LookupTableRom[(kb & 0x000FFFFF) >> 1];
        /* Same conditions as in the normal path, not just write freedom of
           both blocks. */
        const bool nach_adj = (kand != NULL) && ((page->e_addr + 2) == kand->b_addr);
        /* The late check needs the cycle condition as well: `page` is the
           just completed partner and `kand` the candidate; kand branches back
           to page, and page must branch to kand. */
        bool nach_zykel = false;
        if (ZYKEL2_enabled() && kand != NULL && !nach_adj
            && (kand->flags & BLOCK_WRITE) == 0) {
          nach_zykel = ZykelSpringtNach(page->b_addr, page->e_addr, kand->b_addr);
        }
        if (kand != NULL && kand->b_addr == kb && (kand->flags & BLOCK_WRITE) == 0
            && (nach_adj || nach_zykel)
            /* Same rejection as in the normal path, otherwise the second chance
               would re-accept what the first one rejected. */
            && !(WANDERFIX_stufe()
                 && ((SchleifeWandert(kand->b_addr, kand->e_addr)
                   || SchleifeWandert(page->b_addr, page->e_addr))
                     ? (INSTR(++g_wanderfix_abgelehnt), true) : false))
            && !(RECHENZYK_an() && RechenZykAb(kand->b_addr, kand->e_addr, page->b_addr, page->e_addr))) {
          kand->flags |= BLOCK_LOOP;
          /* BLOCK_LOOP_ADJ is required: the runtime path checks exactly this
             bit, so without it the late-accepted cycle would never take
             effect. */
          kand->flags |= BLOCK_LOOP_ADJ;
          /* same for the late-accepted case */
          if (!nach_adj && nach_zykel) kand->flags |= BLOCK_LOOP_ZYK;
          Chain2MerkenNurNeu(kand->b_addr, page->b_addr, page->e_addr);
          erledigt = true;
        }
      }
      if (!erledigt) nachpruef_tab[d++] = nachpruef_tab[i];
    }
    nachpruef_anzahl = d;
  }

#ifdef BUILD_INFO 
  //LOG("*********** end block size = %08X *************\n", (ptr - startptr));
  if( show_code_ ) {
      if( page->flags & BLOCK_LOOP ) LOG("Infinity loop");
      LOG("*********** end block *************\n");
  }
#endif 

#if defined(ARCH_IS_LINUX)
  cacheflush((uintptr_t)page->code,(uintptr_t)ptr,0);
#endif

#if 0 //defined(BUILD_INFO) // Dump code
  char fname[64];
#if defined(ANDROID)
  sprintf(fname,"/mnt/sdcard/yabause/%08X.bin",start_addr);
#else
  sprintf(fname,"%08X.bin",start_addr);
#endif
   FILE * fp = fopen(fname, "wb");
  if(fp){
    fwrite(page->code, sizeof(char), (uintptr_t)ptr - (uintptr_t)page->code, fp);
    fclose(fp);
  }
#endif

  return 0;
}

DynarecSh2::DynarecSh2() {
  m_pDynaSh2     = new tagSH2;
  m_pDynaSh2->getmembyte = (uintptr_t)memGetByte;
  m_pDynaSh2->getmemword = (uintptr_t)memGetWord;
  /* Fast memory routines (assembler leaf functions in dynalib_arm64.s) serve
     HWRAM 0x06xxxxxx without a C call and tail-jump to the C functions for
     everything else, so results are identical. One stage variable controls
     both reads and writes. */
  const int mf = 5;   /* fixed: read fast paths up to stage 5 */
  {
    m_pDynaSh2->getmemlong = mf ? (uintptr_t)memGetLongFast : (uintptr_t)memGetLong;
    /* stage 2: also word and byte reads */
    if (mf >= 2) {
      m_pDynaSh2->getmemword = (uintptr_t)memGetWordFast;
      m_pDynaSh2->getmembyte = (uintptr_t)memGetByteFast;
    }
    /* stage 4: also low work RAM reads (0x002); stage 5: also on-chip register
       reads via tail-jump to OnchipRead* (only when the fast I/O path is
       enabled, otherwise stage 4). */
    if (mf >= 4) {
      const int oc = (mf >= 5) && MemFastIoFastAn();
      m_pDynaSh2->getmemlong = oc ? (uintptr_t)memGetLongFast5 : (uintptr_t)memGetLongFast4;
      m_pDynaSh2->getmemword = oc ? (uintptr_t)memGetWordFast5 : (uintptr_t)memGetWordFast4;
      m_pDynaSh2->getmembyte = oc ? (uintptr_t)memGetByteFast5 : (uintptr_t)memGetByteFast4;
    }
  }
  m_pDynaSh2->setmembyte = (uintptr_t)memSetByte;
  m_pDynaSh2->setmemword = (uintptr_t)memSetWord;
  m_pDynaSh2->setmemlong = (uintptr_t)memSetLong;
  /* Stage 3: HWRAM writes with a presence-bit check (dynalib_arm64.s). If the
     bits are clear, setDirty would return immediately, so the store is done
     directly; otherwise the C function runs (invalidation). Placed after the
     default assignments above. */
  {
    if (mf >= 3) {
      m_pDynaSh2->setmemlong = (uintptr_t)memSetLongFast;
      m_pDynaSh2->setmemword = (uintptr_t)memSetWordFast;
      m_pDynaSh2->setmembyte = (uintptr_t)memSetByteFast;
    }
  }
  m_pDynaSh2->eachclock = (uintptr_t)DebugEachClock;
  /* The work RAM pointers may not exist yet here (the constructor can run
     before memory is allocated), so they are also refreshed on every block
     translation, see CompileBlock. */
  /* The inlinemem_an field is unused but kept: it keeps hiwram_base and
     lowram_base 8-byte aligned, and their offsets are hard-coded in the
     assembler. */
  m_pDynaSh2->inlinemem_an = 0u;
  m_pDynaSh2->hiwram_base = (uintptr_t)HighWram;
  m_pDynaSh2->lowram_base = (uintptr_t)LowWram;

  m_pCompiler = CompileBlocks::getInstance();
  m_ClockCounter = 0;
  m_IntruptTbl.clear();
  m_bIntruptSort = true;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  /* Zero like the neighbouring counters. */
  loopskip_cnt_ = 0;
  pre_PC_ = 0;
  ctx_ = NULL;
  mtx_ = YabThreadCreateMutex();
  logenable_ = false;
  m_pDynaSh2->memcycle = 0;

}

DynarecSh2::~DynarecSh2(){
  YabThreadFreeMutex(mtx_);
}

void DynarecSh2::ResetCPU(){
  memset((void*)m_pDynaSh2->GenReg, 0, sizeof(u32) * 16);
  memset((void*)m_pDynaSh2->CtrlReg, 0, sizeof(u32) * 3);
  memset((void*)m_pDynaSh2->SysReg, 0, sizeof(u32) * 6);

  m_pDynaSh2->CtrlReg[0] = 0x000000;  // SR
  m_pDynaSh2->CtrlReg[2] = 0x000000; // VBR
  m_pDynaSh2->SysReg[3] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2],NULL);
  m_pDynaSh2->GenReg[15] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2] + 4,NULL);
  m_pDynaSh2->SysReg[4] = 0;
  m_pDynaSh2->SysReg[5] = 0;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  m_pDynaSh2->memcycle = 0;
  m_IntruptTbl.clear();
}

void DynarecSh2::ExecuteCount( u32 Count ) {
  u32 targetcnt = 0;
  
  m_pDynaSh2->SysReg[4] = 0;
    if (Count > pre_exe_count_) {
    targetcnt = Count - pre_exe_count_;
  }
  else {
    // Just Onestep
    //Execute();
    pre_exe_count_ = (pre_exe_count_ + m_pDynaSh2->SysReg[4]) - Count ;
    return;
  }

#if 0
  // Overflow
  if (targetcnt < m_pDynaSh2->SysReg[4]){
    targetcnt = Count + (0xFFFFFFFF - m_pDynaSh2->SysReg[4]) + 1;
    m_pDynaSh2->SysReg[4] = 0;
  }
#endif

  m_pDynaSh2->exitcount = targetcnt;
  zs_erste_scheibe_ = 0;  /* new time slice: clear the marker */

  //if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
  //  this->CheckInterupt();
  //}
  m_pDynaSh2->memcycle = 0;
  while (m_pDynaSh2->SysReg[4] < targetcnt) {
    if (Execute() == IN_INFINITY_LOOP ) {
        SET_COUNT(targetcnt);
        loopskip_cnt_++;
    }
    m_pDynaSh2->SysReg[4] += m_pDynaSh2->memcycle;
    m_pDynaSh2->memcycle = 0;
    //printf("%d/%d\n",GET_COUNT(),targetcnt);
  }

  CurrentSH2->cycles = m_pDynaSh2->SysReg[4];
  g_sync_cycles[is_slave_ ? 1 : 0] += m_pDynaSh2->SysReg[4];
  //if (Count == 1) {
  //  one_step_ = true;
  //  pre_exe_count_ = 0;
  //}
  //else {
  //  one_step_ = false;
    pre_exe_count_ = m_pDynaSh2->SysReg[4] - targetcnt;
  //}
}

int DynarecSh2::CheckOneStep() {
  if (one_step_ == 1) {
    m_pDynaSh2->SysReg[3] += 2;
    return 1;
  }
  return 0;
}

void DynarecSh2::Undecoded(){

  LOG("Undecoded %08X", GET_PC());

  // Save regs.SR on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_SR());

  // Save regs.PC on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_PC()+2);


  // What caused the exception? The delay slot or a general instruction?
  // 4 for General Instructions, 6 for delay slot
  u32 vectnum = 4; //  Fix me

  // Jump to Exception service routine
  u32 newpc = memGetLong(GET_VBR() + (vectnum << 2));

  /* Note that vector 4 is always used here, although the SH-2 specification
     calls for vector 6 on a delay-slot failure. */

  SET_PC(newpc);

  return;
}

/* always_inline: Execute() must be inlined into its caller; a plain `inline`
   is only a hint to the compiler. */
__attribute__((always_inline)) inline int DynarecSh2::Execute(){

  Block * pBlock = NULL;

  /* Block entry counters (total and per CPU); compiled out in NODIAG builds. */
  { extern unsigned int g_blk_eintritte; DZ(g_blk_eintritte++);
    DZ(is_slave_ ? g_blk_s++ : g_blk_m++); }
#if defined(EXECUTE_STAT)
  m_pCompiler->setShowCode( is_slave_ );
#endif
//#endif

  if ((GET_PC() & 0xFF000000) == 0xC0000000)
  {
    pBlock = m_pCompiler->LookupTableC[(GET_PC() & 0x000FFFFF) >> 1];
    if (pBlock == NULL)
    {
      pBlock = m_pCompiler->CompileBlock(GET_PC(), NULL, is_slave_);
      m_pCompiler->LookupTableC[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
    }
  }
  else {

    switch (GET_PC() & 0x0FF00000)
    {

      // ROM
    case 0x00000000:
      if (yabsys.extend_backup) {
        const u32 bupaddr = 0x0007d600; // MappedMemoryReadLong(0x06000358);
        if (GET_PC() == bupaddr) {
          LOG("BUP_Init");
          BiosBUPInit(ctx_);
          yabsys.extend_backup = 2;
          return IN_INFINITY_LOOP;
        }
        else if (yabsys.extend_backup == 2 &&
          GET_PC() >= 0x0380 &&
          GET_PC() <= 0x03A8) {
          BiosHandleFunc(ctx_);
          return IN_INFINITY_LOOP;
        }
      }
      if (yabsys.emulatebios) {
        ctx_->cycles = 0;
         BiosHandleFunc(ctx_);
         m_pDynaSh2->memcycle += ctx_->cycles;
        return 0;
      }
      pBlock = m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1];
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC(), NULL, is_slave_);
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // Low Memory
    case 0x00200000:
      pBlock = m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1];
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC(), NULL, is_slave_);
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // High Memory
    case 0x06000000:
      /*case 0x06100000:*/

      pBlock = m_pCompiler->LookupTable[(GET_PC() & 0x000FFFFF) >> 1];
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC(), m_pCompiler->LookupParentTable, is_slave_);
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTable[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // Cache
    default:
      pBlock = m_pCompiler->CompileBlock(GET_PC(), NULL, is_slave_);
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
      break;
    }
  }
    
#if 0
    static FILE * fp = NULL;
    char fname[64];
    sprintf(fname,"/mnt/sdcard/yabause/intlog.txt");
    if( fp == NULL ) {
        fp = fopen(fname, "w");
    }
    if(fp){
        fprintf(fp,"\n---dynaExecute %08X----\n", GET_PC());
        fflush(fp);
    }
#endif
//  if(yabsys.frame_count == 7){
//    logenable_ = true;
//  }
//  if (logenable_) {
//    LOG("[%s] dynaExecute start %08X %08X", (is_slave_ == false) ? "M" : "S", GET_PC(), GET_PR());
//  }
  /* pBlock is non-NULL here and is executed regularly below. Remember its
     start address for the wild-jump diagnostics in BiosHandleFunc(). */

#if defined(DEBUG_CPU) || defined(EXECUTE_STAT)
    u32 prepc = GET_PC();
  if (is_slave_) { //statics_trigger_ == COLLECTING) {
    u64 pretime = YabauseGetTicks();
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
    compie_statics_[prepc].count++;
    compie_statics_[prepc].time += YabauseGetTicks() - pretime;
    compie_statics_[prepc].end_addr = pBlock->e_addr;
  }
  else {
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
  }
#else
  m_pDynaSh2->cur_block = (uintptr_t)pBlock;  /* remember the executing block */
  ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
  pBlock = (Block*)m_pDynaSh2->cur_block;  /* last executed block (blocks may be chained) */
#endif

  if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
    this->CheckInterupt();
  }

  if (!m_pCompiler->debug_mode_ && (pBlock->flags&BLOCK_LOOP) ){
    /* Counted before any further check: the dispatch of a BLOCK_LOOP block,
       not its success. */
    /* `<=` rather than `<`: pBlock->e_addr is the address of the last
       instruction, so the range includes it. With `<` the return branch, where
       the PC of a wait loop rests, would fall outside. */
    bool in_block = (m_pDynaSh2->SysReg[3] >= pBlock->b_addr
                     && m_pDynaSh2->SysReg[3] <= pBlock->e_addr);
    // For blocks flagged BLOCK_LOOP through the two-block adjacency case the
    // "PC inside own range" test can never succeed, because the compile
    // condition requires a branch back to before the block start. Idle is
    // instead detected when the PC lies in the partner block of the cycle,
    // whose range was recorded at translation time. Not extended to more than
    // two blocks.
    /* ZYKEL2 blocks are only skipped on the slave. On the slave the CPU is
       waiting anyway, so skipping gains time. On the master it is doing real
       work, and skipping only delays that work, which then returns in a burst.
       Only BLOCK_LOOP_ZYK is restricted; the adjacency case stays active on
       both CPUs. */
    if (!in_block && CHAIN2_enabled() && (pBlock->flags & BLOCK_LOOP_ADJ)) {
      DZ(is_slave_ ? g_c2kand_s++ : g_c2kand_m++);
      /* Only block at the first lock-out per time slice. A hit ends the time
         slice (SET_COUNT(targetcnt) makes the loop condition false), so there
         is at most one per slice. A lock-out does not change SysReg[4], so
         there can be many per slice: repeated entries into the same block.
         From the second entry into the same block within one slice the master
         is only spinning, and skipping then ends idle time instead of delaying
         work. A pass-through block is normally locked out only the first
         time, so it is unaffected. */
      if ((pBlock->flags & BLOCK_LOOP_ZYK) && ZYKSLAVE_enabled() && !is_slave_
          && !(ZSERST_enabled() && zs_erste_scheibe_ == pBlock->b_addr)) {
        DZ(g_c2zs_m++);  /* blocked by the slave-only rule */
        if (ZSERST_enabled() && zs_erste_scheibe_ == 0) {
          zs_erste_scheibe_ = pBlock->b_addr;
        }
      } else if (Chain2ImPartner(pBlock->b_addr, m_pDynaSh2->SysReg[3])) {
        in_block = true;
        DZ(is_slave_ ? g_c2tr_s++ : g_c2tr_m++);
      } else {
        DZ(is_slave_ ? g_c2np_s++ : g_c2np_m++);  /* partner range does not match */
      }
    } else if (!in_block && CHAIN2_enabled()) {
      DZ(is_slave_ ? g_c2noadj_s++ : g_c2noadj_m++);
    }
    if (in_block) {
      /* The only real idle-loop skip. loopskip_cnt_ also counts several
         BIOS/undecoded returns, so this is the counter to use for skips. */
      if (is_slave_) INSTR(g_ls46_s++); else g_ls46_m++;   /* g_ls46_m feeds the ZSERST adaptation */
      return IN_INFINITY_LOOP;
    } else {
      return 0;
    }
  }
  return 0;
}

bool operator < (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return data1.level > data2.level;
} 
bool operator == (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return ( data1.Vector == data2.Vector );
}

void DynarecSh2::RemoveInterrupt(u8 Vector, u8 level) {
  YabThreadLock(mtx_);
  m_IntruptTbl.remove_if([&](const dIntcTbl & n) { 
    return n.Vector == Vector; 
  });
  if (m_IntruptTbl.size() != 0) {
    m_IntruptTbl.sort();
    m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level << 4;
  }
  else {
    m_pDynaSh2->SysReg[5] = 0x0000;
  }
  YabThreadUnLock(mtx_);
}

void DynarecSh2::AddInterrupt( u8 Vector, u8 level )
{
  // Ignore Timer0 and Timer1 when masked
  //if ((Vector == 67 /*|| Vector == 68*/) && level <= ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)){
  //  LOG("Vector %d is skiped\n", Vector);
  //  return;
  //}

  dIntcTbl tmp;
  tmp.Vector = Vector;
  tmp.level  = level;


  YabThreadLock(mtx_);
  m_bIntruptSort = false;
  m_IntruptTbl.push_back(tmp);
  if( m_IntruptTbl.size() > 1 ) {
    m_IntruptTbl.sort();
    m_IntruptTbl.unique();
  }
  m_bIntruptSort = true;
  m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
  YabThreadUnLock(mtx_);
}


int DynarecSh2::CheckInterupt(){

  interruput_chk_cnt_++;

  if( m_IntruptTbl.size() == 0 ) {
    return 0;
  }

  
    
  YabThreadLock(mtx_);  
  dlstIntct::iterator pos = m_IntruptTbl.begin();
  if( InterruptRutine((*pos).Vector, (*pos).level ) != 0 ) {
    m_IntruptTbl.pop_front();
    if( m_IntruptTbl.size() != 0 ) {
      m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
    }else{
      m_pDynaSh2->SysReg[5] = 0x0000;
    }
    YabThreadUnLock(mtx_);
    return 1;
  }
  YabThreadUnLock(mtx_);
  return 0;
}

int DynarecSh2::InterruptRutine(u8 Vector, u8 level)
{
  if (((u32)level) > ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)) {

    interruput_cnt_++;

    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->CtrlReg[0],NULL);
    if (SleepEndeAn() && memGetWord(m_pDynaSh2->SysReg[3]) == 0x001B) {  /* an accepted IRQ wakes the CPU from SLEEP */
      m_pDynaSh2->SysReg[3] += 2;
    }
    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->SysReg[3],NULL);
    if (level == 0x10) { //NMI
      m_pDynaSh2->CtrlReg[0] |= 0x000000F0;
    }
    else {
      m_pDynaSh2->CtrlReg[0] &= ~0x000000F0;
      m_pDynaSh2->CtrlReg[0] |= ((u32)(level << 4) & 0x000000F0);
    }
    m_pDynaSh2->SysReg[3] = memGetLong(m_pDynaSh2->CtrlReg[2] + (((u32)Vector) << 2));

    //LOG("**** [%s] Exception vecnum=%s(%x), PC=%08X to %08X, level=%08X\n", (is_slave_ == false) ? "M" : "S", ScuGetVectorString(Vector), Vector,prepc, m_pDynaSh2->SysReg[3], level);

#if defined(DEBUG_CPU)
//    LOG("**** [%s] Exception vecnum=%u, PC=%08X to %08X, level=%08X\n", (is_slave_==false)?"M":"S", Vector, prepc, m_pDynaSh2->SysReg[3], level);
#endif
    return 1;
  }
  return 0; 
}


int DynarecSh2GetDisasmebleString(string & out, u32 from, u32 to) {
  char linebuf[128];
  if (from > to) return -1;
  for (u32 i = from; i < (to+2); i += 2) {
    SH2Disasm(i, MappedMemoryReadWord(i,NULL), 0, NULL, linebuf);
    out += linebuf;
    out += "\n";
  }
  return 0;
}

int DynarecSh2::Resume() {
  statics_trigger_ = NORMAL;
  return 0;
}

void DynarecSh2::ShowStatics(){
#if defined(DEBUG_CPU)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() - pre_cnt_, loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  pre_cnt_ = GET_COUNT();
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

  switch (statics_trigger_) {
  case NORMAL:
    break;
  case REQUESTED:
    statics_trigger_ = COLLECTING;
    break;
  case COLLECTING:
    statics_trigger_ = FINISHED;
    while (FINISHED == statics_trigger_) {
      YabThreadUSleep(10000);
    }
    break;
  case FINISHED:
    break;
  }
#elif defined(EXECUTE_STAT)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() , loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

for( auto i = compie_statics_.begin(); i != compie_statics_.end() ; ++i ) {
    if(i->second.time>100) LOG("%08X\t%d\t%d",i->first,i->second.count, i->second.time);
  }
compie_statics_.clear();
#endif
}

int DynarecSh2::GetCurrentStatics(MapCompileStatics & buf){
#if !defined(DEBUG_CPU)
  return -1;
#else

  if (statics_trigger_ != NORMAL && statics_trigger_ != FINISHED) return -1;

  statics_trigger_ = REQUESTED;
  while (statics_trigger_!= FINISHED) {
    YabThreadUSleep(10000);
  }
  
  buf = compie_statics_;
  compie_statics_.clear();
#endif
  return 0;
}

void DynarecSh2::ShowCompileInfo(){
  int i = 0;
  while (asm_list[i].func != NULL) {
    printf("%s: %d\n", opcode_list[i].mnem, asm_list[i].build_count);
    i++;
  }
}

void DynarecSh2::ResetCompileInfo() {
  int i = 0;
  while (asm_list[i].func != NULL) {
    asm_list[i].build_count = 0;
    i++;
  }
}
