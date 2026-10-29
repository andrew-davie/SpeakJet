/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * speakjet: an emulation of the SpeakJet speech and sound synthesizer used in
 * the AtariVox. second, from-scratch implementation (branch "rebuild"): written
 * from the SpeakJet User's Manual and recordings of two AtariVoxes only, with
 * the smallest footprint that still matches them. procedural rules wherever a
 * rule was found; per-sound parameters are a few bytes each.
 *
 * Written by Claude, Anthropic's AI model, under the direction of Andrew Davie.
 *
 * What the chip was found to do (from the recordings):
 *   - it makes samples at about 8026 Hz (manual: 8192) and holds each one; the
 *     AtariVox then low-passes the held signal: two real poles, near 2 kHz
 *     (fitted on the voicing pulses of both units) and 2.9 kHz (from the
 *     images of the tones, 6.5 to 23 kHz, both units). the output here is at
 *     48 kHz so the images the hold puts up to 20 kHz are carried as the
 *     real thing's are
 *   - voicing: a period of N = floor(rate / Pitch) + 1 ticks. at the start of
 *     each period the three formant oscillators (sines, manual: oscillators 1
 *     to 3) restart and an envelope (manual: the envelope oscillator) steps
 *     to full at once and falls linearly over about 7 ms (there is no rise:
 *     measured tick by tick, the pulse starts abruptly); the sum of the sines
 *     times the envelope is the voice. the pulse is the same on both units to
 *     the sample
 *   - noise: an oscillator whose frequency is jittered ("disordered with white
 *     noise", manual register 6) makes a band of noise centred on its
 *     frequency; the unvoiced sounds are one such band each
 */

#include "speakjet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- chip ---- */

/* the rate the chip makes samples at. measured from the images of its tones on
 * two units: 8039 and 8013 Hz (the manual says 8192). the chip's own
 * arithmetic assumes 8000: its tones come out Fs/8000 sharp on both units, and
 * the voicing period is floor(8000 / Pitch) + 1 ticks */
#define RATE 8026.0
#define NOMINAL 8000.0

#define TICKS(ms) ((int)((ms) * RATE / 1000.0 + 0.5))

/* the constants of the model. the values are what the recordings gave; a
 * build with -DSJ_TUNE reads overrides from the SJ_TUNE environment variable
 * ("ATTACK=35,FLOOR=0.06,...") so they can be fitted without recompiling */
static struct tune {
  double env_rise, env_fall;      /* voicing envelope: rise (under one tick: a step) and linear fall, ticks */
  double lpf_fc, lpf_q;           /* the output stage's two poles */
  double attack, release;         /* every sound's level ramp, ms */
  double fx_attack, fx_release;   /* the effects' */
  double floor;                   /* the output noise floor, relative */
  double dist2, dist3;            /* the output stage's nonlinearity: v + dist2 v^2 + dist3 v^3 */
  double jitter;                  /* every oscillator's frequency wanders by this fraction each sample
                                   * (the skirts around the chip's tones) */
  double clip;                    /* mixer 1 clips the formants' sum at this level (manual: 63; 0 = none) */
  double quant;                   /* the output's steps per full scale (0 = none): the chip's output is
                                   * coarse, about 5 bits across a vowel, which is the broadband hiss
                                   * under its voice */
  double decay_db;                /* an FX_DECAY part falls this many dB */
  double transition;              /* the voice moves to a new sound's target over this, ms */
  double closure;                 /* silence before a stop's burst, ms */
  double overlap, overlap_v;      /* a sound followed at once by another is cut short by this, ms; more when both are voiced */
  double speed_a, speed_b;        /* length factor (speed_a - Speed) / speed_b */
  double fx_speed_a, fx_speed_b;  /* the effects' (they shorten faster) */
  double fast, slow;              /* Fast and Slow add this times the manual's length */
  double copy_extra;              /* a Repeat copy adds this to the manual's length, ms */
  double bend_hz;                 /* Bend step, Hz */
  double pause[7];                /* the pauses, ms (the manual's) */
} T = {0.5, 58, 2396, 0.491, 35, 30, 10, 15, 0.018, 0, 0, 0.03, 0, 48, 35, 25, 30, 15, 30, 162, 48, 149, 35, -0.3, 0.35, 0, 44, {0, 100, 200, 700, 30, 60, 90}};

#ifdef SJ_TUNE
#include <stdio.h>
static void tune_from_env(void) {
  static const char *const names[] = {"ENVR", "ENVF", "LPFFC", "LPFQ", "ATTACK", "RELEASE", "FXATT", "FXREL", "FLOOR", "DIST2", "DIST3", "JITTER", "CLIP", "QUANT", "DECAY", "TRANS", "CLOSURE", "OVERLAP", "OVERLAPV", "SPEEDA", "SPEEDB", "FXSPEEDA", "FXSPEEDB", "FAST", "SLOW", "COPY", "BENDHZ", "P0", "P1", "P2", "P3", "P4", "P5", "P6"};
  const char *e = getenv("SJ_TUNE");
  double *v = (double *)&T;
  while (e && *e) {
    char key[16];
    double x;
    int n = 0;
    if (sscanf(e, "%15[A-Z0-9]=%lf%n", key, &x, &n) == 2) {
      for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(names[i], key) == 0)
          v[i] = x;
      e += n;
    } else
      break;
    while (*e == ',')
      e++;
  }
}
#endif


/* one target the synthesizer moves toward: three voiced formants (Hz, level 0
 * to 31), the voice's offset (dc: a fourth oscillator at 0 Hz under the same
 * envelope, the level the pulse sits below the formants' sum) and one noise
 * band (centre Hz, half width Hz, level 0 to 31) */
typedef struct frame {
  float f[3], a[3], dc;
  float nf, nw, na;
} frame;

/* a sound is a few segments; each holds or glides to a frame for some ticks */
typedef struct segment {
  int ticks;
  frame from, to;
  bool voiced;
  int gate;      /* ticks of the on/off period for a pulsed effect, 0 = none */
  uint8_t flags; /* FX_* */
} segment;

/* the sound effects: each is a few parts. a part plays up to three tones
 * (plain sines; with FX_VOICED the first two are the voice's formants and the third stays a plain tone), a noise band, for
 * ms; the tones sweep from fa to fb (linearly, or FX_EXP geometrically, or
 * FX_EXPDECAY settling toward fb); FX_DECAY fades the level to nothing over
 * the part, FX_BUMP shapes it as a half sine; gate pulses it on and off;
 * FX_PULSED pulses the plain tones with the voice's envelope at Pitch */
enum { FX_VOICED = 1, FX_EXP = 2, FX_DECAY = 4, FX_BUMP = 8, FX_EXPDECAY = 16, FX_NSWEEP = 32, FX_PULSED = 64 };

/* the tables' units: frequencies in 16 Hz steps, lengths in 5 ms, so every
 * field is a byte */
#define HZ_STEP 16
#define MS_STEP 5

typedef struct fxpart {
  uint8_t ms;         /* MS_STEP */
  uint8_t fa[3], fb[3]; /* HZ_STEP */
  uint8_t v[3];
  uint8_t nf, nw;     /* HZ_STEP */
  uint8_t nv, gate, flags; /* gate in ms */
} fxpart;

#define MAX_SEGMENTS 8


struct sj_chip {
  /* the input buffer: a FIFO of raw bytes, as the chip's is */
  uint8_t buf[SJ_BUFFER_SIZE];
  int head, count;
  int executing; /* bytes of the command being executed, still counted */
  int pending_code;

  /* the controls */
  uint8_t volume, speed, pitch, bend, repeat;
  bool fast, slow, stress, relax;

  /* what is playing */
  segment seg[MAX_SEGMENTS];
  int nseg, cur, tick;
  int stick, stotal; /* ticks into the sound, and its whole length */
  int attack, release;
  int copies;        /* copies of a Repeat still to play */
  int copy_of;       /* which allophone */
  frame last; /* where the voice is, for transitions into the next sound */
  bool sounding;

  /* synthesizer state */
  int period, ptick;
  double phase[3];
  double nz[1]; /* the noise oscillator's phase */
  uint32_t rnd;
  double lastr; /* the previous noise sample, for the floor's high pass */

  /* output */
  double held, heldpos;
  double z[2];
  double lpf[5]; /* the low pass's coefficients: b0 b1 b2 a1 a2 */
  bool speaking;
};

/* ------------------------------------------------------------- tables ---- */

/* the sound component database: per allophone, its kind, manual length and
 * targets. generated from params.json by tools/gen_tables.py; edit that */
enum { K_VOWEL, K_GLIDE, K_NASAL, K_VFRIC, K_FRIC, K_STOP, K_VSTOP, K_AFFR };

typedef struct allophone {
  uint8_t kind;
  uint8_t ms;         /* the length played at Speed 114, measured, MS_STEP */
  uint8_t f1, f2, f3; /* formants, HZ_STEP */
  uint8_t a1, a2, a3; /* their levels, 0 to 31 */
  uint8_t g1, g2, g3; /* K_GLIDE: the second target */
  uint8_t nf, nw;     /* the noise band's centre and throw, HZ_STEP */
  uint8_t na;
  uint8_t cp;         /* the length of each further copy of a Repeat, MS_STEP */
  int8_t dc;          /* the voice's offset under the envelope, formant level units */
} allophone;

/* TABLES-BEGIN */
#include "speakjet_tables.c"
/* TABLES-END */

static const char *const names[] = {
    "IY", "IH", "EY", "EH", "AY", "AX", "UX", "OH", "AW", "OW", "UH", "UW", "MM", "NE", "NO", "NGE", "NGO", "LE",
    "LO", "WW", "RR", "IYRR", "EYRR", "AXRR", "AWRR", "OWRR", "EYIY", "OHIY", "OWIY", "OHIH", "IYEH", "EHLL",
    "IYUW", "AXUW", "IHWW", "AYWW", "OWWW", "JH", "VV", "ZZ", "ZH", "DH", "BE", "BO", "EB", "OB", "DE", "DO", "ED",
    "OD", "GE", "GO", "EG", "OG", "CH", "HE", "HO", "WH", "FF", "SE", "SO", "SH", "TH", "TT", "TU", "TS", "KE", "KO",
    "EK", "OK", "PE", "PO", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "A0", "A1", "A2", "A3", "A4",
    "A5", "A6", "A7", "A8", "A9", "B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "C0", "C1", "C2", "C3",
    "C4", "C5", "C6", "C7", "C8", "C9", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11",
    "M0", "M1", "M2"};

const char *sj_sound_name(uint8_t code) {
  return code >= 128 && code <= 254 ? names[code - 128] : NULL;
}

/* -------------------------------------------------------------- API ---- */

sj_chip *sj_new(void) {
  sj_chip *sj = (sj_chip *)calloc(1, sizeof(sj_chip));
  if (sj != NULL)
    sj_power_on(sj);
  return sj;
}

void sj_free(sj_chip *sj) { free(sj); }

static void reset_controls(sj_chip *sj) {
  sj->volume = SJ_DEFAULT_VOLUME;
  sj->speed = SJ_DEFAULT_SPEED;
  sj->pitch = SJ_DEFAULT_PITCH;
  sj->bend = SJ_DEFAULT_BEND;
  sj->repeat = 0;
  sj->fast = sj->slow = sj->stress = sj->relax = false;
}

void sj_power_on(sj_chip *sj) {
  memset(sj, 0, sizeof(*sj));
  reset_controls(sj);
  sj->pending_code = -1;
  sj->rnd = 0x12345678u;
#ifdef SJ_TUNE
  tune_from_env();
#endif
  /* the output low pass: two real poles, bilinear, prewarped */
  {
    const double w = 2.0 * SJ_SAMPLE_RATE * tan(M_PI * T.lpf_fc / SJ_SAMPLE_RATE);
    const double k = 2.0 * SJ_SAMPLE_RATE;
    const double d = k * k + w / T.lpf_q * k + w * w;
    sj->lpf[0] = w * w / d;
    sj->lpf[1] = 2.0 * sj->lpf[0];
    sj->lpf[2] = sj->lpf[0];
    sj->lpf[3] = (2.0 * w * w - 2.0 * k * k) / d;
    sj->lpf[4] = (k * k - w / T.lpf_q * k + w * w) / d;
  }
}

void sj_reset(sj_chip *sj) {
  sj->head = sj->count = 0;
  sj->executing = 0;
  sj->pending_code = -1;
  reset_controls(sj);
}

int sj_command_size(uint8_t code) {
  switch (code) {
  case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 29: case 30:
    return 2;
  }
  return 1;
}

bool sj_write(sj_chip *sj, uint8_t code, uint8_t data) {
  const int n = sj_command_size(code);
  if (sj->count + n > SJ_BUFFER_SIZE)
    return false;
  sj->buf[(sj->head + sj->count) % SJ_BUFFER_SIZE] = code;
  if (n == 2)
    sj->buf[(sj->head + sj->count + 1) % SJ_BUFFER_SIZE] = data;
  sj->count += n;
  return true;
}

bool sj_write_byte(sj_chip *sj, uint8_t byte) {
  if (sj->pending_code >= 0) {
    const uint8_t code = (uint8_t)sj->pending_code;
    sj->pending_code = -1;
    return sj_write(sj, code, byte);
  }
  if (sj_command_size(byte) == 2) {
    sj->pending_code = byte;
    return true;
  }
  return sj_write(sj, byte, 0);
}

bool sj_buffer_half_full(const sj_chip *sj) { return sj->count > SJ_BUFFER_HALF_FULL; }
bool sj_speaking(const sj_chip *sj) { return sj->speaking; }
int sj_buffered(const sj_chip *sj) { return sj->count; }

void sj_get_controls(const sj_chip *sj, sj_controls *c) {
  c->volume = sj->volume;
  c->speed = sj->speed;
  c->pitch = sj->pitch;
  c->bend = sj->bend;
  c->repeat = sj->repeat;
  c->fast = sj->fast;
  c->slow = sj->slow;
  c->stress = sj->stress;
  c->relax = sj->relax;
  c->pending_code = sj->pending_code;
}

void sj_set_controls(sj_chip *sj, const sj_controls *c) {
  sj->volume = c->volume > 127 ? 127 : c->volume;
  sj->speed = c->speed > 127 ? 127 : c->speed;
  sj->pitch = c->pitch;
  sj->bend = c->bend > 15 ? 15 : c->bend;
  sj->repeat = c->repeat;
  sj->fast = c->fast;
  sj->slow = c->slow;
  sj->stress = c->stress;
  sj->relax = c->relax;
  sj->pending_code = c->pending_code >= 0 && c->pending_code < 256 && sj_command_size((uint8_t)c->pending_code) == 2 ? c->pending_code : -1;
}

/* ------------------------------------------------------------- the MSA ---- */

/* the manual's lengths, for the copies of a Repeat */
static const uint8_t manual_ms[72] = {
    70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,
    70,  70,  70,  200, 200, 190, 200, 185, 165, 200, 225, 185, 170, 140, 180, 170, 170, 200,
    131, 70,  70,  70,  70,  70,  45,  45,  10,  10,  45,  45,  10,  10,  55,  55,  55,  55,
    70,  70,  70,  70,  70,  40,  40,  50,  40,  50,  70,  170, 55,  55,  55,  45,  99,  99};

/* the length of a sound at the current Speed, as a factor on its length at
 * Speed 114. linear: measured 95/197/287 ms for AW at Speed 127/90/60; the
 * effects shorten faster (0.63 at 127, 1.55 at 90, 2.6 at 62) */
static double speed_factor(const sj_chip *sj, bool effect) {
  return effect ? (T.fx_speed_a - sj->speed) / T.fx_speed_b : (T.speed_a - sj->speed) / T.speed_b;
}

/* Fast and Slow take away or add a share of the manual's length (measured
 * half: AW +35, IH +40,
 * SE +20, EYIY +75 ms under Slow): the manual's "1/2" and "1 1/2 times" are
 * of its own length column */
static double modified_ms(const sj_chip *sj, const allophone *a, double ms) {
  if (sj->fast)
    ms += T.fast * manual_ms[a - allophones];
  if (sj->slow)
    ms += T.slow * manual_ms[a - allophones];
  return ms > 10 ? ms : 10;
}

/* Bend: every frequency moves by the same amount, 44 Hz a step (D1's 697
 * and 1209 Hz both step by 44). below 5 there is a 22 Hz jump */
static double bend_hz(const sj_chip *sj) {
  return T.bend_hz * (sj->bend - 5) + (sj->bend < 5 ? T.bend_hz / 2 : 0.0);
}

/* the master volume: measured on AW, 16 to 127 is only 6 dB */
static double volume_gain(const sj_chip *sj) {
  static const double db[5] = {-3.3, -1.3, -0.6, 0.0, 2.5};
  static const double at[5] = {16, 32, 64, 96, 127};
  const double v = sj->volume;
  if (v <= 0)
    return 0.0;
  double d;
  if (v < at[0])
    d = db[0] - (at[0] - v) * 0.6;
  else {
    int i = 0;
    while (i < 3 && v > at[i + 1])
      i++;
    d = db[i] + (db[i + 1] - db[i]) * (v - at[i]) / (at[i + 1] - at[i]);
  }
  return pow(10.0, d / 20.0);
}

static float bent(double hz, double bend) { return hz > 0 ? (float)(hz + bend) : 0.0f; }

static frame silent_frame(void) {
  frame f;
  memset(&f, 0, sizeof f);
  return f;
}

static void add_segment(sj_chip *sj, int ticks, frame from, frame to, bool voiced) {
  if (sj->nseg >= MAX_SEGMENTS || ticks <= 0)
    return;
  segment *s = &sj->seg[sj->nseg++];
  s->ticks = ticks;
  s->from = from;
  s->to = to;
  s->voiced = voiced;
  s->gate = 0;
  s->flags = 0;
}

/* start playing the segments built so far */
static void start(sj_chip *sj, bool effect) {
  sj->attack = TICKS(effect ? T.fx_attack : T.attack);
  sj->release = TICKS(effect ? T.fx_release : T.release);
  sj->cur = sj->tick = sj->stick = 0;
  sj->copies = 0;
  sj->stotal = 0;
  for (int i = 0; i < sj->nseg; i++)
    sj->stotal += sj->seg[i].ticks;
  sj->sounding = sj->nseg > 0;
}

static void play_effect(sj_chip *sj, int code) {
  const int first = fxindex[code - 200][0], count = fxindex[code - 200][1];
  const double bend = bend_hz(sj);
  sj->nseg = 0;
  for (int i = 0; i < count; i++) {
    const fxpart *p = &fxparts[first + i];
    frame a = silent_frame(), b = silent_frame();
    for (int k = 0; k < 3; k++) {
      a.f[k] = bent(p->fa[k] * HZ_STEP, bend);
      b.f[k] = bent(p->fb[k] * HZ_STEP, bend);
      a.a[k] = b.a[k] = p->v[k];
    }
    a.nf = b.nf = (float)(p->nf * HZ_STEP);
    a.nw = b.nw = (float)(p->nw * HZ_STEP);
    a.na = b.na = p->nv;
    const int n = sj->nseg;
    add_segment(sj, TICKS(p->ms * MS_STEP * speed_factor(sj, true)), a, b, (p->flags & (FX_VOICED | FX_PULSED)) != 0);
    if (sj->nseg > n) {
      sj->seg[n].gate = TICKS(p->gate);
      sj->seg[n].flags = p->flags;
    }
  }
  start(sj, true);
}

static frame allo_frame(const allophone *a, bool second, double bend) {
  frame f;
  f.f[0] = bent((second ? a->g1 : a->f1) * HZ_STEP, bend);
  f.f[1] = bent((second ? a->g2 : a->f2) * HZ_STEP, bend);
  f.f[2] = bent((second ? a->g3 : a->f3) * HZ_STEP, bend);
  f.a[0] = a->a1;
  f.a[1] = a->a2;
  f.a[2] = a->a3;
  f.dc = a->dc;
  f.nf = (float)(a->nf * HZ_STEP);
  f.nw = (float)(a->nw * HZ_STEP);
  f.na = a->na;
  return f;
}

/* the segments of one copy of a Repeat: the sound's body for its own copy
 * length (about the manual's length for most sounds, not for the stops), no
 * transition (the copies run on seamlessly, measured on ZZ x10 and EYIY x5;
 * a stop's copies repeat its burst, BE x5 is 260 ms) */
static int copy_segments(sj_chip *sj, const allophone *a) {
  const double bend = bend_hz(sj);
  const int ticks = TICKS((a->cp * MS_STEP + T.copy_extra) * speed_factor(sj, false));
  const frame tgt = allo_frame(a, false, bend);
  sj->nseg = 0;
  if (a->kind == K_GLIDE)
    add_segment(sj, ticks, tgt, allo_frame(a, true, bend), true);
  else
    add_segment(sj, ticks, tgt, tgt, a->a1 + a->a2 + a->a3 > 0);
  return ticks;
}

/* expand an allophone into segments: its first play */
static void play_allophone(sj_chip *sj, const allophone *a) {
  const double bend = bend_hz(sj);
  const int ticks = TICKS(modified_ms(sj, a, a->ms * MS_STEP) * speed_factor(sj, false));
  const frame tgt = allo_frame(a, false, bend);
  sj->nseg = 0;
  switch (a->kind) {
  case K_VOWEL:
  case K_NASAL:
  case K_VFRIC: {
    int tr = TICKS(T.transition);
    if (tr > ticks)
      tr = ticks;
    add_segment(sj, tr, sj->last, tgt, true);
    add_segment(sj, ticks - tr, tgt, tgt, true);
    break;
  }
  case K_GLIDE: {
    const frame tgt2 = allo_frame(a, true, bend);
    int tr = TICKS(T.transition);
    if (tr > ticks / 2)
      tr = ticks / 2;
    add_segment(sj, tr, sj->last, tgt, true);
    add_segment(sj, ticks - tr, tgt, tgt2, true);
    break;
  }
  case K_FRIC:
    add_segment(sj, ticks, tgt, tgt, false);
    break;
  case K_STOP:
  case K_VSTOP: {
    /* the closure ("silence already added" says the manual; a voiced stop
     * hums through it on its first formant alone, quietly) then the burst and
     * release, which is the measured length */
    frame bar = silent_frame();
    if (a->kind == K_VSTOP) {
      bar.f[0] = tgt.f[0];
      bar.a[0] = tgt.a[0] / 4.0f;
    }
    add_segment(sj, TICKS(T.closure), bar, bar, a->kind == K_VSTOP);
    add_segment(sj, ticks, tgt, tgt, a->kind == K_VSTOP);
    break;
  }
  case K_AFFR: {
    /* a stop's closure, then the fricative (voiced for JH) */
    add_segment(sj, TICKS(T.closure), silent_frame(), silent_frame(), false);
    add_segment(sj, ticks, tgt, tgt, a->a1 + a->a2 + a->a3 > 0);
    break;
  }
  }
  start(sj, false);
  /* the copies of a Repeat follow on; the level ramps once, over the whole */
  sj->copies = sj->repeat > 1 ? sj->repeat - 1 : 0;
  sj->copy_of = (int)(a - allophones);
  if (sj->copies > 0) {
    const int nseg = sj->nseg, total = sj->stotal;
    segment saved[MAX_SEGMENTS];
    memcpy(saved, sj->seg, sizeof saved);
    sj->stotal = total + sj->copies * copy_segments(sj, a);
    memcpy(sj->seg, saved, sizeof saved);
    sj->nseg = nseg;
  }
}

static void play_pause(sj_chip *sj, double ms) {
  sj->nseg = 0;
  add_segment(sj, TICKS(ms), silent_frame(), silent_frame(), false);
  start(sj, false);
}

static int peek(const sj_chip *sj, int i) { return sj->buf[(sj->head + i) % SJ_BUFFER_SIZE]; }

/* take the next command from the buffer and start it. the command stays
 * counted in the buffer until it has finished */
static void next_command(sj_chip *sj) {
  while (sj->count > 0 && !sj->sounding) {
    const uint8_t code = (uint8_t)peek(sj, 0);
    const int n = sj_command_size(code);
    const uint8_t data = n == 2 ? (uint8_t)peek(sj, 1) : 0;
    sj->executing = n;
    if (code <= 6) {
      play_pause(sj, T.pause[code]);
    } else if (code >= 128 && code <= 199) {
      play_allophone(sj, &allophones[code - 128]);
      sj->repeat = 0;
      /* followed at once by another sound: the next one's start covers this
       * one's end, by more when both are voiced (measured on the pairs:
       * vowel to vowel 51 ms, with a hiss or a stop on either side 10 to 24) */
      if (sj->count > n && peek(sj, n) >= 128 && peek(sj, n) <= 254 && sj->nseg > 0) {
        segment *l = &sj->seg[sj->nseg - 1];
        const int next = peek(sj, n);
        const bool both = l->voiced && next <= 199 && allophones[next - 128].kind != K_FRIC && allophones[next - 128].kind != K_STOP;
        const int cut = TICKS(both ? T.overlap_v : T.overlap);
        if (l->ticks > cut + 1) {
          l->ticks -= cut;
          sj->stotal -= cut;
        }
      }
    } else if (code >= 200 && code <= 254) {
      play_effect(sj, code);
      sj->repeat = 0;
    } else {
      switch (code) {
      case 7: sj->fast = true; break;
      case 8: sj->slow = true; break;
      case 14: sj->stress = true; break;
      case 15: sj->relax = true; break;
      case 20: sj->volume = data > 127 ? 127 : data; break;
      case 21: sj->speed = data > 127 ? 127 : data; break;
      case 22: sj->pitch = data; break;
      case 23: sj->bend = data > 15 ? 15 : data; break;
      case 26: sj->repeat = data; break;
      case 30: play_pause(sj, data * 10.0); break;
      case 31: reset_controls(sj); break;
      default: break; /* 16 wait, 24/25 ports, 28/29 eeprom, 255: nothing */
      }
    }
    if (!sj->sounding) {
      /* the command is done at once */
      sj->head = (sj->head + n) % SJ_BUFFER_SIZE;
      sj->count -= n;
      sj->executing = 0;
    }
  }
}

/* the sound has ended: retire its command, clear the one-shot modifiers */
static void finish_sound(sj_chip *sj) {
  const int code = peek(sj, 0);
  if (code >= 128)
    sj->fast = sj->slow = sj->stress = sj->relax = false;
  sj->head = (sj->head + sj->executing) % SJ_BUFFER_SIZE;
  sj->count -= sj->executing;
  sj->executing = 0;
  sj->sounding = false;
}

/* --------------------------------------------------------- synthesizer ---- */

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static inline double rnd(sj_chip *sj) {
  sj->rnd = sj->rnd * 1664525u + 1013904223u;
  return (sj->rnd >> 8) * (1.0 / 16777216.0) - 0.5;
}

/* one chip sample */
static double tick(sj_chip *sj) {
  if (!sj->sounding)
    next_command(sj);
  if (!sj->sounding)
    return 0.0;

  const segment *s = &sj->seg[sj->cur];
  const double t = s->ticks > 1 ? (double)sj->tick / (s->ticks - 1) : 1.0;
  frame f;
  for (int k = 0; k < 3; k++) {
    const double fa = s->from.f[k], fb = s->to.f[k];
    if ((s->flags & FX_EXP) && fa > 0 && fb > 0)
      f.f[k] = (float)(fa * pow(fb / fa, t));
    else if (s->flags & FX_EXPDECAY)
      f.f[k] = (float)(fb + (fa - fb) * exp(-4.0 * t));
    else
      f.f[k] = (float)lerp(fa, fb, t);
    f.a[k] = (float)lerp(s->from.a[k], s->to.a[k], t);
  }
  f.dc = (float)lerp(s->from.dc, s->to.dc, t);
  f.nf = (float)lerp(s->from.nf, s->to.nf, t);
  f.nw = (float)lerp(s->from.nw, s->to.nw, t);
  f.na = (float)lerp(s->from.na, s->to.na, t);
  sj->last = f;

  double v = 0.0;

  /* voicing */
  if (s->voiced && sj->pitch > 0) {
    if (sj->ptick <= 0) {
      sj->period = (int)(RATE / sj->pitch) + 1; /* measured: 92 ticks at Pitch 88 on both units */
      sj->ptick = sj->period;
      if (!(s->flags & FX_PULSED)) /* pulsed tones run on; formants restart */
        sj->phase[0] = sj->phase[1] = sj->phase[2] = 0.0;
    }
    const int age = sj->period - sj->ptick;
    double env = age < T.env_rise ? (double)age / T.env_rise : 1.0 - (double)(age - T.env_rise) / T.env_fall;
    if (env < 0.0)
      env = 0.0;
    /* an effect's third tone is a plain tone beside the voice */
    const int nf = s->flags & FX_VOICED ? 2 : 3;
    /* the offset: measured on every voiced sound's pulse (both units), an
     * envelope-shaped term beside the sines, against their starting direction */
    double sum = -f.dc;
    for (int k = 0; k < nf; k++) {
      sum += f.a[k] * sin(2.0 * M_PI * sj->phase[k]);
      sj->phase[k] += f.f[k] * (1.0 + T.jitter * 2.0 * rnd(sj)) / NOMINAL;
    }
    /* mixer 1's limit: the manual says the three volumes must total 63 or
     * less; past it the sum clips */
    if (T.clip > 0)
      sum = sum > T.clip ? T.clip : sum < -T.clip ? -T.clip : sum;
    v += env * sum / 128.0 * (s->flags & FX_PULSED ? 0.6 : 1.0);
    if (nf == 2) {
      v += f.a[2] / 128.0 * 0.6 * sin(2.0 * M_PI * sj->phase[2]);
      sj->phase[2] += f.f[2] * (1.0 + T.jitter * 2.0 * rnd(sj)) / NOMINAL;
    }
    sj->ptick--;
  } else {
    sj->ptick = 0;
    /* plain tones (the effects) */
    for (int k = 0; k < 3; k++) {
      if (f.f[k] > 0.0f && f.a[k] > 0.0f)
        v += f.a[k] / 128.0 * 0.6 * sin(2.0 * M_PI * sj->phase[k]);
      sj->phase[k] += f.f[k] * (1.0 + T.jitter * 2.0 * rnd(sj)) / NOMINAL;
    }
  }

  /* noise: an oscillator whose frequency is thrown about by white noise every
   * sample, within +-nw of nf (the manual's register 6, "disordered with white
   * noise"). with a wide throw the band is flat with sharp edges; thrown past
   * the chip's Nyquist it folds back, which is the hiss sounds' peak at 4 kHz */
  if (f.na > 0.0f) {
    double nf = f.nf, lvl = 1.0;
    /* FX_NSWEEP: over each gate period the band climbs from 0.4 to 2 times
     * its centre and back, quieter at the top (measured on C8 and C9: 650 Hz
     * to 3 kHz and back every 20 ms with a 13 dB dip, both units; the gate's
     * off half scores better than a dip alone) */
    if ((s->flags & FX_NSWEEP) && s->gate > 0) {
      const double sw = sin(M_PI * (sj->tick % s->gate) / s->gate);
      nf *= 0.4 + 1.6 * sw;
      lvl = 1.0 - 0.75 * sw;
    }
    sj->nz[0] += (nf + f.nw * 2.0 * rnd(sj)) / NOMINAL;
    v += lvl * f.na / 128.0 * 0.7 * sin(2.0 * M_PI * sj->nz[0]);
  }

  if (s->gate > 0 && sj->tick % s->gate >= s->gate / 2)
    v = 0.0;
  if (s->flags & FX_DECAY)
    v *= pow(10.0, -T.decay_db * t / 20.0);
  if (s->flags & FX_BUMP)
    v *= sin(M_PI * t);

  /* the sound's attack and release */
  {
    const int att = sj->attack, rel = sj->release;
    if (sj->stick < att)
      v *= (double)sj->stick / att;
    if (sj->stotal - sj->stick < rel)
      v *= (double)(sj->stotal - sj->stick) / rel;
  }
  sj->stick++;

  v *= volume_gain(sj);
  /* the output stage's nonlinearity and its step size */
  v += T.dist2 * v * v + T.dist3 * v * v * v;
  if (T.quant > 0)
    v = floor(v * T.quant + 0.5) / T.quant;
  /* the output noise floor: white under the effects' tones, rising with
   * frequency under the voice (the valley between the first two formants is
   * deeper on the chip than a white floor leaves it) */
  {
    const double r = rnd(sj);
    v *= 1.0 + T.floor * 2.0 * (s->voiced && !(s->flags & (FX_VOICED | FX_PULSED)) ? r - sj->lastr : r);
    sj->lastr = r;
  }

  if (++sj->tick >= s->ticks) {
    sj->tick = 0;
    if (++sj->cur >= sj->nseg) {
      if (sj->copies > 0) {
        sj->copies--;
        copy_segments(sj, &allophones[sj->copy_of]);
        sj->cur = 0;
      } else
        finish_sound(sj);
    }
  }
  return v;
}

bool sj_render(sj_chip *sj, float *buf, size_t n) {
  const double step = RATE / SJ_SAMPLE_RATE;
  const double b0 = sj->lpf[0], b1 = sj->lpf[1], b2 = sj->lpf[2], a1 = sj->lpf[3], a2 = sj->lpf[4];
  bool sounding = sj->sounding || sj->count > 0;
  for (size_t i = 0; i < n; i++) {
    /* the average of the held chip samples over this output sample */
    double rest = step, sum = 0.0;
    while (sj->heldpos + rest >= 1.0) {
      sum += sj->held * (1.0 - sj->heldpos);
      rest -= 1.0 - sj->heldpos;
      sj->held = tick(sj);
      sj->heldpos = 0.0;
      sounding = sounding || sj->sounding || sj->count > 0;
    }
    sum += sj->held * rest;
    sj->heldpos += rest;
    const double x = sum / step;
    /* direct form II transposed */
    const double y = b0 * x + sj->z[0];
    sj->z[0] = b1 * x - a1 * y + sj->z[1];
    sj->z[1] = b2 * x - a2 * y;
    buf[i] = (float)y;
  }
  sj->speaking = sounding;
  return sounding;
}
