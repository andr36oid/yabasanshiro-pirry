/*  Copyright 2005-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sndsdl.c
    \brief SDL sound interface.
*/

#ifdef HAVE_LIBSDL

/* Free-running counters, written only by the SDL audio callback. */
unsigned int g_snd_dehn = 0;
unsigned int g_snd_rufe = 0;
unsigned int g_snd_rat_mil = 1000;

unsigned int g_snd_unter = 0;
unsigned int g_snd_unter_bytes = 0;

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(__APPLE__) || defined(GEKKO)
 #ifdef HAVE_LIBSDL2
  #include <SDL2/SDL.h>
 #else
  #include <SDL/SDL.h>
 #endif
#else
 #include "SDL.h"
#endif
#include "error.h"
#include "scsp.h"
#include "sndsdl.h"
#include "debug.h"

static int SNDSDLInit(void);
static void SNDSDLDeInit(void);
static int SNDSDLReset(void);
static int SNDSDLChangeVideoFormat(int vertfreq);
static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace(void);
static void SNDSDLMuteAudio(void);
static void SNDSDLUnMuteAudio(void);
static void SNDSDLSetVolume(int volume);
#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport);
u8 SNDSDLMidiIn(int *isdata);
int SNDSDLMidiOut(u8 data);
#endif

// ---------------------------------------------------------------------------
// Decoupled sound path. Runtime switch YABA_SNDSTRETCH (env, read once in
// SNDSDLInit(), default 1 = on):
//   on : decoupled path -- ring buffer with explicit fill-level accounting,
//        adaptive fine-trim resampling (+-1%) against crystal/frame jitter,
//        packet-loss concealment (last pitch period from the ring history)
//        instead of a hard cut on a real underrun. Tempo and pitch stay
//        stable when emulation speed drops.
//   off: original path of this file (fixed 1:1 byte counter, no correction).
// Deliberately not included:
//   - deficit time stretching / grain WSOLA, which ties tempo and pitch to
//     the emulation speed.
//   - the CDDA gap fill of the SCSP core, which depends on a different
//     ring buffer architecture.
//   - the YABA_SNDTRACE/YABA_WAVDUMP capture helpers.
static int g_yaba_sndstretch = 1;

static int SNDSDLInit_Legacy(void);
static void SNDSDLDeInit_Legacy(void);
static int SNDSDLChangeVideoFormat_Legacy(int vertfreq);
static void SNDSDLUpdateAudio_Legacy(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace_Legacy(void);

static int SNDSDLInit_Stretch(void);
static void SNDSDLDeInit_Stretch(void);
static int SNDSDLChangeVideoFormat_Stretch(int vertfreq);
static void SNDSDLUpdateAudio_Stretch(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace_Stretch(void);
static void MixAudio_Stretch(UNUSED void *userdata, Uint8 *stream, int len);
static void PushFrame(s32 l, s32 r);
static void ResetStream(void);
static int SNDSDLLadetonPause(int was, unsigned int *fuell, unsigned int *aufgefuellt, unsigned int *ziel);
// ---------------------------------------------------------------------------

SoundInterface_struct SNDSDL = {
SNDCORE_SDL,
"SDL Sound Interface",
SNDSDLInit,
SNDSDLDeInit,
SNDSDLReset,
SNDSDLChangeVideoFormat,
SNDSDLUpdateAudio,
SNDSDLGetAudioSpace,
SNDSDLMuteAudio,
SNDSDLUnMuteAudio,
SNDSDLSetVolume,
#ifdef USE_SCSPMIDI
SNDSDLMidiChangePorts,
SNDSDLMidiIn,
SNDSDLMidiOut
#endif
};

#define NUMSOUNDBLOCKS  4

// Ring parameters (used only in the YABA_SNDSTRETCH=1 path): ring about 160 ms
// deep (safety margin), operating point / fill target at soundprefill
// (~67 ms), fine trim clamped to +-1% (crystal/frame jitter, never audible as
// a pitch change).
#define YABA_STRETCH_NUMSOUNDBLOCKS 8
#define YABA_STRETCH_BLOCKSAMPLES   512
#define YABA_STRETCH_PREFILL        ((44100 / 60) * 4)
#define YABA_STRETCH_RATIOMIN       0.99
#define YABA_STRETCH_RATIOMAX       1.01
#define YABA_STRETCH_RATIOGAIN      0.10
#define YABA_STRETCH_RATIOSLEW      0.10

// Emergency band of the fine trim.
//
// The normal trim is clamped to +-1% (inaudible). At that clamp two problems
// occur:
//   - Ring runs empty: the regulator sits at the upper limit 1.01 and needs
//     about 8.6 s to recover from a deficit equal to the fill target
//     (3807 frames); further underruns and PLC seams follow meanwhile.
//   - Ring runs full: the regulator sits at the lower limit 0.99, the producer
//     still delivers more, and the excess is dropped block-wise in
//     SNDSDLUpdateAudio_Stretch() -- a hard forward jump of up to 16 ms.
// So while the fill level is outside a safety band, the trim may temporarily
// go to +-1.5% (~26 cents, transient and less noticeable than a seam or a
// dropped block). Once the target is reached again the +-1% clamp applies.
// This is not deficit time stretching: 1.5% is a hard limit and the tempo
// stays tied to the wall clock.
#define YABA_STRETCH_RATIOMIN_EMG   0.985
#define YABA_STRETCH_RATIOMAX_EMG   1.015
// Lower bound relative to the fill target, below which refilling is accelerated.
#define YABA_STRETCH_LOWFILL        0.60
// Upper bound relative to the ring size, above which draining is accelerated.
#define YABA_STRETCH_HIGHFILL       0.85

// Fill target in frames; SNDSDLInit_Stretch() raises it if SDL/ALSA grants a
// larger callback period than requested (otherwise the ring periodically
// drops to 0, causing crackling at a fixed cadence).
static u32 soundprefill = YABA_STRETCH_PREFILL;

static u16 *stereodata16;
static u32 soundoffset;
static volatile u32 soundpos;
static u32 soundlen;
static u32 soundbufsize;
static SDL_AudioSpec audiofmt;
static u8 soundvolume;
static int muted = 0;

// Used only in the YABA_SNDSTRETCH=1 path:
static u32 soundbuffered;   // bytes written but not yet played
static double soundratio;   // output frames per input frame
static double soundfrac;    // fractional read position in the input block
static s32 soundlastl;      // last input frame, for interpolation
static s32 soundlastr;

// Set by the audio callback on repeated underrun, read/cleared by the regulator
// in SNDSDLUpdateAudio_Stretch(). Both sides run under the SDL audio lock, so
// a plain int is sufficient.
static int soundrebuild;
// Diagnostics (only if YABA_SNDTRACE is set): fill level report once per second.
static int g_snd_trace = -1;
static u32 soundfillmin;
static double soundfillsum;
static long soundfillcnt;
// Touched only by the SDL audio callback thread.
// plc_run: consecutive underrun callbacks. Repeated PLC on the same material
//   turns into a static hum (same period every time), so from the 5th in a
//   row it fades out cleanly instead.
// plc_lastP: last detected pitch period, start candidate of the search.
static int plc_run = 0;
static int plc_lastP = 0;

//////////////////////////////////////////////////////////////////////////////

static void MixAudio(UNUSED void *userdata, Uint8 *stream, int len) {
	int i;
	Uint8* soundbuf = (Uint8*)stereodata16;

	// original code
	for (i = 0; i < len; i++)
	{
		if (soundpos >= soundbufsize)
			soundpos = 0;
		stream[i] = muted ? audiofmt.silence : soundbuf[soundpos];
		soundpos++;
	}
}

//////////////////////////////////////////////////////////////////////////////

// Scores one candidate period for the PLC, so the search below can run
// coarse/fine instead of exhaustively. Normalized cross-correlation over a
// window of W frames immediately before the read pointer, backwards with ring wrap.
static double PlcScore(const Sint16 *ring, u32 rf, u32 pos_f, int P)
{
	const int W = 240;
	double cross = 0.0, en = 1e-9;
	int k;

	for (k = 0; k < W; k += 2) {
		u32 ai = (pos_f + rf - 1 - (u32)k) % rf;
		u32 bi = (pos_f + rf - 1 - (u32)(k + P)) % rf;
		double b0 = ring[2*bi], b1 = ring[2*bi+1];
		cross += (double)ring[2*ai]*b0 + (double)ring[2*ai+1]*b1;
		en    += b0*b0 + b1*b1;
	}
	return (cross > 0.0) ? (cross*cross)/en : 0.0;
}

//////////////////////////////////////////////////////////////////////////////

// Callback of the decoupled path. Runs on the SDL audio thread, which holds the
// audio lock for its duration, so soundoffset/soundbuffered are stable here.
static void MixAudio_Stretch(UNUSED void *userdata, Uint8 *stream, int len) {
	u32 want = (u32)len;
	u32 done = 0;

	if (stereodata16 == NULL || soundbufsize == 0)
	{
		SDL_memset(stream, audiofmt.silence, len);
		return;
	}

	// Never read past the write pointer. Reading whatever happens to sit ahead
	// of it replays samples from the previous lap of the ring, which is heard
	// as a periodic buzz, and it lets the read pointer overtake the write
	// pointer so every later free-space calculation is wrong as well.
	while (done < want && soundbuffered > 0)
	{
		u32 chunk = want - done;

		if (chunk > soundbuffered)
			chunk = soundbuffered;
		if (chunk > soundbufsize - soundpos)
			chunk = soundbufsize - soundpos;

		if (muted)
			SDL_memset(stream + done, audiofmt.silence, chunk);
		else
			SDL_memcpy(stream + done, (Uint8 *)stereodata16 + soundpos, chunk);

		soundpos += chunk;
		if (soundpos >= soundbufsize)
			soundpos = 0;
		soundbuffered -= chunk;
		done += chunk;
	}

	if (done >= want)
		plc_run = 0;

	// Underrun: silence is a far smaller artifact than stale samples, and it
	// keeps the pointers consistent so the stream recovers on the next refill.
	if (done < want) {
		u32 gap = want - done;
		u32 nframes = gap / 4;
		Sint16 *out = (Sint16 *)(stream + done);
		int plc_done = 0;

		plc_run++;

		// PLC (packet-loss concealment): instead of fading to silence, repeat the
		// last pitch period from the ring history (immediately before soundpos,
		// backwards with wrap) for a plausible tonal continuation instead of a
		// click. The history is safe to read under the audio lock. The period P
		// is estimated by maximum normalized cross-correlation over the history.
		// With silence, a too small ring or no period: fall back to the fade below.
		if (!muted && soundbufsize > 4096 && plc_run <= 4) {
			Sint16 *ring = (Sint16 *)stereodata16;
			u32 rf = soundbufsize / 4;   // ring size in frames
			u32 pos_f = soundpos / 4;    // read pointer in frames
			const int P_MIN = 32, P_MAX = 480;
			double best = 0.0; int bestP = 0, P;

			// Three-stage search, kept cheap because it runs inside the underrun
			// callback: last detected period as start candidate, coarse grid
			// (step 4), fine search +-3 around the winner.
			if (plc_lastP >= P_MIN && plc_lastP <= P_MAX) {
				best = PlcScore(ring, rf, pos_f, plc_lastP);
				if (best > 0.0) bestP = plc_lastP;
			}
			for (P = P_MIN; P <= P_MAX; P += 4) {
				double score = PlcScore(ring, rf, pos_f, P);
				if (score > best) { best = score; bestP = P; }
			}
			if (bestP > 0) {
				int lo = bestP - 3, hi = bestP + 3, q;
				if (lo < P_MIN) lo = P_MIN;
				if (hi > P_MAX) hi = P_MAX;
				for (q = lo; q <= hi; q++) {
					double score;
					if (q == bestP) continue;
					score = PlcScore(ring, rf, pos_f, q);
					if (score > best) { best = score; bestP = q; }
				}
			}
			plc_lastP = bestP;
			if (bestP > 0) {
				u32 fi;
				for (fi = 0; fi < nframes; fi++) {
					u32 si = (pos_f + rf - (u32)bestP + (fi % (u32)bestP)) % rf;
					out[2*fi]   = ring[2*si];
					out[2*fi+1] = ring[2*si+1];
				}
				if (nframes > 1764) {   // long gaps (>~40 ms) fade linearly to 0
					u32 t; s32 den = (s32)(nframes - 1764); if (den < 1) den = 1;
					for (t = 1764; t < nframes; t++) {
						s32 num = (s32)(nframes - 1 - t);
						out[2*t]   = (Sint16)((s32)out[2*t]   * num / den);
						out[2*t+1] = (Sint16)((s32)out[2*t+1] * num / den);
					}
				}
				plc_done = 1;
			}
		}

		if (!plc_done) {
			Sint16 fl = 0, fr = 0;
			if (done >= 4) {
				Sint16 *lastp = (Sint16 *)(stream + done - 4);
				fl = lastp[0];
				fr = lastp[1];
			}
			const u32 FADE = 64;
			u32 fade_n = (nframes < FADE) ? nframes : FADE;
			u32 fi;
			for (fi = 0; fi < fade_n; fi++) {
				s32 num = (s32)(FADE - 1 - fi);
				out[2 * fi]     = (Sint16)((s32)fl * num / (s32)(FADE - 1));
				out[2 * fi + 1] = (Sint16)((s32)fr * num / (s32)(FADE - 1));
			}
			if (gap > fade_n * 4)
				SDL_memset(stream + done + fade_n * 4, audiofmt.silence,
				           gap - fade_n * 4);
		}

		// Underrun diagnostics: counts underruns and the total missing bytes,
		// reports at most once per second. Runs only on this (single) SDL audio
		// callback thread, so plain statics are sufficient.
		{
			static long underrun_count = 0;
			static long underrun_bytes = 0;
			static struct timespec last_report;
			static int last_report_init = 0;
			static int reprimed_this_window = 0;
			struct timespec now;
			double elapsed;

			underrun_count++;
			underrun_bytes += (long)(want - done);

			/* Free-running counters for the per-second status line: never reset,
			   always counted, turned into per-second rates by the reader. */
			g_snd_unter++;
			g_snd_unter_bytes += (unsigned int)(want - done);

			// After a second underrun within the ~1 s window, flag the regulator in
			// SNDSDLUpdateAudio_Stretch(): it may temporarily trim up to +1.5%
			// instead of +1% to refill the ring without a gap or discarded audio.
			// The regulator clears the flag once the fill target is reached again.
			if (underrun_count >= 2 && !reprimed_this_window) {
				reprimed_this_window = 1;
				soundrebuild = 1;
			}

			clock_gettime(CLOCK_MONOTONIC, &now);
			if (!last_report_init) {
				last_report = now;
				last_report_init = 1;
			}
			elapsed = (double)(now.tv_sec - last_report.tv_sec)
			        + (double)(now.tv_nsec - last_report.tv_nsec) / 1e9;
			if (elapsed >= 1.0) {
				underrun_count = 0;
				underrun_bytes = 0;
				reprimed_this_window = 0;
				last_report = now;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLInit(void)
{
   return g_yaba_sndstretch ? SNDSDLInit_Stretch() : SNDSDLInit_Legacy();
}

static int SNDSDLInit_Legacy(void)
{
   //samples should be a power of 2 according to SDL-doc
   //so normalize it to the nearest power of 2 here
   u32 normSamples = 512;

#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif
   SDL_InitSubSystem(SDL_INIT_AUDIO);
//   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0);
//      return -1;

   audiofmt.freq = 44100;
   audiofmt.format = AUDIO_S16SYS;
   audiofmt.channels = 2;
   audiofmt.samples = (audiofmt.freq / 60) * 2;
   audiofmt.callback = MixAudio;
   audiofmt.userdata = NULL;

   while (normSamples < audiofmt.samples)
      normSamples <<= 1;

   audiofmt.samples = normSamples;

   soundlen = audiofmt.freq / 60; // 60 for NTSC or 50 for PAL. Initially assume it's going to be NTSC.
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;

   soundvolume = SDL_MIX_MAXVOLUME;

   if (SDL_OpenAudio(&audiofmt, NULL) != 0)
   {
      YabSetError(YAB_ERR_SDL, (void *)SDL_GetError());
      return -1;
   }

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   soundpos = 0;

   SDL_PauseAudio(0);

   return 0;
}

// Pre-size the ring for the slowest video format (50 Hz) and never resize it,
// so ChangeVideoFormat_Stretch() does not have to free a buffer the audio
// thread is reading from.
static int SNDSDLInit_Stretch(void)
{
#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif
   SDL_InitSubSystem(SDL_INIT_AUDIO);

   audiofmt.freq = 44100;
   audiofmt.format = AUDIO_S16SYS;
   audiofmt.channels = 2;
   audiofmt.samples = YABA_STRETCH_BLOCKSAMPLES; // already a power of two
   audiofmt.callback = MixAudio_Stretch;
   audiofmt.userdata = NULL;

   {
      unsigned blocks = YABA_STRETCH_NUMSOUNDBLOCKS;
      soundbufsize = (audiofmt.freq / 50) * blocks * 2 * 2;
   }

   soundvolume = SDL_MIX_MAXVOLUME;

   {
      SDL_AudioSpec obtained;
      memset(&obtained, 0, sizeof(obtained));
      if (SDL_OpenAudio(&audiofmt, &obtained) != 0)
      {
         YabSetError(YAB_ERR_SDL, (void *)SDL_GetError());
         return -1;
      }
      // Adapt the fill target to the callback period actually granted: some
      // SDL/ALSA combinations grant larger periods than requested, and a too
      // small target then drops to 0 periodically (crackling at a fixed
      // cadence). 3 granted periods + 1 production frame of margin.
      if (obtained.samples > 0) {
         u32 want_prefill = (u32)obtained.samples * 3 + (44100 / 60);
         if (want_prefill > soundprefill) soundprefill = want_prefill;
      }

   }


   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   ResetStream();

   SDL_PauseAudio(0);

   g_ladeton_pause = SNDSDLLadetonPause;

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLDeInit(void)
{
   if (g_yaba_sndstretch)
      SNDSDLDeInit_Stretch();
   else
      SNDSDLDeInit_Legacy();
}

static void SNDSDLDeInit_Legacy(void)
{
   SDL_CloseAudio();

   if (stereodata16)
      free(stereodata16);
}

static void SNDSDLDeInit_Stretch(void)
{
   g_ladeton_pause = NULL;
   // Closes the device first, so the callback is no longer running by the
   // time the buffer it reads from goes away.
   SDL_CloseAudio();

   if (stereodata16)
   {
      free(stereodata16);
      stereodata16 = NULL;
   }
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLReset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLChangeVideoFormat(int vertfreq)
{
   return g_yaba_sndstretch ? SNDSDLChangeVideoFormat_Stretch(vertfreq)
                             : SNDSDLChangeVideoFormat_Legacy(vertfreq);
}

static int SNDSDLChangeVideoFormat_Legacy(int vertfreq)
{
   soundlen = audiofmt.freq / vertfreq;
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;

   if (stereodata16)
      free(stereodata16);

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   return 0;
}

static int SNDSDLChangeVideoFormat_Stretch(UNUSED int vertfreq)
{
   // The ring is already sized for the slowest format, so there is nothing to
   // reallocate. Just re-silence it and reset the bookkeeping.
   SDL_LockAudio();

   if (stereodata16)
      memset(stereodata16, 0, soundbufsize);

   ResetStream();

   SDL_UnlockAudio();

   return 0;
}

/* Brings the ring to the target fill level WITHOUT discarding audio that has not been played yet.
   Call under SDL_LockAudio. Target = soundprefill frames, the same target ResetStream sets and the regulator
   (SNDSDLUpdateAudio_Stretch, target) controls to. If short, exactly that much silence is appended at the WRITE end
   (soundoffset/soundbuffered as in PushFrame); soundpos and the ring contents stay untouched.
   Afterwards the regulator/stretcher state is reset like ResetStream (soundratio=1.0, soundrebuild, plc_run,
   plc_lastP, fill statistics), so there is no multi-second catch-up at 1.015. The interpolation state (soundfrac,
   soundlastl/r) is zeroed only if silence was appended: the last written frame is then 0 and the join is seamless;
   without an append the interpolation continues from the old last frame.
   nur_unter_ziel=1: if the fill level is at or above the target, everything stays untouched (returns 0). */
static int LadetonAuffuellenGesperrt(unsigned int *fuell_vorher, unsigned int *aufgefuellt, unsigned int *ziel, int nur_unter_ziel)
{
   const u32 framebytes = sizeof(s16) * 2;
   u32 soll, add;
   if (stereodata16 == NULL || soundbufsize == 0)
      return 0;
   soll = soundprefill * framebytes;
   if (soll > soundbufsize - framebytes)   /* safety clamp: never fill the whole ring */
      soll = (soundbufsize - framebytes) & ~(framebytes - 1);
   *fuell_vorher = soundbuffered / framebytes;
   *ziel = soll / framebytes;
   if (nur_unter_ziel && soundbuffered >= soll)
      return 0;
   add = (soundbuffered < soll) ? ((soll - soundbuffered) & ~(framebytes - 1)) : 0;
   if (add > 0)
   {
      u32 rest = add;
      while (rest > 0)   /* append silence at the write end, with ring wrap */
      {
         u32 stueck = soundbufsize - soundoffset;
         if (stueck > rest) stueck = rest;
         memset(((u8 *)stereodata16) + soundoffset, 0, stueck);
         soundoffset += stueck;
         if (soundoffset >= soundbufsize) soundoffset = 0;
         rest -= stueck;
      }
      soundbuffered += add;
      soundfrac = 0.0;
      soundlastl = 0;
      soundlastr = 0;
      *aufgefuellt = add / framebytes;
   }
   /* reset regulator/stretcher state like ResetStream, without touching pointers or contents */
   soundratio = 1.0;
   soundrebuild = 0;
   plc_run = 0;
   plc_lastP = 0;
   soundfillmin = 0xFFFFFFFFu;
   soundfillsum = 0.0;
   soundfillcnt = 0;
   return 1;
}

/* Pauses SDL output during a lock. SDL_PauseAudio(1) makes the audio thread output silence without calling the
   callback, so nothing is consumed and ring/regulator stay put; the callback never blocks (SDL only takes the device
   lock briefly, a running callback makes the CALLER wait). Never call under SDL_LockAudio.
   was=1 pause - was=0 resume + refill only if below target - was=-1 resume only (saving). */
static int SNDSDLLadetonPause(int was, unsigned int *fuell, unsigned int *aufgefuellt, unsigned int *ziel)
{
   int r = 0;
   *fuell = 0; *aufgefuellt = 0; *ziel = 0;
   if (!g_yaba_sndstretch || stereodata16 == NULL)
      return 0;
   if (was == 1)
   {
      SDL_PauseAudio(1);
      SDL_LockAudio();
      *fuell = soundbuffered / (sizeof(s16) * 2);
      SDL_UnlockAudio();
      return 1;
   }
   SDL_LockAudio();
   if (was == 0)
      r = LadetonAuffuellenGesperrt(fuell, aufgefuellt, ziel, 1);
   else
      *fuell = soundbuffered / (sizeof(s16) * 2);
   SDL_UnlockAudio();
   SDL_PauseAudio(0);
   return r;
}

//////////////////////////////////////////////////////////////////////////////

static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len) {
   u32 i;

   for (i = 0; i < len; i++)
   {
      // Left Channel
      *srcL = ( *srcL *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcL > 0x7FFF) *dst = 0x7FFF;
      else if (*srcL < -0x8000) *dst = -0x8000;
      else *dst = *srcL;
      srcL++;
      dst++;
      // Right Channel
	  *srcR = ( *srcR *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcR > 0x7FFF) *dst = 0x7FFF;
      else if (*srcR < -0x8000) *dst = -0x8000;
      else *dst = *srcR;
      srcR++;
      dst++;
   }
}

static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   if (g_yaba_sndstretch)
      SNDSDLUpdateAudio_Stretch(leftchanbuffer, rightchanbuffer, num_samples);
   else
      SNDSDLUpdateAudio_Legacy(leftchanbuffer, rightchanbuffer, num_samples);
}

static void SNDSDLUpdateAudio_Legacy(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   u32 copy1size=0, copy2size=0;
   SDL_LockAudio();

   if ((soundbufsize - soundoffset) < (num_samples * sizeof(s16) * 2))
   {
      copy1size = (soundbufsize - soundoffset);
      copy2size = (num_samples * sizeof(s16) * 2) - copy1size;
   }
   else
   {
      copy1size = (num_samples * sizeof(s16) * 2);
      copy2size = 0;
   }

   sdlConvert32uto16s((s32 *)leftchanbuffer, (s32 *)rightchanbuffer,
   (s16 *)(((u8 *)stereodata16)+soundoffset), copy1size / sizeof(s16) / 2);

   if (copy2size)
      sdlConvert32uto16s((s32 *)leftchanbuffer + (copy1size / sizeof(s16) / 2),
      (s32 *)rightchanbuffer + (copy1size / sizeof(s16) / 2), (s16 *)stereodata16,
       copy2size / sizeof(s16) / 2);

   soundoffset += copy1size + copy2size;
   soundoffset %= soundbufsize;

   SDL_UnlockAudio();
}

// Rewinds the ring to the primed state. The buffer is kept zeroed, so the
// primed region plays as silence. Callers must hold the audio lock, or be
// sure the device is not running yet.
static void ResetStream(void)
{
   soundpos = 0;
   soundoffset = soundprefill * sizeof(s16) * 2;
   soundbuffered = soundoffset;

   soundratio = 1.0;
   soundfrac = 0.0;
   soundlastl = 0;
   soundlastr = 0;
   soundrebuild = 0;
   plc_run = 0;
   plc_lastP = 0;
   soundfillmin = 0xFFFFFFFFu;
   soundfillsum = 0.0;
   soundfillcnt = 0;
}

// Writes one output frame, already scaled and clipped, at the write pointer.
// The audio lock must be held.
static void PushFrame(s32 l, s32 r)
{
   s16 *dst;

   // Scale through a wider type and leave the source alone: the input is the
   // SCSP's own output ring, which other consumers read as well, so
   // in-place scaling would both destroy their data and could overflow s32.
   l = (s32)(((s64)l * soundvolume) / SDL_MIX_MAXVOLUME);
   r = (s32)(((s64)r * soundvolume) / SDL_MIX_MAXVOLUME);

   if (l > 0x7FFF) l = 0x7FFF;
   else if (l < -0x8000) l = -0x8000;
   if (r > 0x7FFF) r = 0x7FFF;
   else if (r < -0x8000) r = -0x8000;

   dst = (s16 *)(((u8 *)stereodata16) + soundoffset);
   dst[0] = (s16)l;   // Left Channel
   dst[1] = (s16)r;   // Right Channel

   soundoffset += sizeof(s16) * 2;
   if (soundoffset >= soundbufsize)
      soundoffset = 0;
   soundbuffered += sizeof(s16) * 2;
}

// Adaptive fine-trim resampling. The emulator produces 44100/60 samples per
// emulated video frame (an audio rate tied to the frame rate), while the
// device consumes at its own crystal rate of 44100 Hz; the two never match
// exactly. Without correction the excess is dropped in small chunks
// (crackling) or gaps appear. The correction is clamped to +-1% (see
// YABA_STRETCH_RATIOMIN/MAX), far below an audible pitch change. It is
// deliberately only a crystal/jitter fine trim, not deficit time stretching:
// on larger production gaps (emulation running below speed) the output stays
// at the same speed/pitch and the gap itself is covered by PLC/fade in
// MixAudio_Stretch().
static void SNDSDLUpdateAudio_Stretch(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   const s32 *srcl = (const s32 *)leftchanbuffer;
   const s32 *srcr = (const s32 *)rightchanbuffer;
   const u32 framebytes = sizeof(s16) * 2;
   double step, target, err, want, fill, rmin, rmax;
   s32 last_i = -1;

   SDL_LockAudio();

   if (stereodata16 == NULL || soundbufsize == 0 || num_samples == 0)
   {
      SDL_UnlockAudio();
      return;
   }

   // soundfrac walks the input block; every time it lands somewhere we emit
   // one output frame interpolated between the two input frames it falls
   // between. soundlastl/r carry the block's last frame so the interpolation
   // is continuous across calls instead of restarting at every block
   // boundary.
   step = 1.0 / soundratio;

   while (soundfrac < (double)num_samples)
   {
      s32 i = (s32)soundfrac;            // >= 0, so this truncates like floor
      double f = soundfrac - (double)i;
      s32 al = (i == 0) ? soundlastl : srcl[i - 1];
      s32 ar = (i == 0) ? soundlastr : srcr[i - 1];

      // The caller sized this block from SNDSDLGetAudioSpace(), so this only
      // trips if the device stopped consuming entirely.
      if (soundbuffered + framebytes > soundbufsize)
         break;

      PushFrame((s32)(al + (srcl[i] - al) * f),
                (s32)(ar + (srcr[i] - ar) * f));

      last_i = i;
      soundfrac += step;
   }

   if (soundfrac >= (double)num_samples)
   {
      // Block fully processed: history set to the last input sample, remaining
      // phase carried into the next block.
      soundfrac -= (double)num_samples;
      soundlastl = srcl[num_samples - 1];
      soundlastr = srcr[num_samples - 1];
   }
   else
   {
      // Ring full: the rest of the block is dropped (it does not fit), but the
      // interpolation history must not jump to the end of the whole block
      // (phase jump at the cut, audible as a seam/click). It stays at the last
      // input sample actually used; soundfrac restarts cleanly at the next block.
      soundfrac = 0.0;
      if (last_i >= 0)
      {
         soundlastl = srcl[last_i];
         soundlastr = srcr[last_i];
      }
   }

   // Steer the ring towards the primed fill level. Proportional only: a
   // small standing error is fine here, it just means the ring settles a
   // few ms off the target, whereas an integral term would keep winding the
   // ratio up against a rate difference that never goes away.
   target = (double)(soundprefill * framebytes) / (double)soundbufsize;
   fill = (double)soundbuffered / (double)soundbufsize;
   err = fill - target;

   want = 1.0 - YABA_STRETCH_RATIOGAIN * err;

   // Emergency band (see YABA_STRETCH_*_EMG above).
   // soundratio = output frames per input frame, so:
   //   ratio > 1  -> more output frames, ring fills, pitch slightly lower
   //   ratio < 1  -> fewer output frames, ring drains, pitch slightly higher
   // Hence a too low fill level raises the UPPER clamp and a too high one
   // lowers the LOWER clamp.
   rmin = YABA_STRETCH_RATIOMIN;
   rmax = YABA_STRETCH_RATIOMAX;
   if (soundrebuild || fill < target * YABA_STRETCH_LOWFILL)
      rmax = YABA_STRETCH_RATIOMAX_EMG;
   if (fill > YABA_STRETCH_HIGHFILL)
      rmin = YABA_STRETCH_RATIOMIN_EMG;
   if (soundrebuild && fill >= target)
      soundrebuild = 0;

   if (want > rmax) want = rmax;
   else if (want < rmin) want = rmin;

   soundratio += (want - soundratio) * YABA_STRETCH_RATIOSLEW;

   /* Stretch statistics. soundratio = output frames per input frame; 1.0 = no stretching.
      g_snd_dehn counts regulation steps with more than 0.5% stretching, g_snd_rufe counts
      all steps (their ratio is the stretch rate), g_snd_rat_mil holds the current ratio x1000. */
   {
      const double abw = (soundratio > 1.0) ? (soundratio - 1.0) : (1.0 - soundratio);
      g_snd_rufe++;
      /* The threshold lies below the clamp ([0.99, 1.01], emergency band
         [0.985, 1.015]); 0.5% is the range in which the regulator actually works. */
      if (abw > 0.005) g_snd_dehn++;
      g_snd_rat_mil = (unsigned int)(soundratio * 1000.0 + 0.5);
   }


   SDL_UnlockAudio();
}

//////////////////////////////////////////////////////////////////////////////

static u32 SNDSDLGetAudioSpace(void)
{
   return g_yaba_sndstretch ? SNDSDLGetAudioSpace_Stretch() : SNDSDLGetAudioSpace_Legacy();
}

static u32 SNDSDLGetAudioSpace_Legacy(void)
{
   u32 freespace=0;

   if (soundoffset > soundpos)
      freespace = soundbufsize - soundoffset + soundpos;
   else
      freespace = soundpos - soundoffset;

   return (freespace / sizeof(s16) / 2);
}

static u32 SNDSDLGetAudioSpace_Stretch(void)
{
   u32 freeframes;
   double ratio, in;

   // Derived from an explicit fill count rather than from the two pointers:
   // soundoffset == soundpos is both "empty" and "full", and comparing the
   // raw pointers always reads it as "full", so a ring the callback had just
   // drained would look like it had no room at all.
   SDL_LockAudio();
   freeframes = (soundbufsize - soundbuffered) / (sizeof(s16) * 2);
   ratio = soundratio;
   SDL_UnlockAudio();

   // Reported in input frames. One input frame becomes `ratio` output
   // frames, so while the resampler is compressing we can take in more than
   // we have room for. One frame is held back to cover soundfrac's carry.
   in = (double)freeframes / ratio;
   if (in < 2.0)
      return 0;

   return (u32)in - 1;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLMuteAudio(void)
{
   muted = 1;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLUnMuteAudio(void)
{
   muted = 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLSetVolume(int volume)
{
   soundvolume = ( (double)SDL_MIX_MAXVOLUME /(double)100 ) *volume;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport)
{
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

u8 SNDSDLMidiIn(int *isdata)
{
	*isdata = 0;
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

int SNDSDLMidiOut(u8 data)
{
	return 1;
}

//////////////////////////////////////////////////////////////////////////////
#endif
#endif
