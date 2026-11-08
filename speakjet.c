/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * speakjet: an emulation of the SpeakJet speech and sound synthesizer used in
 * the AtariVox. second, from-scratch implementation: written
 * from the SpeakJet User's Manual and recordings of two AtariVoxes only, with
 * the smallest footprint that still matches them. procedural rules wherever a
 * rule was found; per-sound parameters are a few bytes each.
 *
 * Written by Claude, Anthropic's AI model, under the direction of Andrew Davie.
 *
 * What the chip was found to do (from the recordings):
 *   - it makes samples at about 8026 Hz (manual: 8192) and holds each one; the
 *     AtariVox then low-passes the held signal: two real poles, near 1.1 and
 *     2.4 kHz (the tones the chip sets directly, both units), and a real zero
 *     near 14 kHz (the images above 8 kHz). the library runs
 *     that analogue stage exactly on the held samples and samples it at 48 kHz,
 *     so the images the hold puts up to 20 kHz come out at the real thing's
 *     levels (a digital filter's warping and an average over each output
 *     sample left them 1-8 dB low above 5 kHz)
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

/* strict ISO C (-std=c99 on glibc) leaves these out of math.h */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- chip ---- */

/* the rate the chip makes samples at. measured from the images of its tones on
 * two units: 8039 and 8013 Hz (the manual says 8192). the chip's own
 * arithmetic assumes 8000: its tones come out Fs/8000 sharp on both units, and
 * the voicing period is floor(8000 / Pitch) + 1 ticks */
#define RATE 8026.0
#define NOMINAL 8000.0
#ifndef NHP_FC
#define NHP_FC 400.0 /* an allophone's noise high pass, Hz */
#endif
#ifndef NSWEEP_HPK
#define NSWEEP_HPK (1.0 / (1.0 + 2.0 * M_PI * 150.0 / 8000.0)) /* the swept noise's high pass (C8, C9): 150 Hz */
#endif
#ifndef JOIN_CLOSURE
#define JOIN_CLOSURE 45.0 /* a stop's closure after a sound, at least this, ms (Man Goes Down's GO) */
#define NASAL_BAR 0.15848931924611134 /* a voiced stop's bar after a nasal: 16 dB under its own level */
#define SLOW_GLIDE_ON 25.0 /* ms: see the ASSUMPTION at its use */
#define JOIN_A 66.9 /* a vowel or glide into a vowel or glide: cut JOIN_A - JOIN_K * (F1+F2 ERB distance) ms */
#define JOIN_K 6.9
#define NASAL_FADE 0.6513 /* and the nasal fades into it over this share of the closure */
#define NASAL_CLOSURE 38.0 /* a voiced stop's closure after a nasal, ms (before Speed and Slow) */
#define NASAL_RAMP 10.0 /* and its release rises over this, ms */
#define NASAL_RAMP_FROM 0.316228 /* from 10 dB down */
#endif
#ifndef NHP_TWO
#define NHP_TWO 1 /* two stages (1) or one (0) */
#endif
#define FRIC_RAMP 3.0 /* an unvoiced fricative's attack and release, ms */
#define THUMP_MS 16.0 /* a sibilant's starting burst of low noise: it fades out over this, ms */
#define FXBURST_MS 5.0 /* an effect's starting click: it fades out over this, ms (220 B0: the chip's is ~5 ms, flat to 1.5 kHz and beyond) */
#define THUMP_HZ 800.0 /* and is low-passed here */
#define NASAL_ATTACK 25.0 /* a nasal's attack, ms (the vowels': T.attack) */
#ifndef BAR_UP
#define BAR_UP 0.12 /* a voiced stop's bar: its upper formants at this fraction of the release's */
#endif
#ifndef BAR_CLICK
#define BAR_CLICK 0.0 /* and the click it opens on, ms */
#endif
#define AM_STEADY 0.006 /* a plain tone's level holds to 0.01-0.09 dB rms in its middle (VoxRecord, both units: the DTMF tones, B0, B3, R7, R9) */
#define AM_DEPTH 0.1 /* the effects' tones hold their pitch (both units: 0.03-0.13 % rms on the DTMF tones and the robots' carriers) */
#define AM_RATE 0.05 /* and the energy round their lines is a slow wobble in level, 10 % rms, spread to about 64 Hz */
#define SLEW 0.078 /* the effects' bed, measured: grows with the tone's frequency on both units (220 B0, 227 B7 against 215 A5) */
#define LPF_ZERO 14000.0 /* and one real zero, Hz: the images above 8 kHz (every effect, both units) */
#define BED_POLE 0.6909588478246612 /* up to about 1.5 kHz, then with the tone's level alone (one pole: the DTMF digits against 220 B0, 221 B1, 228 B8) */
#define SCP_DB 5.9 /* a register tone's level: VoxProbe 9, both units, the 1000 Hz tone at volume 31 stands 3.1-3.6 dB over 235 C5
                     * and 241 D1, which put it 4.9 and 6.9 dB above the effects' tone scale; the mean */
#define WANDER_DEPTH 0.015 /* 252 M0, both units: its line wanders 1.45-1.59 % rms */
#define WANDER_RATE 0.055 /* spread to about 70 Hz (a one-pole low-pass) */

#define TICKS(ms) ((int)((ms) * RATE / 1000.0 + 0.5))

/* the constants of the model. the values are what the recordings gave; a
 * build with -DSJ_TUNE reads overrides from the SJ_TUNE environment variable
 * ("ATTACK=35,FLOOR=0.06,...") so they can be fitted without recompiling */
static struct tune {
  double env_rise, env_fall;      /* voicing envelope: rise (under one tick: a step) and linear fall, ticks */
  double lpf_fc, lpf_q;           /* the output stage's two poles (both units: the 12 DTMF pairs' level ratios and VoxProbe 7's
                                   * direct tones at 125, 1000 and 3000 Hz, all 14 within 0.45 dB, rms 0.16) */
  double attack, release;         /* every sound's level ramp, ms */
  double fx_attack, fx_release;   /* the effects' (both units: a tone starts within 1-3 ms; its splatter is the bed round it) */
  double floor;                   /* the voiced output noise floor, relative. 0: the chip's voice is exactly
                                   * periodic (both units, every vowel and nasal at Pitch 88: the period holds
                                   * to one 48 kHz sample, and the gaps between harmonics sit 30-60 dB under
                                   * them, which a 0.05 floor filled to 15-30) */
  double dist2, dist3;            /* the output stage's nonlinearity: v + dist2 v^2 + dist3 v^3 */
  double jitter;                  /* the voiced oscillators' frequency wanders by this fraction each sample.
                                   * 0, measured with the floor: it smeared the same gaps */
  double clip;                    /* mixer 1 clips the formants' sum at this level (manual: 63; 0 = none) */
  double quant;                   /* the output's steps per full scale (0 = none, measured: the chip puts out its
                                   * quiet consonants and the stops' tails whole, far below a 48th of full scale); it was
                                   * coarse, about 5 bits across a vowel, which is the broadband hiss
                                   * under its voice */
  double decay_db;                /* an FX_DECAY part falls this many dB */
  double transition;              /* the voice moves to a new sound's target over this, ms */
  double closure;                 /* the closure before a stop's burst, ms */
  double gate_depth;              /* an effect's gate: its off half's level, a factor */
  double overlap, overlap_v;      /* a sound followed at once by another is cut short by this, ms; more when both are voiced */
  double speed_a, speed_b;        /* length factor (speed_a - Speed) / speed_b */
  double fx_speed_a, fx_speed_b;  /* the effects' (they shorten faster) */
  double fast, slow;              /* Fast and Slow: factors on the played length */
  double copy_extra;              /* a Repeat copy adds this to the manual's length, ms */
  double copy_closure;            /* a stop's Repeat copy opens on this much silence, ms */
  double bend_hz;                 /* Bend step, Hz */
  double pause[7];                /* the pauses, ms (VoxProbe 1 section 5, AW Pn AW x3, both units, 2600 and Stella;
                                   * Man Goes Down P1 and P2's gaps): P0 leaves ~20 ms, P3 P5 P6 run 52/30/31 ms under
                                   * the manual's 700 60 90, P1 P2 P4 are the manual's (AW's own fade-in and -out leave
                                   * every AW Pn AW gap ~19 ms under the chip's; the phrases' gaps between stops and
                                   * hisses match these) */
} T = {0.5, 58, 1609, 0.462, 35, 30, 1, 25, 0, 0, 0, 0, 0, 0, 35, 25, 30, 0.4, 15, 30, 162, 48, 147, 33, 2.0 / 3.0, 4.0 / 3.0, 0, 20, 44, {20, 90, 188, 648, 15, 30, 59}};
/* P2 188 ms, not 200 (2026-11-08, VoxProbe 13 section 9, both units, placement fixed: the second AW of AW P2 AW starts
 * 10-20 ms after the chips' at every Speed and Pitch; section 9 12.0 -> 9.4 ms) */

#ifdef SJ_TUNE
#include <stdio.h>
static void tune_from_env(void) {
  static const char *const names[] = {"ENVR", "ENVF", "LPFFC", "LPFQ", "ATTACK", "RELEASE", "FXATT", "FXREL", "FLOOR", "DIST2", "DIST3", "JITTER", "CLIP", "QUANT", "DECAY", "TRANS", "CLOSURE", "GATE", "OVERLAP", "OVERLAPV", "SPEEDA", "SPEEDB", "FXSPEEDA", "FXSPEEDB", "FAST", "SLOW", "COPY", "COPYCL", "BENDHZ", "P0", "P1", "P2", "P3", "P4", "P5", "P6"};
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
 * to 31) and one noise band (centre Hz, half width Hz, level 0 to 31) */
typedef struct frame {
  float f[3], a[3], lv; /* lv: the sound's level as a factor */
  float nf, nw, na;
} frame;

/* a sound is a few segments; each holds or glides to a frame for some ticks */
typedef struct segment {
  int ticks;
  frame from, to;
  bool voiced;
  int gate;      /* ticks of the on/off period for a pulsed effect, 0 = none */
  uint16_t flags; /* FX_* */
  float pa, pb;   /* an effect part's pulse rate: Pitch plus pa Hz at its start, pb at its end */
  float lr;       /* an effect part's level ramp: half-decibels gained (or lost) across the part, even in dB */
} segment;

/* the sound effects: each is a few parts. a part plays up to three tones
 * (plain sines; with FX_VOICED the first two are the voice's formants and the third stays a plain tone), a noise band, for
 * ms; the tones sweep from fa to fb (linearly, or FX_EXP geometrically, or
 * FX_EXPDECAY settling toward fb); FX_DECAY fades the level to nothing over
 * the part, FX_BUMP shapes it as a half sine; gate pulses it on and off;
 * FX_PULSED pulses the plain tones with the voice's envelope at Pitch */
enum { FX_VOICED = 1, FX_EXP = 2, FX_DECAY = 4, FX_BUMP = 8, FX_EXPDECAY = 16, FX_NSWEEP = 32, FX_PULSED = 64, FX_FADE = 128, FX_ALL = 256, FX_WANDER = 512, FX_SINE = 1024, FX_TRI = 2048, FX_SQUARE = 3072, FX_ONE = 4096, FX_BUZZ = 8192, FX_SMOOTH = 16384 };
#define FX_SHAPE 3072 /* the envelope oscillator's waveform (manual, register 8): saw (the voice's measured pulse), sine, triangle, square */
/* FX_ONE: only the first tone pulses, the other two are plain tones beside it (A0, both units: a voice at Pitch under
 * two smooth glides; register 8 envelopes oscillators 1-3 as a group, 4 and 5 run apart) */

/* the tables' units: frequencies in 16 Hz steps, lengths in 5 ms, so every
 * field is a byte */
#define HZ_STEP 16
#define MS_STEP 5

typedef struct fxpart {
  uint8_t ms;         /* MS_STEP */
  uint8_t fa[3], fb[3]; /* HZ_STEP */
  uint8_t v[3];
  uint8_t nf, nw;     /* HZ_STEP */
  uint8_t nv, gate;   /* gate in ms */
  uint16_t flags;
  int8_t lv;          /* the part's level, half-decibels (measured against the voice, both units) */
  int16_t pa, pb;     /* the part's pulse rate, Hz added to Pitch at its start and end (both units: the same offset at Pitch 60, 88 and 150) */
  int8_t lr;          /* the part's level ramp, half-decibels across it (both units: B1's tone falls and rises a steady
                       * 1.5 dB per 10 ms through its sweeps, the same up and down where the output stage alone can't) */
  uint8_t ve[3];      /* each tone's level at the part's end, 0 to 31 (both units: A7's lowest tone fades in by 27 dB while
                       * its highest fades out by 30, inside one sweep; the tones' volumes are registers swept like their
                       * frequencies) */
} fxpart;

#define MAX_SEGMENTS 32 /* C7 is seven chirps with gaps between */


struct sj_chip {
  /* the input buffer: a FIFO of raw bytes, as the chip's is */
  uint8_t buf[SJ_BUFFER_SIZE];
  int head, count;
  int executing; /* bytes of the command being executed, still counted */
  int pending_code;

  /* the controls */
  uint8_t volume, speed, pitch, bend, repeat;
  bool fast, slow, stress, relax;
  bool joined;        /* the last sound ran straight into the next: no ramps at that join */
  bool after_pause;    /* a pause code played since the last sound */
  bool last_nge;       /* the last sound played was NGE */
  bool after_velar;    /* the last sound played was GE */
  bool last_nasal;    /* the last sound played was a nasal, MM NE NO NGE NGO (not the liquids that share K_NASAL) */
  int ramp_seg;       /* a voiced stop after a nasal: its release ramp's segment (-1: none) */

  /* what is playing */
  segment seg[MAX_SEGMENTS];
  int nseg, cur, tick;
  int stick, stotal; /* ticks into the sound, and its whole length */
  bool effect;       /* the sound is an effect (its Speed law) */
  bool read;         /* the command after it has been read ahead */
  int ahead;         /* bytes of that command, executed early: retired with the sound */
  int attack, release;
  int onset;          /* the sound's louder onset: its length in ticks, where it starts (a stop's or an affricate's
                       * release, after the closure), and its gain */
  int onset_from;
  double onset_gain;
  double release_gain; /* a stop's release (from onset_from), relative */
  double wfloor;       /* an allophone's white floor under its noise band */
  double thump;        /* its starting burst of low noise, relative to the noise band */
  double thlp;         /* that burst's low-pass state */
  double thk;          /* and its low-pass coefficient */
  double nhk;          /* an allophone's noise high pass coefficient */
  double fxburst, fxbk, fxblp; /* an effect's onset burst (fxs): level, low-pass coefficient and state */
  double fxbms;        /* its length, ms (fxs tl; FXBURST_MS when 0) */
  double fxhpk;        /* the effect's own noise high pass (fxs hp), one stage; 0: none */
  int copies;        /* copies of a Repeat still to play */
  int copy_of;       /* which allophone */
  frame last; /* where the voice is, for transitions into the next sound */
  bool sounding;

  /* synthesizer state */
  int period, ptick;
  double phase[3];
  float xa[3]; /* FX_SMOOTH: the tone amplitudes as they ramp */
  float xv; /* FX_SMOOTH: the voice's gain as it ramps in */
  double vbuzz; double bzbuf[1024]; int bzpos, bzclk; /* FX_BUZZ: the source's recent past, for the comb; ticks to the next click */
  int neq; double eqc[3][5], eqz[3][2]; /* the playing effect's peaking filters (fxeq): b0 b1 b2 a1 a2, and their state */
  double nz[2]; /* the noise resonator's two delays */
  double nhp[4]; /* an allophone's noise high pass: input and output of each of its two stages */
  double venv; /* the voicing envelope this tick, for an FX_ALL effect's noise */
  double wander; /* an FX_WANDER part's slow pitch drift, a fraction */
  double vprev; /* the output's last tick */
  double bed; /* the output's change per tick, smoothed, for the effects' bed */
  double am; /* the effects' slow level wobble */
  uint32_t rnd;
  double lastr; /* the previous noise sample, for the floor's high pass */

  /* the serial control protocol (manual, SCP): "\0" enters it, then the synthesizer's registers are written
   * directly ("<address>J<value>N"); its commands act at once, never through the buffer */
  bool scp, esc;      /* in control mode; a "\\" seen, its node character next */
  uint32_t sval;      /* the SCP input buffer's decimal value so far */
  int slen;           /* and its characters (16 at most) */
  uint8_t mtype, maddr; /* the Memory Type and Memory Address control registers */
  uint16_t reg[16];   /* the synthesizer's registers as SCP last wrote them (0-5 Hz, 6 distortion, 8 envelope control,
                       * 11-15 volumes; 7, the master volume, is volume) */
  bool hold;          /* SCP "S": the MSA ignores the buffer until "T" */
  double sph[5];      /* the direct oscillators' phases */
  int eage;           /* ticks into the envelope oscillator's period */

  /* output */
  double held, heldpos;
  double z[2];
  double lpf[6]; /* the low pass: its two poles (per tick), their decays over an output sample, p2 / (p2 - p1), p2 / zero */
  bool speaking;
};

/* ------------------------------------------------------------- tables ---- */

/* the sound component database: per allophone, its kind, manual length and
 * targets. generated from params.json by tools/gen_tables.py; edit that */
enum { K_VOWEL, K_GLIDE, K_NASAL, K_VFRIC, K_FRIC, K_STOP, K_VSTOP, K_AFFR };

typedef struct allophone {
  uint8_t kind;
  uint8_t ms;         /* the length played at Speed 114, measured, MS_STEP */
  uint8_t f1, f2, f3; /* formants, HZ_STEP (a glide: f1, f2 = the two allophones it runs between) */
  uint8_t a1, a2, a3; /* their levels, 0 to 31 */
  uint8_t nf, nw;     /* the noise band's centre and bandwidth, HZ_STEP */
  uint8_t na;
  uint8_t cp;         /* the length of each further copy of a Repeat, MS_STEP */
  int8_t lv;          /* the sound's level, half-decibels (measured against the vowels, both units) */
  uint8_t cb;         /* a stop's Repeat copies keep its voice bar through their closures */
  uint8_t ob;         /* its onset: the first ob ms play og half-decibels louder (0: none) */
  uint8_t og;
  uint8_t cl;         /* a stop's closure, ms (0: the common T.closure) */
  int8_t rl;          /* a stop's release after its first pulse, half-decibels re its bar (0: as the table) */
  uint8_t wf;         /* a white floor under its noise band, /256 of the band's level (0: none) */
  uint8_t rr;         /* its own release (fade-out) length, ms (0: the common one) */
  uint8_t tc;         /* its starting burst's low-pass corner, Hz/32 (0: THUMP_HZ) */
  uint8_t th;         /* a low thump at its start: low noise under 800 Hz fading over 16 ms, /64 of the noise band's level (0: none) */
  uint8_t ba;         /* a voiced stop's bar fade-in from silence, ms (0: FRIC_RAMP) */
  int8_t bl;          /* the bar's level re the release, half-decibels (0: the same) */
  uint8_t aa;         /* its own attack (fade-in) length, ms (0: the class's) */
  uint8_t hp;         /* its noise high pass corner, Hz/16 (0: NHP_FC) */
  uint8_t nd;         /* a decaying release: the noise band's level at the end of its last segment, /256 (0: steady) */
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
  /* the output low pass: two real poles (Q under 0.5) per chip tick, their
   * decays over one output sample, and the zero */
  {
    const double w = 2.0 * M_PI * T.lpf_fc / RATE;
    const double h = 0.5 / (T.lpf_q < 0.499 ? T.lpf_q : 0.499), r = sqrt(h * h - 1.0);
    sj->lpf[0] = w * (h - r);
    sj->lpf[1] = w * (h + r);
    sj->lpf[2] = exp(-sj->lpf[0] * RATE / SJ_SAMPLE_RATE);
    sj->lpf[3] = exp(-sj->lpf[1] * RATE / SJ_SAMPLE_RATE);
    sj->lpf[4] = sj->lpf[1] / (sj->lpf[1] - sj->lpf[0]);
    sj->lpf[5] = sj->lpf[1] / (2.0 * M_PI * LPF_ZERO / RATE);
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

static bool scp_byte(sj_chip *sj, uint8_t c);

bool sj_write(sj_chip *sj, uint8_t code, uint8_t data) {
  if (sj_command_size(code) == 1 && scp_byte(sj, code))
    return true;
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
  if (sj->scp || sj->esc)
    return sj_write(sj, byte, 0); /* control mode's characters are single bytes */
  if (sj_command_size(byte) == 2) {
    sj->pending_code = byte;
    return true;
  }
  return sj_write(sj, byte, 0);
}

/* one serial byte for the SCP module, which watches every byte: true if it took the byte. Registers 0-5 take 0 to
 * 3999 Hz, 6 and 8 0 to 255, 7 (the master volume, the MSA's Volume) 0 to 127, 11-15 0 to 31. "V" (say "Ready") is
 * not played: the chip's phrase for it is unmeasured. Memory type 32 (the EEPROM) is ignored */
static bool scp_byte(sj_chip *sj, uint8_t c) {
  if (sj->esc) {
    sj->esc = false;
    if (c >= '0' && c <= '9') {
      /* node 0 is every chip's; the AtariVox's own node is 0 */
      sj->scp = c == '0';
      sj->sval = 0;
      sj->slen = 0;
      sj->mtype = sj->maddr = 0;
      return true;
    }
    if (sj->scp) {
      sj->scp = false; /* an escape and a non-numeric character leave control mode */
      return true;
    }
    return false; /* outside control mode the character goes on to the MSA */
  }
  if (c == '\\') {
    sj->esc = true;
    return true;
  }
  if (!sj->scp)
    return false;
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
    if (sj->slen < 16) {
      sj->slen++;
      if (c <= '9')
        sj->sval = sj->sval * 10 + (c - '0');
    }
    return true;
  }
  const uint32_t v = sj->sval;
  sj->sval = 0;
  sj->slen = 0;
  switch (c) {
  case 'H': sj->mtype = (uint8_t)v; break;
  case 'J': sj->maddr = (uint8_t)v; break;
  case 'N':
    if (sj->mtype == 0 && sj->maddr < 16) {
      const int a = sj->maddr;
      if (a == 7)
        sj->volume = v > 127 ? 127 : (uint8_t)v;
      else if (a <= 5)
        sj->reg[a] = (uint16_t)(v > 3999 ? 3999 : v);
      else if (a >= 11)
        sj->reg[a] = (uint16_t)(v > 31 ? 31 : v);
      else if (a == 6 || a == 8)
        sj->reg[a] = (uint16_t)(v > 255 ? 255 : v);
    }
    break;
  case 'R': /* clears the buffer: what is playing plays on */
    sj->count = sj->executing + sj->ahead;
    break;
  case 'S': sj->hold = true; break;
  case 'T': sj->hold = false; break;
  case 'W': sj_power_on(sj); break;
  case 'X': sj->scp = false; break;
  default: break;
  }
  return true;
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

/* the length of a sound at the current Speed, as a factor on its length at
 * Speed 114. linear: measured 95/197/287 ms for AW at Speed 127/90/60; the
 * effects change more: (147 - Speed) / 33 on every part (fitted on every effect
 * recording at a Speed other than 114; alone they read about (145 - Speed) / 31,
 * steeper still at low Speeds than near 114). Speech below Speed 114 follows
 * the effects' law (VoxProbe 12, both units: AY and IYUW x1.21 at Speed 108,
 * the effects' law x1.18, the old speech law x1.125; Man Goes Down P1 at
 * Speed 108 E 8.28 -> 6.01, P 1.78 -> 0.94); above 114 the speech law stays
 * (P2 at Speed 118 is worse on the effects' law) */
/* ASSUMPTION (2026-11-08, VoxProbe 13 sections 3 and 9 and Man Goes Down P2 at Speed 118, both units): above Speed 114
 * an unslowed vowel or glide (and its Repeat copy) and a pause shrink by (155.9 - Speed) / 47.4, a step down from 114
 * (118: x0.80, 127: x0.61, as measured; the effects' law gives x0.88 at 118), and the other speech sounds don't shrink
 * at all. P2 1700 ms against 1695/1700, timing inside it 12.9 -> 8.5 ms; with the step on the vowels alone the
 * consonants ran 15-40 ms short each. Below 114 nothing changes */
static double speed_step(const sj_chip *sj) { return (155.9 - sj->speed) / 47.4; }

static double speed_factor(const sj_chip *sj, bool effect) {
  if (effect || sj->speed < 114)
    return (T.fx_speed_a - sj->speed) / T.fx_speed_b;
  return (T.speed_a - sj->speed) / T.speed_b;
}

/* Fast and Slow take away or add a share of the manual's length (measured
 * half: AW +35, IH +40,
 * SE +20, EYIY +75 ms under Slow): the manual's "1/2" and "1 1/2 times" are
 * of its own length column */
static double modified_ms(const sj_chip *sj, const allophone *a, double ms) {
  /* Fast x2/3 and Slow x4/3 of the played length (VoxProbe 12 section 2,
   * 2026-11-05, at -20 dB, both units, 2600 and Stella: Fast AY IYUW x0.63-0.70,
   * Slow AY IYUW OWWW NE AW UX x1.27-1.36; was x0.77 and x1.22). The
   * unvoiced stops and affricates don't change (TT is 45-50 ms plain, fast or
   * slow); the voiced stops do under Slow (VoxProbe 12: Slow GO OWWW +12 ms) */
  if (a->kind == K_STOP || a->kind == K_AFFR || (a->kind == K_VSTOP && !sj->slow))
    return ms;
  if (a->kind == K_VSTOP)
    return ms * T.slow;
  if (sj->fast)
    ms *= T.fast;
  /* SE SO SH take half of Slow's stretch (2026-11-08, VoxProbe 13 section 10, all four takes: every slowed SE 16-34 ms
   * long at x4/3; VoxProbe 12 had Slow SE +20 ms; Man Goes Down P4's Slow SE Slow SE) */
  if (sj->slow)
    ms *= (a - allophones >= 59 && a - allophones <= 61) ? 1.0 + (T.slow - 1.0) / 2.0 : T.slow;
  return ms > 10 ? ms : 10;
}

/* Bend: every frequency moves by the same amount, 44 Hz a step (D1's 697
 * and 1209 Hz both step by 44). below 5 there is a 22 Hz jump */
static double bend_hz(const sj_chip *sj) {
  return T.bend_hz * (sj->bend - 5) + (sj->bend < 5 ? T.bend_hz / 2 : 0.0);
}

/* the library's own sin, exp and log for everything done per sample: libm's
 * last bits differ from one platform to another, and the output quantiser
 * turns a last bit on the wrong side of a step into a whole step. plain
 * series in doubles, the same to the bit everywhere the build keeps fused
 * multiply-add off. sj_sin2pi takes cycles */
static double sj_sin2pi(double x) {
  x -= floor(x + 0.5); /* [-0.5, 0.5) */
  if (x > 0.25)
    x = 0.5 - x;
  else if (x < -0.25)
    x = -0.5 - x; /* [-0.25, 0.25] */
  const double y = 2.0 * M_PI * x, y2 = y * y;
  double term = y, sum = y;
  for (int n = 1; n <= 9; n++) { /* to y^19: under 1e-15 at pi/2 */
    term *= -y2 / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

static double sj_exp(double x) {
  if (x < -700.0)
    return 0.0;
  const double k = floor(x / M_LN2 + 0.5), r = x - k * M_LN2; /* |r| <= ln2 / 2 */
  double term = 1.0, sum = 1.0;
  for (int n = 1; n <= 15; n++) {
    term *= r / n;
    sum += term;
  }
  return ldexp(sum, (int)k);
}

static double sj_log(double x) {
  int e;
  const double m = frexp(x, &e), s = (m - 1.0) / (m + 1.0), s2 = s * s; /* m in [0.5, 1): |s| <= 1/3 */
  double term = s, sum = s;
  for (int n = 1; n <= 15; n++) {
    term *= s2;
    sum += term / (2.0 * n + 1.0);
  }
  return 2.0 * sum + e * M_LN2;
}

/* the master volume: measured on AW, 16 to 127 is only 6 dB */
static double volume_gain(const sj_chip *sj) {
  /* measured on both units at Pitch 20 (VoxProbe 6): Volume 16 is 15 dB under
   * 32, and from 32 up the level barely moves */
  static const double db[5] = {-15.5, -1.3, -1.1, 0.0, 2.2};
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
  return sj_exp(d / 20.0 * M_LN10);
}

static float bent(double hz, double bend) { return hz > 0 ? (float)(hz + bend) : 0.0f; }

static frame silent_frame(void) {
  frame f;
  memset(&f, 0, sizeof f);
  f.lv = 1.0f;
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
  s->pa = s->pb = 0.0f;
  s->lr = 0.0f;
}

/* start playing the segments built so far */
static void start(sj_chip *sj, bool effect) {
  if (!effect)
    sj->neq = 0; /* an allophone has no peaking filters */
  sj->attack = TICKS(effect ? T.fx_attack : T.attack);
  sj->fxburst = 0.0;
  sj->fxbms = FXBURST_MS;
  sj->fxhpk = 0.0;
  sj->onset = 0;
  sj->onset_from = 0;
  sj->release_gain = 1.0;
  sj->wfloor = 0.0;
  sj->thump = 0.0;
  sj->thlp = 0.0;
  sj->release = TICKS(effect ? T.fx_release : T.release);
  sj->cur = sj->tick = sj->stick = 0;
  sj->copies = 0;
  sj->stotal = 0;
  for (int i = 0; i < sj->nseg; i++)
    sj->stotal += sj->seg[i].ticks;
  sj->sounding = sj->nseg > 0;
  /* the MSA drives the synthesizer itself and leaves the oscillator volumes at 0 (manual: its pauses ramp them out) */
  for (int k = 11; k <= 15; k++)
    sj->reg[k] = 0;
  sj->effect = effect;
  sj->read = false;
  sj->ahead = 0;
}

/* an effect part, read from the table's byte stream (fxdata, from fxat[code - 200]): its length, a byte saying
 * which fields follow (bits 0-1 the number of tones; 4 noise, 8 gate, 16 pulse offsets, 32 level ramp, 64 end
 * levels, 128 a high flags byte), its flags and level, then three bytes a tone and only the fields it uses */
static const uint8_t *fx_decode(const uint8_t *q, fxpart *f) {
  memset(f, 0, sizeof *f);
  f->ms = *q++;
  const uint8_t h = *q++;
  f->flags = *q++;
  if (h & 128)
    f->flags |= (uint16_t)(*q++ << 8);
  f->lv = (int8_t)*q++;
  const int n = h & 3;
  for (int k = 0; k < n; k++) {
    f->fa[k] = *q++;
    f->fb[k] = *q++;
    f->v[k] = *q++;
  }
  if (h & 4) {
    f->nf = *q++;
    f->nw = *q++;
    f->nv = *q++;
  }
  if (h & 8)
    f->gate = *q++;
  if (h & 16) {
    f->pa = (int16_t)(uint16_t)(q[0] | q[1] << 8);
    f->pb = (int16_t)(uint16_t)(q[2] | q[3] << 8);
    q += 4;
  }
  if (h & 32)
    f->lr = (int8_t)*q++;
  for (int k = 0; k < 3; k++)
    f->ve[k] = h & 64 && k < n ? q[k] : f->v[k];
  if (h & 64)
    q += n;
  return q;
}

static void play_effect(sj_chip *sj, int code) {
  sj->last_nasal = false;
  sj->after_pause = false;
  sj->after_velar = false;
  /* its peaking filters (fxeq, at most 3): a fixed shaping of its spectrum toward the chip's, set after its structure
   * was tuned (2026-11-04); RBJ peaking biquads at the chip's rate */
  sj->neq = 0;
  for (int i = 0; i < FXEQ_N && sj->neq < 3; i++) {
    if (fxeq[i][0] != code - 200)
      continue;
    const double f = fxeq[i][1] * HZ_STEP, g = (int8_t)fxeq[i][2] / 2.0, qq = fxeq[i][3] / 10.0;
    const double A = pow(10.0, g / 40.0), w = 2.0 * M_PI * f / RATE, al = sin(w) / (2.0 * qq), cw = cos(w), a0 = 1.0 + al / A;
    double *c = sj->eqc[sj->neq];
    c[0] = (1.0 + al * A) / a0;
    c[1] = -2.0 * cw / a0;
    c[2] = (1.0 - al * A) / a0;
    c[3] = -2.0 * cw / a0;
    c[4] = (1.0 - al / A) / a0;
    sj->eqz[sj->neq][0] = sj->eqz[sj->neq][1] = 0.0;
    sj->neq++;
  }
  const uint8_t *q = fxdata + fxat[code - 200], *const end = fxdata + fxat[code - 199];
  const double bend = bend_hz(sj);
  sj->nseg = 0;
  for (int i = 0; q < end; i++) {
    fxpart part;
    q = fx_decode(q, &part);
    (void)i; /* FX-PART-DECODED: the part's index, for the anneal's override */
    const fxpart *p = &part;
    frame a = silent_frame(), b = silent_frame();
    for (int k = 0; k < 3; k++) {
      a.f[k] = bent(p->fa[k] * HZ_STEP, bend);
      b.f[k] = bent(p->fb[k] * HZ_STEP, bend);
      a.a[k] = p->v[k];
      b.a[k] = p->ve[k];
    }
    /* D0 to D11 are the DTMF pairs, measured on both units within 3 Hz of
     * the standard once each unit's rate is allowed for (keypad order: D1 to
     * D9, then 0, * and #), at equal level; the table holds their length */
    if (code >= 240 && code <= 251) {
      static const double row[4] = {697, 770, 852, 941}, col[3] = {1209, 1336, 1477};
      const int d = code - 240, r = d == 0 || d > 9 ? 3 : (d - 1) / 3, c = d == 0 ? 1 : d == 10 ? 0 : d == 11 ? 2 : (d - 1) % 3;
      a.f[0] = b.f[0] = bent(row[r], bend);
      a.f[1] = b.f[1] = bent(col[c], bend);
      a.f[2] = b.f[2] = 0.0f;
      a.a[0] = b.a[0] = a.a[1] = b.a[1] = 31;
      a.a[2] = b.a[2] = 0;
    }
    a.lv = b.lv = (float)sj_exp(p->lv * (0.5 * M_LN10 / 20.0));
    a.nf = b.nf = (float)(p->nf * HZ_STEP);
    a.nw = b.nw = (float)(p->nw * HZ_STEP);
    a.na = b.na = p->nv;
    const int n = sj->nseg;
    add_segment(sj, TICKS(p->ms * MS_STEP * speed_factor(sj, true)), a, b, (p->flags & (FX_VOICED | FX_PULSED)) != 0);
    if (sj->nseg > n) {
      sj->seg[n].gate = TICKS(p->gate);
      sj->seg[n].flags = p->flags;
      sj->seg[n].pa = p->pa;
      sj->seg[n].pb = p->pb;
      sj->seg[n].lr = p->lr;
    }
  }
  start(sj, true);
  /* its own structure (fxs, the structure loop's): fade-in and fade-out lengths, and a burst of low noise at its start
   * (2026-11-06: the DTMF tones open on a broadband column ~15 ms long that the chip has and the library lacked) */
  const uint8_t *x = fxs[code - 200];
  if (x[0])
    sj->attack = TICKS(x[0]);
  if (x[1])
    sj->release = TICKS(x[1]);
  sj->fxburst = x[2] / 64.0; /* /64 since 2026-11-07 (was /256; values divided by four): 253 M1's 50 ms burst needs the range */
  sj->fxbk = 1.0 - exp(-2.0 * M_PI * (x[3] ? x[3] * 32.0 : THUMP_HZ) / 8000.0);
  sj->fxblp = 0.0;
  /* 253 M1 (2026-11-07, both units, every take): a low block 100-500 Hz for the first ~50 ms under a hiss that has little
   * under 500 Hz. An effect's noise is otherwise white to DC: its own high pass (hp, Hz/16, one stage) and its burst's own
   * length (tl, ms) */
  sj->fxhpk = x[4] ? 1.0 / (1.0 + 2.0 * M_PI * (x[4] * 16.0) / 8000.0) : 0.0;
  sj->fxbms = x[5] ? (double)x[5] : FXBURST_MS;
}

static frame allo_frame(const allophone *a, bool second, double bend) {
  frame f;
  /* a glide runs from one allophone's frame to another's, levels included
   * (measured on all 16: IYRR starts on IY and ends on RR); a glide's f1 and
   * f2 hold the two allophones' indices and it has no formants of its own */
  if (a->kind == K_GLIDE)
    return allo_frame(&allophones[second ? a->f2 : a->f1], false, bend);
  /* a voiced stop's release ends on formants of its own (allog: the few that have them) */
  const uint8_t *g = NULL;
  if (second)
    for (int i = 0; i < ALLOG_N; i++)
      if (allog[i][0] == a - allophones)
        g = &allog[i][1];
  f.f[0] = bent((g ? g[0] : a->f1) * HZ_STEP, bend);
  f.f[1] = bent((g ? g[1] : a->f2) * HZ_STEP, bend);
  f.f[2] = bent((g ? g[2] : a->f3) * HZ_STEP, bend);
  f.a[0] = a->a1;
  f.a[1] = a->a2;
  f.a[2] = a->a3;
  f.lv = (float)sj_exp(a->lv * (0.5 * M_LN10 / 20.0));
  f.nf = (float)(a->nf * HZ_STEP);
  f.nw = (float)(a->nw * HZ_STEP);
  f.na = a->na;
  return f;
}

/* a stop: the closure ("silence already added" says the manual; a voiced
 * stop hums through it on its first formant alone) then the burst and
 * release, which is the measured length. the closure: the sound before fades
 * away through it (both units: a vowel before a stop falls 30 dB over 50 to
 * 60 ms at Speed 114, twice that at Speed 60), not a cut to silence; a voiced
 * stop hums through it on its first formant */
static int next_sound(const sj_chip *sj, int i);
static bool slow_before_next(const sj_chip *sj, int i);
static int add_stop(sj_chip *sj, const allophone *a, frame from, int ticks, double bend) {
  const frame tgt = allo_frame(a, false, bend);
  frame bar = from, gone = silent_frame();
  if (a->kind == K_VSTOP) {
    gone.f[0] = tgt.f[0];
    gone.a[0] = tgt.a[0]; /* the first formant's level: both units' voiced stops open on a voice bar 2-9 dB under the release (BE, BO, DE, DO, GE, GO) */
    gone.lv = tgt.lv;     /* at the sound's own level (VoxProbe 6, slowed: DE's bar 3-4 dB under its release, BE's 8) */
    gone.lv = (float)(tgt.lv * pow(10.0, a->bl / 40.0)); /* or its own (the structure loop's bl) */
    /* and its upper formants, weak (both units, the bar alone: 6-10 dB more than F1's tail at 1.8-5 kHz, 12-24 on GE GO) */
    gone.f[1] = tgt.f[1];
    gone.f[2] = tgt.f[2];
    gone.a[1] = (float)(tgt.a[1] * BAR_UP);
    gone.a[2] = (float)(tgt.a[2] * BAR_UP);
  }
  /* from silence, a voiced stop's bar is there at once, at its level (both units, the VoxRecord solo takes lined up on
   * the release: BE BO DE DO GE GO's bar 2-11 dB under the peak through the closure, not a fade up from nothing) */
  if (a->kind == K_VSTOP && bar.a[0] + bar.a[1] + bar.a[2] + bar.na == 0.0f)
    bar = gone;
  /* each stop's own closure (both units, lined up on the release: BE BO DE DO's bar 50-55 ms, the final stops' 5-16);
   * those were measured from silence, where the closure is the voice bar. After a sound the chip closes for at least
   * JOIN_CLOSURE (2026-11-06, Man Goes Down's NE GO, both units: 30-40 ms at -27 to -37 dB before the burst; ours, with
   * GO's 8 ms, ran straight on and "goes" lost its /g/; P on every recording with a stop after a sound: MGD P1 1.946 ->
   * 1.625, EH DE 0.694 -> 0.194, AW TT 1.058 -> 1.023, MGD P2 0.615 -> 0.656, every other unchanged) */
  double clms = a->cl ? a->cl : T.closure;
  /* ASSUMPTION (2026-11-07, VoxProbe 12 DO AYWW, both units: 284 ms where DO's 44 ms closure gave 315; DO alone needs
   * the 44): a voiced stop from silence, not slowed, running into a sound with no Slow on it closes for at most 30 ms.
   * "man goes down"'s DO (Slow on its AYWW) keeps its closure, as the recordings want. Not after a pause code
   * (2026-11-08, Man Goes Down P2's TT P1 BE: BE 45 ms early, P2 timing 14.1 -> 12.7 ms) */
  if (a->kind == K_VSTOP && from.a[0] + from.a[1] + from.a[2] + from.na == 0.0f && next_sound(sj, sj->executing) >= 128 && !slow_before_next(sj, sj->executing) && !sj->slow && !sj->after_pause && clms > 30.0)
    clms = 30.0;
  if (from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f && clms < JOIN_CLOSURE)
    clms = JOIN_CLOSURE;
  /* ASSUMPTION (2026-11-08, VoxProbe 13 section 6, all four takes): after a sound DE DO close 25 ms longer and GE GO
   * 15 ms (AW EH LO MM NE NO NGE OW RR before DO were 15-59 ms short, before GO 4-58, before BO 0-11) */
  const long stop_id = a - allophones;
  const bool alveolar = stop_id == 46 || stop_id == 47, velar = stop_id == 50 || stop_id == 51;
  if (a->kind == K_VSTOP && from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f)
    clms += alveolar ? 25.0 : velar ? 15.0 : 0.0;
  /* a voiced stop after a nasal closes quietly, and Slow lengthens that closure (Man Goes Down's NE GO, VoxProbe 12
   * section 4, all four takes: the join falls to -38..-41 dB above 300 Hz, under -30 dB for 30-40 ms as sent and 20-30
   * without its Slow; ours stayed above -22 dB and "goes" lost its /g/. EH DE, a vowel into a voiced stop, has no such
   * gap on either unit. P1: E 6.01 -> 5.02, P 0.94 -> 0.53; P2 and EH DE unchanged) */
  /* ASSUMPTION (2026-11-08): an unvoiced stop straight after a pause code closes 30 ms longer, in silence, so it can't
   * be seen on a stop played alone (VoxProbe 13 UX P1 EK 22 ms short; Man Goes Down P4's UX P1 EK: P4 timing 9.7 -> 4.9
   * ms, length 910 -> 940 against 942/945) */
  if (a->kind == K_STOP && sj->after_pause)
    clms += 30.0;
  /* ASSUMPTION (2026-11-08, VoxProbe 13 NE KO OW, both units by picture: NE stops at 0.105 s and KO bursts at 0.15, 30 ms
   * after ours; NE TT and NE PO already land): KE and KO after a nasal close 30 ms longer */
  if ((a - allophones == 66 || a - allophones == 67) && sj->last_nasal && from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f)
    clms += 30.0;
  /* ASSUMPTION (2026-11-08): a slowed unvoiced stop after a sound closes 20 ms longer (VoxProbe 13 section 10, all four
   * takes: AW Slow TT AW and Slow AYWW Slow TT 60 ms short; Man Goes Down P2's UW Slow TT: P2 timing 17.0 -> 12.3) */
  if (a->kind == K_STOP && sj->slow && from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f && next_sound(sj, sj->executing) < 128)
    clms += 20.0; /* only before a pause (2026-11-08): before a sound the vowel ahead runs on instead (see the join) */
  const bool nasal_join = a->kind == K_VSTOP && sj->last_nasal && from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f;
  /* the gap's width, band by band on both units' Man Goes Down NE GO (2026-11-07): 20-32 ms under the row's peak
   * -45 dB at 700-3000 Hz, closing at 473-485 ms; 45 ms read 28-38 and closed 480-488 */
  if (nasal_join)
    clms = NASAL_CLOSURE;
  /* ASSUMPTION (2026-11-08, VoxProbe 13 section 6; Man Goes Down P1 1488 -> 1502 against 1495/1502): after a nasal
   * DE DO GE GO close 10 ms longer than BE BO; not under Slow (Man Goes Down P1's Slow GO: timing 18.3 -> 12.3 ms
   * without it) */
  if (nasal_join && (alveolar || velar) && !sj->slow)
    clms += 10.0;
  /* ASSUMPTION (2026-11-08, VoxProbe 13 section 6, all four takes: NGE before BO DO GO 38-48 ms short): after NGE the
   * closure is 40 ms longer */
  if (nasal_join && sj->last_nge)
    clms += 40.0;
  if (nasal_join && sj->slow)
    clms *= T.slow;
  const int closure = TICKS(clms * speed_factor(sj, false));
  /* from silence, the bar opens on a click: the release's full frame falling into the bar over BAR_CLICK ms (both units,
   * every voiced stop's first 10 ms 11-33 dB over the library's at 1-10 kHz) */
  int click = 0;
  if (a->kind == K_VSTOP && from.a[0] + from.a[1] + from.a[2] + from.na == 0.0f && BAR_CLICK > 0) {
    click = TICKS(BAR_CLICK);
    if (click > closure - 1)
      click = closure - 1;
    add_segment(sj, click, tgt, gone, true);
  }
  if (nasal_join) {
    frame quiet = gone;
    quiet.lv = (float)(gone.lv * NASAL_BAR);
    /* and the whole band dips evenly (both units: 300-700 Hz 22-25 ms under its peak -30 dB, where the bar's F1
     * hummed through; 1300-1700 at -38..-47 dB, where eight-fold F2 drew a line through the gap) */
    quiet.a[0] *= 0.300f;
    quiet.a[1] *= 2.500f;
    quiet.a[2] *= 2.500f;
    /* the nasal's F2 and F3 fall into the closure rather than rising to the stop's (both units, NE GO: F2 1640 -> 1510,
     * F3 2615 -> 2140 Hz; ours rose to GO's 1900 and 3200) */
    quiet.f[1] = bar.f[1] * 0.92f;
    quiet.f[2] = bar.f[2] * 0.82f;
    const int fade = (int)(closure * NASAL_FADE);
    add_segment(sj, fade, bar, quiet, true);
    add_segment(sj, closure - fade, quiet, quiet, true);
  } else if (a->kind == K_STOP && bar.a[0] + bar.a[1] + bar.a[2] + bar.na > 0.0f && closure > TICKS(10.0) + 1) {
    /* an unvoiced stop's closure is silent after a 10 ms fade (2026-11-08, VoxProbe 13, both units: every vowel, nasal
     * or hiss before TT KO PO stops and leaves a 20-40 ms gap before the burst; ours faded across the whole closure and
     * never went quiet, so the runs of sound didn't match the chips' on 11 items) */
    add_segment(sj, TICKS(10.0), bar, gone, bar.a[0] + bar.a[1] + bar.a[2] > 0.0f);
    add_segment(sj, closure - TICKS(10.0), gone, gone, false);
  } else
    add_segment(sj, closure - click, bar, gone, a->kind == K_VSTOP || bar.a[0] + bar.a[1] + bar.a[2] > 0.0f);
  /* the release glides to the stop's second frame: the first formant falls into a labial or alveolar closure
   * (both units: EB, OB, ED, OD); a stop without a measured target holds */
  /* after a nasal the release comes in over NASAL_RAMP from 10 dB down (both units: 5-15 dB under ours in its first
   * 10 ms, full by ~15 ms; ours started at full level with a 5 ms onset boost on top) */
  if (nasal_join && ticks > TICKS(NASAL_RAMP) + 1) {
    frame low = tgt;
    low.lv = (float)(tgt.lv * NASAL_RAMP_FROM);
    sj->ramp_seg = sj->nseg;
    add_segment(sj, TICKS(NASAL_RAMP), low, tgt, true);
    add_segment(sj, ticks - TICKS(NASAL_RAMP), tgt, allo_frame(a, true, bend), true);
  } else
    add_segment(sj, ticks, tgt, allo_frame(a, true, bend), a->kind == K_VSTOP);
  return closure + ticks;
}

/* the segments of one copy of a Repeat: the sound's body for its own copy
 * length (about the manual's length for most sounds), no transition (the
 * copies run on seamlessly, measured on ZZ x10 and EYIY x5). a stop's copy is
 * the stop again, closure and release (VoxProbe 4 and 7, both units: the 12
 * voiced stops repeated 2 to 5 times, each copy a gap then a burst) */
static int copy_segments(sj_chip *sj, const allophone *a) {
  const double bend = bend_hz(sj);
  const frame tgt = allo_frame(a, false, bend);
  sj->nseg = 0;
  const int ticks = TICKS((a->cp * MS_STEP + T.copy_extra) * ((a->kind == K_VOWEL || a->kind == K_GLIDE) && sj->speed > 114 && !sj->slow ? speed_step(sj) : speed_factor(sj, false)));
  if (a->kind == K_STOP || a->kind == K_VSTOP) {
    /* the copy opens on a closure, then the release; cp is the whole copy, measured from one copy's onset to the next.
     * the closure is silent (VoxProbe 4 and 7, both units: GE, GO and the final stops dip 27-35 dB between copies,
     * 33-50 dB in the voice bar's band) or, for BE, BO, DE and DO, holds the voice bar (they dip only about 22 dB) */
    int closure = TICKS(T.copy_closure * speed_factor(sj, false));
    if (closure > ticks - 1)
      closure = ticks - 1;
    frame gone = silent_frame();
    if (a->cb) {
      gone.f[0] = tgt.f[0];
      gone.a[0] = tgt.a[0];
      gone.lv = tgt.lv;
    }
    add_segment(sj, closure, gone, gone, a->cb != 0);
    add_segment(sj, ticks - closure, tgt, allo_frame(a, true, bend), a->kind == K_VSTOP);
    return ticks;
  }
  if (a->kind == K_GLIDE) {
    const frame tgt2 = allo_frame(a, true, bend);
    const int hold = ticks / 5, move = ticks * 7 / 10;
    /* the copy glides back from the last copy's end to the first frame (VoxProbe 4, both units: IYRR x5 rises
     * 1383 -> 2083 Hz over its first 60 ms each time, not a jump) */
    add_segment(sj, hold, tgt2, tgt, true);
    add_segment(sj, move, tgt, tgt2, true);
    add_segment(sj, ticks - hold - move, tgt2, tgt2, true);
  }
  else
    add_segment(sj, ticks, tgt, tgt, a->a1 + a->a2 + a->a3 > 0);
  return ticks;
}

/* the frame a sound's transition starts from: the last frame, or, after
 * silence, the sound's own frame at no level (the formants don't sweep up
 * from 0 Hz at the start of a phrase or after a pause) */
static frame from_frame(const sj_chip *sj, frame tgt) {
  /* after a hiss (noise only) a sound starts as from silence: its formants and noise band don't sweep from the hiss's
   * (2026-11-08, VoxProbe 13 SE TT AW and AW Slow SE Slow SE UX, both units: the hiss stops and the next sound starts
   * on its own frame; ours swept a dark line from ~6 kHz down into TT's closure and UX's onset) */
  if (sj->last.a[0] > 0 || sj->last.a[1] > 0 || sj->last.a[2] > 0)
    return sj->last;
  tgt.a[0] = tgt.a[1] = tgt.a[2] = 0.0f;
  tgt.na = 0.0f;
  return tgt;
}

static frame lerp_frame(frame a, frame b, float t) {
  for (int q = 0; q < 3; q++) {
    a.f[q] += (b.f[q] - a.f[q]) * t;
    a.a[q] += (b.a[q] - a.a[q]) * t;
  }
  a.lv += (b.lv - a.lv) * t;
  a.nf += (b.nf - a.nf) * t;
  a.nw += (b.nw - a.nw) * t;
  a.na += (b.na - a.na) * t;
  return a;
}

/* the sound's last at ticks bend F2 and F3 to (f2, f3), its levels and F1 as they are (split where the span starts) */
static void anticipate(sj_chip *sj, int at, float f2, float f3) {
  int total = 0;
  for (int k = 0; k < sj->nseg; k++)
    total += sj->seg[k].ticks;
  if (at <= 0 || at >= total - 1 || sj->nseg >= MAX_SEGMENTS)
    return;
  const int start = total - at;
  int acc = 0, k = 0;
  while (acc + sj->seg[k].ticks <= start)
    acc += sj->seg[k++].ticks;
  if (acc < start) {
    const segment sg = sj->seg[k];
    const int t1 = start - acc;
    const frame mid = lerp_frame(sg.from, sg.to, (float)t1 / (float)sg.ticks);
    memmove(&sj->seg[k + 2], &sj->seg[k + 1], (size_t)(sj->nseg - k - 1) * sizeof(segment));
    sj->seg[k].ticks = t1;
    sj->seg[k].to = mid;
    sj->seg[k + 1] = sg;
    sj->seg[k + 1].ticks = sg.ticks - t1;
    sj->seg[k + 1].from = mid;
    sj->nseg++;
    k++;
  }
  const float f2a = sj->seg[k].from.f[1], f3a = sj->seg[k].from.f[2];
  int el = 0;
  for (; k < sj->nseg; k++) {
    segment *g = &sj->seg[k];
    g->from.f[1] = f2a + (f2 - f2a) * (float)el / (float)at;
    g->from.f[2] = f3a + (f3 - f3a) * (float)el / (float)at;
    el += g->ticks;
    g->to.f[1] = f2a + (f2 - f2a) * (float)el / (float)at;
    g->to.f[2] = f3a + (f3 - f3a) * (float)el / (float)at;
  }
}

/* expand an allophone into segments: its first play */
static void play_allophone(sj_chip *sj, const allophone *a) {
  const double bend = bend_hz(sj);
  /* vowels and glides take the effects' Speed law above 114 too (2026-11-08, VoxProbe 13 section 3, all four takes:
   * AW AY EYIY IYUW OWWW UX alone at Speed 118, 122, 127 10-30 ms long on the speech law, within a few ms on this one;
   * Man Goes Down P2 at Speed 118 1698 ms against 1695/1700 with the liquid joins below) */
  int ticks = TICKS(modified_ms(sj, a, a->ms * MS_STEP) * ((a->kind == K_VOWEL || a->kind == K_GLIDE) ? (sj->speed > 114 && !sj->slow ? speed_step(sj) : speed_factor(sj, true)) : (sj->speed > 114 ? 1.0 : speed_factor(sj, false))));
  const frame tgt = allo_frame(a, false, bend), tgt0 = tgt;
  const frame from = from_frame(sj, tgt);
  sj->nseg = 0;
  sj->ramp_seg = -1;
  switch (a->kind) {
  case K_VOWEL:
  case K_NASAL:
  case K_VFRIC: {
    int tr = TICKS(T.transition);
    /* ASSUMPTION (2026-11-08, VoxProbe 13 section 7, all four takes: after GE's burst F2 glides from ~2000 Hz to the
     * vowel's over ~60 ms before OW; GE OW, GE UW 39-49 ms short, GE IY EH AY 15-20): a vowel after GE moves from GE's
     * frame over 8 ms more per ERB of F2 distance (10 put GE OW's end 10 ms late on the picture), and is that much longer */
    if (sj->after_velar && a->kind == K_VOWEL) {
      const double ea = 21.4 * sj_log(1.0 + 0.00437 * from.f[1]) / M_LN10, eb = 21.4 * sj_log(1.0 + 0.00437 * tgt.f[1]) / M_LN10;
      const int ex = TICKS(8.0 * (ea > eb ? ea - eb : eb - ea) * speed_factor(sj, true));
      tr += ex;
      ticks += ex;
    }
    if (tr > ticks)
      tr = ticks;
    /* a sound with an onset, from silence, starts on its own frame (its onset at full level) */
    if (a->ob && from.a[0] + from.a[1] + from.a[2] + from.na == 0.0f)
      tr = 0;
    add_segment(sj, tr, from, tgt, true);
    add_segment(sj, ticks - tr, tgt, tgt, true);
    break;
  }
  case K_GLIDE: {
    /* measured on the chip's glides: the first frame holds for a fifth of the
     * sound, the move takes the next seven tenths, the second frame holds the
     * last tenth */
    const frame tgt2 = allo_frame(a, true, bend);
    /* a glide's ba (unused by glides otherwise): its first frame's F1 in percent (2026-11-06, 161 AXUW, both units: F1
     * held ~640 Hz through the middle, 20 % over AX's own) */
    frame tgt = tgt0;
    if (a->ba)
      tgt.f[0] = tgt0.f[0] * (float)a->ba / 100.0f;
    const int hold = ticks / 5, move = ticks * 7 / 10;
    int tr = TICKS(T.transition);
    if (tr > hold)
      tr = hold;
    /* with its own F1 (ba) the hold starts on the first allophone's and rises to it (161 AXUW: 520 -> 640 Hz) */
    add_segment(sj, tr, from, tgt0, true);
    add_segment(sj, hold - tr, tgt0, tgt, true);
    /* a glide's cl (unused by glides otherwise): its F1 holds until cl percent of the sound, then moves with the rest
     * to the end of the move (2026-11-06, 161 AXUW, both units: F2 falls throughout while F1 stays open until ~130 ms
     * and closes in the last 60 ms; the other glides move together) */
    if (a->cl > 0 && (int)(a->cl * ticks / 100) > hold && (int)(a->cl * ticks / 100) < hold + move) {
      const int k1 = a->cl * ticks / 100 - hold;
      const float fr = (float)k1 / (float)move;
      frame m = tgt;
      for (int q = 0; q < 3; q++) {
        m.f[q] = tgt.f[q] + (tgt2.f[q] - tgt.f[q]) * fr;
        m.a[q] = tgt.a[q] + (tgt2.a[q] - tgt.a[q]) * fr;
      }
      m.lv = tgt.lv + (tgt2.lv - tgt.lv) * fr;
      m.nf = tgt.nf + (tgt2.nf - tgt.nf) * fr;
      m.nw = tgt.nw + (tgt2.nw - tgt.nw) * fr;
      m.na = tgt.na + (tgt2.na - tgt.na) * fr;
      m.f[0] = tgt.f[0];
      add_segment(sj, k1, tgt, m, true);
      add_segment(sj, move - k1, m, tgt2, true);
    } else
      add_segment(sj, move, tgt, tgt2, true);
    add_segment(sj, ticks - hold - move, tgt2, tgt2, true);
    break;
  }
  case K_FRIC:
    /* ASSUMPTION (2026-11-08, VoxProbe 13 section 10, all four takes: TH between a vowel and a sound ~75 ms on the chip,
     * ~40 here; AW TH AW, IHWW TH RR 25-39 ms short): TH after a voiced sound and before another plays 25 ms longer.
     * Not before a pause (Man Goes Down P2's Slow TH P4: timing 12.7 -> 16.0 ms with it) */
    if (a - allophones == 62 && from.a[0] + from.a[1] + from.a[2] + from.na > 0.0f && next_sound(sj, sj->executing) >= 128)
      ticks += TICKS(25.0 * speed_factor(sj, false));
    add_segment(sj, ticks, tgt, tgt, false);
    break;
  case K_STOP:
  case K_VSTOP:
    add_stop(sj, a, from, ticks, bend);
    break;
  case K_AFFR: {
    /* a stop's closure, then the fricative (voiced for JH) */
    add_segment(sj, TICKS(T.closure), silent_frame(), silent_frame(), false);
    add_segment(sj, ticks, tgt, tgt, a->a1 + a->a2 + a->a3 > 0);
    break;
  }
  }
  start(sj, false);
  sj->last_nasal = a - allophones >= 12 && a - allophones <= 16;
  sj->last_nge = a - allophones == 15;
  sj->after_velar = a - allophones == 50;
  sj->after_pause = false;
  const bool nasal_ramp = sj->ramp_seg >= 0;
  /* an unvoiced fricative starts and stops at once (HE HO WH FF SE SO SH TH, both units, background allowed for: rise
   * and fall 0-4 ms, median 2, where the vowels' 35 ms ramp gave 22-38) */
  if (a->kind == K_FRIC)
    sj->attack = sj->release = TICKS(FRIC_RAMP);
  if (a->kind == K_VSTOP && from.a[0] + from.a[1] + from.a[2] + from.na == 0.0f)
    sj->attack = TICKS(FRIC_RAMP); /* its bar starts at once too */
  if (a->kind == K_NASAL)
    sj->attack = TICKS(NASAL_ATTACK); /* a nasal rises faster than a vowel (both units, lined up at -20 dB: full in 15 ms, not 25) */
  /* a louder onset (VoxRecord solo, both units: the voiced stops' and fricatives' first 1-8 ms, 3-19 dB over the rest) */
  sj->onset = TICKS(a->ob);
  sj->onset_gain = pow(10.0, a->og / 40.0);
  if (nasal_ramp)
    sj->onset = 0; /* its ramp replaces the onset boost */
  if (sj->onset > 0)
    sj->attack = 0; /* a sound with an onset starts at once, the onset at full level */
  if (a->kind == K_VSTOP && a->ba && from.a[0] + from.a[1] + from.a[2] + from.na == 0.0f)
    sj->attack = TICKS(a->ba); /* the bar fades in (both units: 175 DO's bar quiet for its first ~30 ms) */
  if (a->aa)
    sj->attack = TICKS(a->aa); /* a sound's own fade-in (the structure loop's aa: e.g. 185 WH starting ~5 ms early) */
  if ((a->kind == K_STOP || a->kind == K_VSTOP || a->kind == K_AFFR) && sj->nseg > 1)
    for (int k = 0; k < sj->nseg - 1; k++)
      sj->onset_from += sj->seg[k].ticks; /* the release: everything before its segment */
  /* a stop's release level of its own (both units, lined up on the release: DE's and DO's voicing after the first pulse
   * 7-11 dB under the library's, their bars and bursts as the library's) */
  if (a->rl && sj->onset_from > 0)
    sj->release_gain = pow(10.0, a->rl / 40.0);
  /* a white floor under the band (WH, both units: noise 30 dB under its 700 Hz band out to 12 kHz) */
  sj->wfloor = a->wf / 256.0;
  sj->nhk = 1.0 / (1.0 + 2.0 * M_PI * (a->hp ? a->hp * 16.0 : NHP_FC) / 8000.0); /* its own corner (PE, PO: their noise's bottom end) */
  /* a sibilant starts on a burst of low noise (both units, every take: SE SO SH 12-18 dB over their body's 100-700 Hz
   * band at the onset, a column under 1 kHz about 15 ms long) */
  /* no starting burst straight after a hiss (2026-11-08, VoxProbe 13 AW SE SE and AW Slow SE Slow SE UX, both units: the
   * second SE has no low column; ours had a dark blob under 1 kHz there, Man Goes Down P4's too) */
  sj->thump = (sj->last.na > 0.0f && sj->last.a[0] + sj->last.a[1] + sj->last.a[2] == 0.0f) ? 0.0 : a->th / 64.0; /* /64 since 2026-11-07 (was /256): SE's thump was pinned at the byte's top and the chip's is
                             * twice as loud (every figure better at x2); the stored values were divided by four */
  sj->thk = 1.0 - exp(-2.0 * M_PI * (a->tc ? a->tc * 32.0 : THUMP_HZ) / 8000.0);
  /* a release that decays: the noise band ends its last segment at nd/256 of its level (TS, both units, every take:
   * -13 dB over its 95 ms, the low band holding - the white floor - while the band falls) */
  if (a->nd && sj->nseg > 0)
    sj->seg[sj->nseg - 1].to.na *= a->nd / 256.0f;
  if (a->rr)
    sj->release = TICKS(a->rr); /* a sound's own fade-out (SE, SO: the chip fades over most of the sound) */
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
  sj->after_pause = true;
}

static int peek(const sj_chip *sj, int i) { return sj->buf[(sj->head + i) % SJ_BUFFER_SIZE]; }

/* the next sound in the buffer after offset i, looking past the modifiers and
 * settings that sit between sounds (Fast, Slow, Stress, Relax, Pitch, Speed,
 * Volume, Bend, Repeat): a phrase from text puts a pitch or a stress before
 * most sounds. a pause, a delay, a reset or the end of the buffer means the
 * sound is not followed at once: -1 */
static int next_sound(const sj_chip *sj, int i) {
  while (i < sj->count) {
    const int c = peek(sj, i);
    if (c >= 128 && c <= 254)
      return c;
    if (c == 7 || c == 8 || c == 14 || c == 15 || c == 16)
      i += 1;
    else if (c == 20 || c == 21 || c == 22 || c == 23 || c == 26)
      i += 2;
    else
      return -1;
  }
  return -1;
}

/* a Slow among the modifiers between offset i and the next sound */
static bool slow_before_next(const sj_chip *sj, int i) {
  while (i < sj->count) {
    const int c = peek(sj, i);
    if (c >= 128)
      return false;
    if (c == 8)
      return true;
    if (c == 7 || c == 14 || c == 15 || c == 16)
      i += 1;
    else if (c == 20 || c == 21 || c == 22 || c == 23 || c == 26)
      i += 2;
    else
      return false;
  }
  return false;
}

/* the sound's last span ticks taken off, across its segments */
static void trim_tail(sj_chip *sj, int span) {
  while (span > 0 && sj->nseg > 1) {
    segment *g = &sj->seg[sj->nseg - 1];
    if (g->ticks > span + 1) {
      g->ticks -= span;
      sj->stotal -= span;
      return;
    }
    span -= g->ticks;
    sj->stotal -= g->ticks;
    sj->nseg--;
  }
}

static bool slow_before_next(const sj_chip *sj, int i);
/* take the next command from the buffer and start it. the command stays
 * counted in the buffer until it has finished */
static void next_command(sj_chip *sj) {
  while (sj->count > 0 && !sj->sounding && !sj->hold) {
    const uint8_t code = (uint8_t)peek(sj, 0);
    const int n = sj_command_size(code);
    const uint8_t data = n == 2 ? (uint8_t)peek(sj, 1) : 0;
    sj->executing = n;
    if (code <= 6) {
      play_pause(sj, T.pause[code] * (sj->speed > 114 ? speed_step(sj) : (T.fx_speed_a - sj->speed) / T.fx_speed_b));
      sj->joined = false;
    } else if (code >= 128 && code <= 199) {
      /* a sound played straight after the same sound (joined, controls between allowed) plays as a Repeat copy of
       * it (VoxProbe 12, 2026-11-05, both units, 2600 and Stella: AY AY 208 ms, AW AW AW 255, UX UX UX 260 - what
       * Repeat 2 / 3 makes, 214 / 268 / 259 - not two whole sounds, 272 / 316 / 329; a Pitch or Speed between the
       * copies doesn't change it: AW Pitch 88 AW Pitch 88 AW 256) */
      /* a Slow on the second breaks it: two whole sounds (2026-11-08, VoxProbe 13 section 10, all four takes: AW Slow SE
       * Slow SE UX 464 ms, 435 as a copy) */
      const bool again = sj->joined && sj->copy_of == code - 128 && sj->repeat <= 1 && !sj->slow;
      const bool prev_nasal = sj->last_nasal;
      const bool prev_kind_vowel = sj->last.a[0] > 0.0f && !sj->last_nasal;
      play_allophone(sj, &allophones[code - 128]);
      if (again) {
        sj->stotal = copy_segments(sj, &allophones[code - 128]);
        sj->cur = 0;
        sj->tick = 0;
      }
      sj->repeat = 0;
      /* the level ramps were measured on sounds played alone. inside a
       * phrase the chip's level never drops more than 14-30 dB between
       * sounds (Man Goes Down, both units) where ramping every sound out and
       * in drops it 40-65 dB: a sound that follows straight on starts without
       * its attack, and one followed straight on ends without its release */
      if (sj->joined)
        sj->attack = 0;
      sj->joined = false;
      /* followed at once by another sound: the next one's start covers this
       * one's end, by more when both are voiced (measured on the pairs:
       * vowel to vowel 51 ms, with a hiss or a stop on either side 10 to 24) */
      const int next = next_sound(sj, n);
      /* ASSUMPTION (2026-11-07, Man Goes Down P4 only, both units: its Slow IYUW ends ~40-50 ms after ours; the same
       * Slow IYUW alone matches, VoxProbe 12): a slowed glide followed by modifiers that include a Slow, before the next
       * sound, plays SLOW_GLIDE_ON ms longer. P4 944 ms on the chip, 875 -> 925 here; the cause is unknown. 2026-11-08,
       * VoxProbe 13 Slow IYUW Slow SE, both units, by picture: IYUW ends ~20 ms after ours without the rule and the SE
       * after it is right; with Slow IYUW SE (no Slow after) it already matches. 25 ms (P4 940 against 942/945) */
      if (allophones[code - 128].kind == K_GLIDE && sj->slow && next >= 128 && slow_before_next(sj, n) && sj->nseg > 0) {
        sj->seg[sj->nseg - 1].ticks += TICKS(SLOW_GLIDE_ON * speed_factor(sj, false));
        sj->stotal += TICKS(SLOW_GLIDE_ON * speed_factor(sj, false));
      }
      /* a voiced sound straight into SE bends F2 and F3 to ZZ's, SE's voiced partner, over its last 40 ms (Man Goes
       * Down P4, both units: IYUW's F2 falls to ~1090 Hz and rises to ~1500 before the hiss; ZZ's F2 is 1469) */
      if (next == 187 && sj->nseg > 0 && sj->seg[sj->nseg - 1].voiced) {
        const allophone *zz = &allophones[167 - 128];
        anticipate(sj, TICKS(40.0 * speed_factor(sj, false)), bent(zz->f2 * HZ_STEP, bend_hz(sj)), bent(zz->f3 * HZ_STEP, bend_hz(sj)));
      }
      /* GO followed at once by a sound starting on OW (OW OWRR OWIY OWWW) releases on F2 1430 and F3 1920 Hz, F3 the
       * stronger by 4-5 dB (Man Goes Down's GO OWWW, both units, the user's drawn lines: two level bands that then split
       * into OW's F2 and F3); alone it keeps its table formants, which its solo takes need (moving them there: spectrum
       * 2.28 -> 4.12). Only before OW until another context is recorded (VoxProbe 13 section 7) */
      if (code == 179 && (next == 137 || next == 153 || next == 156 || next == 164) && sj->nseg > 0) {
        segment *r = &sj->seg[sj->nseg - 1];
        const double bh = bend_hz(sj);
        r->from.f[1] = r->to.f[1] = bent(1430.0, bh);
        r->from.f[2] = r->to.f[2] = bent(1920.0, bh);
        r->from.a[1] = r->to.a[1] = r->to.a[1] * 0.5f;
        r->from.a[2] = r->to.a[2] * 6.0f;
        r->to.a[2] = r->from.a[2];
        if (sj->ramp_seg >= 0 && sj->ramp_seg < sj->nseg - 1) {
          segment *q = &sj->seg[sj->ramp_seg];
          q->from.f[1] = q->to.f[1] = r->from.f[1];
          q->from.f[2] = q->to.f[2] = r->from.f[2];
          q->from.a[1] = q->to.a[1] = r->from.a[1];
          q->from.a[2] = q->to.a[2] = r->from.a[2];
        }
      }
      if (next >= 0 && sj->nseg > 0) {
        segment *l = &sj->seg[sj->nseg - 1];
        /* ASSUMPTION (2026-11-08, VoxProbe 13 AW Slow TT AW, both units by picture: aspiration ~50 ms where ours had ~25,
         * the AW after it 30-40 ms later than ours; Slow TT alone doesn't change, NE Slow TT IHWW is already long): a
         * slowed unvoiced stop after a sound other than a nasal, followed at once by a voiced sound, releases 40 ms
         * longer */
        /* ASSUMPTION (2026-11-08, VoxProbe 13 section 6, both units, the runs of sound): an unslowed NE or NO straight
         * into GO or GE runs on 18 ms, and 25 more when the GO is slowed (the Slow read ahead); not with a Fast on the GO.
         * NE GO OW's NE 90 -> 110 against 110-120, NO GO 100 -> 115 against 112, NE Slow GO 90 -> 135 against 130, NE Fast
         * GO 90 against 90. Man Goes Down P1's NE is slowed and keeps its length (fitted to the drawn lines) */
        if ((code == 141 || code == 142) && (next == 178 || next == 179) && !sj->slow) {
          bool fast_next = false;
          for (int i = n; i < sj->count && peek(sj, i) < 128; i += (peek(sj, i) >= 20 && peek(sj, i) <= 26) ? 2 : 1)
            if (peek(sj, i) == 7)
              fast_next = true;
          if (!fast_next) {
            const double ex = 18.0 + (slow_before_next(sj, n) ? 25.0 : 0.0);
            l->ticks += TICKS(ex);
            sj->stotal += TICKS(ex);
          }
        }
        /* ASSUMPTION (2026-11-08, VoxProbe 13 AW Slow TT AW and Slow AYWW Slow TT, both units, the runs of sound: the
         * sound before a slowed unvoiced stop runs on ~40 ms - the Slow is read ahead - unless it is a nasal or the stop
         * is followed by a pause (Man Goes Down P2's UW Slow TT P1): AW 130 -> 170 ms against 170) */
        if (next <= 199 && allophones[next - 128].kind == K_STOP && slow_before_next(sj, n) && allophones[code - 128].kind != K_STOP && !(code >= 140 && code <= 144)) {
          int j = n;
          while (j < sj->count && peek(sj, j) < 128)
            j += (peek(sj, j) >= 20 && peek(sj, j) <= 26) ? 2 : 1;
          if (!(j + 1 < sj->count && peek(sj, j + 1) <= 6)) {
            l->ticks += TICKS(40.0);
            sj->stotal += TICKS(40.0);
          }
        }
        if (allophones[code - 128].kind == K_STOP && sj->slow && !prev_nasal && next <= 199 && allophones[next - 128].kind != K_STOP && allophones[next - 128].kind != K_FRIC) {
          l->ticks += TICKS(15.0); /* 15, not 40, now the vowel ahead runs on: the release after it 185 ms against 185 */
          sj->stotal += TICKS(15.0);
        }
        /* a stop on either side takes the short overlap (the comment's own "a stop on either side"; the code only looked
         * at the next sound, so GO lost 51 ms of its release to OWWW and turned 15-25 ms early) */
        const int kc = allophones[code - 128].kind;
        const bool both = l->voiced && kc != K_STOP && kc != K_VSTOP && next <= 199 && allophones[next - 128].kind != K_FRIC && allophones[next - 128].kind != K_STOP;
        /* no overlap before the same sound again: it plays as a copy, which runs on from this one as a Repeat's do */
        /* no overlap after an unvoiced stop: its burst and aspiration play whole (the TT AW pairs, VoxProbe 4, 8, 9, both
         * units: 166-176 ms where ours read 145; 160 with the whole release) */
        int cut = (next == code || kc == K_STOP) ? 0 : TICKS(both ? T.overlap_v : T.overlap);
        /* ASSUMPTIONS (2026-11-08, VoxProbe 13, both units, the runs of sound against the silent closure): no overlap from a
         * vowel, glide or hiss into TT (AW 130 -> 145 against 152, SE 80 -> 95 against 102), nor from NE or NO into KE KO
         * PE PO BE BO (NE 90 -> 105 against 105-110); PO into a voiced sound after a vowel overlaps 20 ms (AW PO AW's
         * release 195 -> 175 against 175) */
        if (next == 191 && !(code >= 140 && code <= 144) && !slow_before_next(sj, n))
          cut = 0;
        if ((code == 141 || code == 142) && (next == 194 || next == 195 || next == 198 || next == 199 || next == 170 || next == 171))
          cut = 0;
        if (code == 199 && prev_kind_vowel && next <= 199 && allophones[next - 128].kind != K_STOP && allophones[next - 128].kind != K_FRIC)
          cut = TICKS(20.0);
        /* ASSUMPTION (2026-11-08, VoxProbe 13 section 10, all four takes: AW TH AW, IHWW TH RR 25-54 ms short): no
         * overlap into TH (Man Goes Down P2's IH TH lines up with it) */
        if (next == 190)
          cut = 0;
        /* ASSUMPTION (2026-11-08, VoxProbe 13 section 5, all four takes: IY MM, MM IY, IY NE, NE IY 26-38 ms short):
         * a nasal (MM NE NO NGE NGO) and a vowel or glide join with no overlap, unless either is slowed (Man Goes Down
         * P1's three nasal joins are all slowed and keep it: P1 1502 against 1495/1502). Vowels only: Man Goes Down P2's MM
         * EYIY, a glide, keeps it (P2 timing 29.7 -> 18.4 ms) */
        if (both && next <= 199 && !sj->slow && !slow_before_next(sj, n)) {
          const int kn2 = allophones[next - 128].kind;
          if ((code >= 140 && code <= 144 && kn2 == K_VOWEL) || (next >= 140 && next <= 144 && kc == K_VOWEL))
            cut = 0;
        }
        /* ASSUMPTION (2026-11-07, fitted on VoxProbe 4's vowel pairs, both units, 2600 and Stella; replaces the fixed
         * 30 ms for these joins, an earlier finding these recordings don't support): a vowel or glide running into a
         * vowel or glide is cut by JOIN_A - JOIN_K * d ms, d the ERB distance of F1 and F2 between its last frame and the
         * next sound's; below zero it runs on by that much (the chip takes longer between sounds further apart: AW IY
         * and IY AW 261/260 ms where the fixed cut gave 215, AW UW 204 where it gave 212). Nasals, liquids and voiced
         * hisses keep the fixed cut: their table formants aren't a vowel position, and the rule put "man goes down" and
         * Man Goes Down P2 51-145 ms long */
        const int kn = next <= 199 ? allophones[next - 128].kind : -1;
        /* the liquids LE LO WW RR join vowels by the same rule (2026-11-08, Man Goes Down P2's IY WW IH 30 ms short: P2
         * timing 23.4 -> 17.0 ms, 1665 -> 1702; VoxProbe 13 section 5 10.2 -> 8.6) */
        if (both && next != code && (kc == K_VOWEL || kc == K_GLIDE || (code >= 145 && code <= 148)) && (kn == K_VOWEL || kn == K_GLIDE || (next >= 145 && next <= 148))) {
          const allophone *b = &allophones[next - 128];
          if (b->kind == K_GLIDE)
            b = &allophones[b->f1]; /* a glide's f1 f2 name the allophones it runs between: it starts on the first's */
          const double bf[2] = {b->f1 * HZ_STEP, b->f2 * HZ_STEP};
          double d = 0.0;
          for (int q = 0; q < 2; q++) {
            const double ea = 21.4 * sj_log(1.0 + 0.00437 * l->to.f[q]) / M_LN10, eb = 21.4 * sj_log(1.0 + 0.00437 * bf[q]) / M_LN10;
            d += ea > eb ? ea - eb : eb - ea;
          }
          const double ms = (JOIN_A - JOIN_K * d) * speed_factor(sj, false);
          if (ms >= 0.0)
            cut = TICKS(ms);
          else {
            cut = 0;
            l->ticks += TICKS(-ms);
            sj->stotal += TICKS(-ms);
          }
        }
        /* ASSUMPTION (2026-11-07, every recorded voiced-stop pair, both units): a voiced stop into a vowel or glide with
         * no Slow on it loses 30 - 10 * d ms more off its end, d the ERB distance between its release F2 and the
         * vowel's (BE EH 168 ms where we gave 198, DO AYWW 284 / 315; BE AW and GO OWWW, far apart, already matched) */
        if (kc == K_VSTOP && code != 178 && code != 179 && next != code && (kn == K_VOWEL || kn == K_GLIDE)) {
          const allophone *b = &allophones[next - 128];
          if (b->kind == K_GLIDE)
            b = &allophones[b->f1];
          const double ea = 21.4 * sj_log(1.0 + 0.00437 * l->to.f[1]) / M_LN10, eb = 21.4 * sj_log(1.0 + 0.00437 * (b->f2 * HZ_STEP)) / M_LN10;
          const double d = ea > eb ? ea - eb : eb - ea, extra = 30.0 - 10.0 * d;
          if (extra > 0.0) {
            trim_tail(sj, cut + TICKS(extra * speed_factor(sj, false)));
            l = &sj->seg[sj->nseg - 1];
            cut = 0;
          } else if (code != 171) {
            /* not after BO (section 6's X BO OW 9.9 -> 9.6 ms mean) */
            /* ASSUMPTION (2026-11-08, VoxProbe 13 section 7, both units by picture: the burst lands at the same time on
             * every row and the vowel after it runs longer when F2 has further to go; BE OW 25 ms short -> 0, DO IY
             * 30 -> 25; section 7 10.9 -> 9.2 ms, section 6 11.3 -> 10.7): below zero the rule lengthens the release */
            l->ticks += TICKS(-extra * speed_factor(sj, false));
            sj->stotal += TICKS(-extra * speed_factor(sj, false));
          }
        }
        if (cut > 0 && l->ticks > cut + 1) {
          l->ticks -= cut;
          sj->stotal -= cut;
        }
        sj->release = 0;
        sj->joined = true;
      }
    } else if (code >= 200 && code <= 254) {
      play_effect(sj, code);
      sj->repeat = 0;
      sj->joined = false;
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
  sj->head = (sj->head + sj->executing + sj->ahead) % SJ_BUFFER_SIZE;
  sj->count -= sj->executing + sj->ahead;
  sj->executing = 0;
  sj->ahead = 0;
  sj->sounding = false;
}

/* --------------------------------------------------------- synthesizer ---- */

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static inline double rnd(sj_chip *sj) {
  sj->rnd = sj->rnd * 1664525u + 1013904223u;
  return (sj->rnd >> 8) * (1.0 / 16777216.0) - 0.5;
}

/* one chip sample */
/* the chip reads the command after a sound while the sound is still playing,
 * at its release: an effect's closing fade, or the last READ_AHEAD_MS (VoxProbe
 * 8, both units: "Speed v, D1, Speed 114" fades at Speed 114's length whatever
 * v; "Volume 16, IY, Volume 96" jumps to Volume 96 in IY's last 25-30 ms;
 * VoxProbe 7 and 4: a Pitch after a sound doesn't move it, and only the one
 * command right after is read). A Speed or Volume read so is carried out at
 * once: the rest of the sound plays at the new rate and level */
#define READ_AHEAD_MS 30
static void read_ahead(sj_chip *sj) {
  sj->read = true;
  const int i = sj->executing;
  if (i + 2 > sj->count)
    return;
  const int code = peek(sj, i), data = peek(sj, i + 1);
  if (code == 20)
    sj->volume = data > 127 ? 127 : data;
  else if (code == 21) {
    const double before = speed_factor(sj, sj->effect);
    sj->speed = data > 127 ? 127 : data;
    const double scale = speed_factor(sj, sj->effect) / before;
    int total = 0;
    for (int k = 0; k < sj->nseg; k++) {
      segment *g = &sj->seg[k];
      if (k == sj->cur)
        g->ticks = sj->tick + 1 + (int)((g->ticks - sj->tick - 1) * scale + 0.5);
      else if (k > sj->cur)
        g->ticks = g->ticks * scale < 1.0 ? 1 : (int)(g->ticks * scale + 0.5);
      total += g->ticks;
    }
    sj->stotal = total;
  } else
    return;
  sj->ahead = 2;
}

/* the master volume and the output stage: the voice (an allophone's voicing) gets the floor, everything else the bed */
static double output_stage(sj_chip *sj, double v, bool voice) {
  v *= volume_gain(sj);
  /* the output stage's nonlinearity and its step size */
  v += T.dist2 * v * v + T.dist3 * v * v * v;
  if (T.quant > 0)
    v = floor(v * T.quant + 0.5) / T.quant;
  /* the output noise floor: white under the effects' tones; none under the
   * voice (T.floor 0, the chip's voice is exactly periodic) */
  {
    const double r = rnd(sj);
    if (voice)
      v *= 1.0 + T.floor * 2.0 * (r - sj->lastr);
    else {
      /* the effects' bed (both units): timing error, proportional to the
       * signal's change per tick smoothed by one pole - it grows with the
       * tone's frequency to about 1.5 kHz and only with its level above */
      /* the buzz's clicks are kept out of the slope (each one is a huge step: the bed turned 227 B7's bands into loud noise) */
      sj->bed += BED_POLE * (v - sj->vbuzz - sj->vprev - sj->bed);
      v += SLEW * 2.0 * r * sj->bed;
    }
    sj->lastr = r;
    sj->vprev = v - sj->vbuzz; /* without the buzz: unchanged for every sound without one */
    sj->vbuzz = 0.0;
  }
  return v;
}

/* the synthesizer as SCP set it, between the MSA's sounds: five sines at registers 1-5 and 11-15; register 8's bits
 * 0-1 the envelope's shape (as the effects': saw is the voice's pulse), bit 6 envelopes oscillators 1-3, bit 7 half
 * envelopes 4 and 5, and no bed (the effects' own); the envelope runs at register 0 Hz, ceil(8000 / rate) ticks (the effects' rule); register 6
 * spreads 4 and 5 into noise round their frequencies. Unmeasured: the distortion's law (here a random frequency
 * error of up to distortion / 255 a tick), and the envelope at 0 Hz (here silent) */
static bool scp_audible(const sj_chip *sj) {
  return sj->volume > 0 && (sj->reg[11] | sj->reg[12] | sj->reg[13] | sj->reg[14] | sj->reg[15]) != 0;
}

static double scp_tick(sj_chip *sj) {
  const int ctl = sj->reg[8], rate = sj->reg[0];
  double env = 1.0;
  if (ctl & 0xc0) {
    if (rate <= 0)
      env = 0.0;
    else {
      int period = (int)ceil(8000.0 / rate);
      switch (rate) {
      case 44: case 51: case 53: case 56: case 63: case 77: case 87: case 88: case 92: case 104: case 127: case 151:
        period++;
      }
      if (sj->eage >= period)
        sj->eage = 0;
      if (sj->eage == 0 && (ctl & 0x40))
        for (int k = 0; k < 3; k++)
          sj->sph[k] = 0.0; /* the enveloped tones restart with each pulse, as the voice's */
      const int age = sj->eage++;
      const double x = (double)age / period;
      switch (ctl & 3) {
      case 1: env = 0.5 + 0.5 * sj_sin2pi(x + 0.25); break;
      case 2: env = x; break;
      case 3: env = x < 0.5 ? 1.0 : 0.0; break;
      default:
        env = age < T.env_rise ? (double)age / T.env_rise : 1.0 - (double)(age - T.env_rise) / T.env_fall;
        if (env < 0.0)
          env = 0.0;
      }
    }
  }
  double s13 = 0.0, s45 = 0.0;
  for (int k = 0; k < 5; k++) {
    const double a = sj->reg[11 + k], f = sj->reg[1 + k];
    if (a > 0.0 && f > 0.0) {
      if (k < 3)
        s13 += a * sj_sin2pi(sj->sph[k]);
      else
        s45 += a * sj_sin2pi(sj->sph[k]);
    }
    double step = f / NOMINAL;
    if (k >= 3 && sj->reg[6] > 0)
      step *= 1.0 + sj->reg[6] / 255.0 * 2.0 * rnd(sj);
    sj->sph[k] += step;
    sj->sph[k] -= floor(sj->sph[k]);
  }
  /* mixer 1's limit of 63 */
  if (T.clip > 0)
    s13 = s13 > T.clip ? T.clip : s13 < -T.clip ? -T.clip : s13;
  double v = 0.0;
  if (ctl & 0x40)
    v -= env * s13 / 128.0; /* enveloped, as the voice: the pulse starts downward */
  else
    v += s13 / 128.0 * 0.6; /* plain, as the effects' tones */
  v += (ctl & 0x80 ? 0.5 * env + 0.5 : 1.0) * s45 / 128.0 * 0.6;
  v *= 1.9724227361148539; /* SCP_DB as a factor, written out so every compiler gets the same */
  return output_stage(sj, v, true); /* no bed: under a register tone it is 57 dB down (VoxProbe 10, all three takes) */
}

static double tick(sj_chip *sj) {
  if (!sj->sounding)
    next_command(sj);
  if (!sj->sounding)
    return scp_audible(sj) ? scp_tick(sj) : 0.0;
  if (!sj->read && sj->copies == 0 && peek(sj, 0) >= 128) {   /* sounds only: no recording shows a pause reading ahead */
    const bool fade = (sj->seg[sj->nseg - 1].flags & FX_FADE) != 0;
    if (fade ? sj->cur == sj->nseg - 1 : sj->stotal - sj->stick <= TICKS(READ_AHEAD_MS))
      read_ahead(sj);
  }

  const segment *s = &sj->seg[sj->cur];
  const double t = s->ticks > 1 ? (double)sj->tick / (s->ticks - 1) : 1.0;
  frame f;
  for (int k = 0; k < 3; k++) {
    const double fa = s->from.f[k], fb = s->to.f[k];
    if ((s->flags & FX_EXP) && fa > 0 && fb > 0)
      f.f[k] = (float)(fa * sj_exp(t * sj_log(fb / fa)));
    else if (s->flags & FX_EXPDECAY)
      f.f[k] = (float)(fb + (fa - fb) * sj_exp(-4.0 * t));
    else
      f.f[k] = (float)lerp(fa, fb, t);
    f.a[k] = (float)lerp(s->from.a[k], s->to.a[k], t);
  }
  f.lv = (float)lerp(s->from.lv, s->to.lv, t);
  if (s->lr != 0.0f)
    f.lv *= (float)sj_exp(s->lr * t * (0.5 * M_LN10 / 20.0));
  f.nf = (float)lerp(s->from.nf, s->to.nf, t);
  f.nw = (float)lerp(s->from.nw, s->to.nw, t);
  f.na = (float)lerp(s->from.na, s->to.na, t);
  /* FX_SMOOTH: a tone switching on from silence or off to silence ramps over 3 ms (24 ticks) instead of stepping at a
   * part boundary (a step is a broadband click: 202 R2, 218 A8 show none on the chip). Per sound: 233 C3's tones do
   * switch sharply on the chip, and one ramp for every sound cost the library P 4.721 -> 4.757 */
  if (s->flags & FX_SMOOTH) {
    if (sj->stick == 0)
      for (int k = 0; k < 3; k++) sj->xa[k] = f.a[k];
    for (int k = 0; k < 3; k++) {
      const bool onoff = f.a[k] < 0.5f || sj->xa[k] < 0.5f || (sj->xa[k] < f.a[k] * 0.5f && sj->xa[k] < 2.0f);
      sj->xa[k] = onoff ? sj->xa[k] + (f.a[k] - sj->xa[k]) / 24.0f : f.a[k];
      f.a[k] = sj->xa[k];
    }
  } else
    for (int k = 0; k < 3; k++) sj->xa[k] = f.a[k];
  sj->last = f;

  double v = 0.0;

  /* voicing */
  /* FX_SMOOTH: where voicing starts after unvoiced sound, the voice comes in over 3 ms (24 ticks) instead of at once (a
   * pulse switching on is a broadband click: 235 C5, 202 R2) */
  {
    const float want = s->voiced && sj->pitch > 0 ? 1.0f : 0.0f;
    if (!(s->flags & FX_SMOOTH) || sj->stick == 0)
      sj->xv = want;
    else
      sj->xv += want > sj->xv ? (want - sj->xv) / 24.0f : want - sj->xv;
  }
  if (s->voiced && sj->pitch > 0) {
    if (sj->ptick <= 0) {
      /* the period in ticks, from the chip's own nominal 8000: the voice runs
       * floor(8000 / Pitch) + 1 (VoxProbe 2; VoxProbe 9: 41 at Pitch 200, 67 at
       * 120), an effect's pulse ceil(8000 / rate), one tick shorter wherever
       * 8000 / rate is whole (VoxProbe 9, both units: C5 at 80, 67, 54, 50, 40,
       * 32 ticks for Pitch 100, 120, 150, 160, 200, 250). an effect's pulses run
       * at Pitch plus the part's offset (C1, C2, C3, C6, C7: the same offset at
       * Pitch 60, 88 and 150 on both units, gliding within the part) */
      /* the offset from integers and one division, so every compiler gets the same period */
      double rate = sj->pitch + s->pa + (s->ticks > 1 ? (double)((s->pb - s->pa) * sj->tick) / (s->ticks - 1) : s->pb - s->pa);
      if (rate < 10.0)
        rate = 10.0;
      /* the voice counts a 16-bit phase up by floor(rate * 65536 / 8000) a tick: floor(65536 / step) + 1 ticks
       * (VoxProbe 2's Pitch sweep 20-255, both units: every Pitch to measuring precision, where floor(8000 / Pitch) + 1
       * missed 27; it is why the reset default, Pitch 88, runs 92 ticks) */
      const int step = (int)(rate * 65536.0 / 8000.0);
      sj->period = s->flags & (FX_VOICED | FX_PULSED) ? (int)ceil(8000.0 / rate) : 65536 / step + 1;
      /* an effect's pulse: one tick more at twelve rates (VoxProbe 10's register sweep, both units) */
      if ((s->flags & (FX_VOICED | FX_PULSED)) && rate == (int)rate)
        switch ((int)rate) {
        case 44: case 51: case 53: case 56: case 63: case 77: case 87: case 88: case 92: case 104: case 127: case 151:
          sj->period++;
        }
      sj->ptick = sj->period;
    }
    /* an effect's third tone is a plain tone beside the voice */
    const int nf = !(s->flags & FX_VOICED) || (s->flags & FX_ALL) ? 3 : s->flags & FX_ONE ? 1 : 2; /* FX_ALL: every tone pulses with the voice */
    if (sj->ptick == sj->period && !(s->flags & FX_PULSED)) /* pulsed tones run on; the enveloped ones restart, a plain tone beside them runs on */
      for (int k = 0; k < nf; k++)
        sj->phase[k] = 0.0;
    const int age = sj->period - sj->ptick;
    double env;
    const double x = (double)age / sj->period;
    switch (s->flags & FX_SHAPE) {
    case FX_SINE: env = 0.5 + 0.5 * sj_sin2pi(x + 0.25); break;
    case FX_TRI: env = x; break; /* VoxProbe 7, both units: a rising ramp, then straight back to nothing */
    case FX_SQUARE: env = x < 0.5 ? 1.0 : 0.0; break;
    default:
      env = age < T.env_rise ? (double)age / T.env_rise : 1.0 - (double)(age - T.env_rise) / T.env_fall;
      if (env < 0.0)
        env = 0.0;
    }
    /* the offset: measured on every voiced sound's pulse (both units), an
     * envelope-shaped term beside the sines, against their starting direction */
    double sum = 0.0;
    for (int k = 0; k < nf; k++) {
      sum += f.a[k] * sj_sin2pi(sj->phase[k]);
      sj->phase[k] += f.f[k] * (1.0 + T.jitter * 2.0 * rnd(sj)) / NOMINAL;
    }
    /* mixer 1's limit: the manual says the three volumes must total 63 or
     * less; past it the sum clips */
    if (T.clip > 0)
      sum = sum > T.clip ? T.clip : sum < -T.clip ? -T.clip : sum;
    v -= sj->xv * env * sum / 128.0 * f.lv * (s->flags & FX_PULSED ? 0.6 : 1.0); /* the pulse starts downward (both units, every vowel at Pitch 20) */
    sj->venv = env;
    for (int k = nf; k < 3; k++) {
      v += f.a[k] / 128.0 * 0.6 * f.lv * sj_sin2pi(sj->phase[k]);
      sj->phase[k] += f.f[k] * (1.0 + T.jitter * 2.0 * rnd(sj)) / NOMINAL;
    }
    sj->ptick--;
  } else {
    sj->ptick = 0;
    sj->venv = 1.0;
    /* FX_WANDER: 252 M0's tone drifts about +-1.5 % over tens of ms on both units: white noise through a one-pole low-pass */
    sj->wander += (WANDER_DEPTH / (0.57735 * sqrt(WANDER_RATE / 2.0)) * rnd(sj) - sj->wander) * WANDER_RATE; /* the input scaled so the drift's rms is WANDER_DEPTH */
    sj->am += (AM_DEPTH / (0.57735 * sqrt(AM_RATE / 2.0)) * rnd(sj) - sj->am) * AM_RATE;
    /* plain tones (the effects) */
    for (int k = 0; k < 3; k++) {
      if (f.f[k] > 0.0f && f.a[k] > 0.0f)
        v += f.a[k] / 128.0 * 0.6 * f.lv * sj_sin2pi(sj->phase[k]);
      sj->phase[k] += f.f[k] * (1.0 + (s->flags & FX_WANDER ? sj->wander : 0.0)) / NOMINAL;
    }
    v *= 1.0 + sj->am * (s->flags & FX_WANDER ? 1.0 : AM_STEADY / AM_DEPTH); /* the full wobble only on a wandering part (M0) */
  }

  if (f.na > 0.0f) {
    double nf = f.nf, lvl = 1.0;
    /* FX_NSWEEP: over each gate period the band climbs from 0.4 to 2 times
     * its centre and back, quieter at the top (measured on C8 and C9: 650 Hz
     * to 3 kHz and back every 20 ms with a 13 dB dip, both units; the gate's
     * off half scores better than a dip alone) */
    if ((s->flags & FX_NSWEEP) && s->gate > 0) {
      const double p = (double)(sj->tick % s->gate) / s->gate, sw = p < 0.4 ? p / 0.4 : 1.0 - (p - 0.4) / 0.6;
      nf *= sj_exp((2.0 * sw - 1.0) * 0.75); /* both units, C8 and C9: up in 8 ms, down in 12, by 2.1x either side of the centre (2026-11-06: the chip spans ~650 Hz - 2.4 kHz) */
      lvl = sj_exp(-5.0 * sw / 20.0 * M_LN10); /* and 5 dB quieter at the top, beyond what the output stage takes (2026-11-06: 15 dB, the chip's own figure, scored worse on spectrum, E and P) */
    }
    /* white noise through a two-pole resonance at nf with bandwidth nw (the
     * chip's hiss is random noise - Gaussian to the sample, both units - not
     * a tone; FF is flat to 4 kHz, the others resonate from WH's 800 Hz to
     * SE's 4 kHz). the gain holds the
     * output power at the input's whatever the band (the AR(2) variance) */
    const double r = sj_exp(-M_PI * (f.nw > 10.0f ? f.nw : 10.0) / RATE), c = sj_sin2pi(nf / RATE + 0.25);
    const double r2 = r * r, g = sqrt((1.0 - r2) * ((1.0 + r2) * (1.0 + r2) - 4.0 * r2 * c * c) / (1.0 + r2));
    /* FX_BUZZ: the noise minus itself one period of the part's pulse rate ago (Pitch plus its offset, ceil(8000 /
     * rate) ticks) at 0.5: notches every rate Hz in an otherwise flat noise, the power kept (232 C2's late burst, unit B:
     * broadband heat dimming just under every ~485 Hz; an impulse train made bright lines with dark gaps instead) */
    double ex = rnd(sj);
    if (s->flags & FX_BUZZ) {
      /* clicks, not noise: an impulse every voice period (Pitch), so the column is an even broadband colour; the comb
       * below then cuts narrow notches every rate Hz (user, C2: plain orange with narrow, regular purple stripes) */
      const int vper = sj->pitch > 0 ? (int)(8000.0 / sj->pitch) + 1 : 92;
      if (sj->bzclk <= 0)
        sj->bzclk = vper;
      ex = sj->bzclk == vper ? sqrt(vper / 12.0) : 0.0;
      sj->bzclk--;
      double rate = sj->pitch + s->pa + (s->ticks > 1 ? (double)((s->pb - s->pa) * sj->tick) / (s->ticks - 1) : s->pb - s->pa);
      if (rate < 10.0)
        rate = 10.0;
      int per = (int)ceil(8000.0 / rate);
      if (per > 1023)
        per = 1023;
      const double old = sj->bzbuf[(sj->bzpos - per) & 1023];
      sj->bzbuf[sj->bzpos & 1023] = ex;
      sj->bzpos++;
      ex = (ex - 0.6 * old) * 0.8574929257125441 * 0.25; /* 1 / sqrt(1 + 0.6^2): notches dim, not black (user, C2); x0.25: the bands sit 12 dB under a noise band of the same byte, so the low levels the chip needs have fine steps (B7's were pinned at byte 2, still too bright) */
    }
    const double y = g * ex + 2.0 * r * c * sj->nz[0] - r2 * sj->nz[1];
    sj->nz[1] = sj->nz[0];
    sj->nz[0] = y;
    if (s->flags & FX_ALL)
      lvl *= sj->venv; /* both units: C0 and M2 are silent between their pulses */
    /* an allophone's noise is high-passed twice at NHP_FC (every unvoiced fricative, both units, the VoxRecord solo
     * takes with the background taken off: 6-21 dB too much under 400 Hz without it) */
    double yo = y;
    /* and the swept noise (C8, C9) at NSWEEP_HP (2026-11-06: ~10 dB heavy under 300 Hz without it) */
    if (!sj->effect || (s->flags & FX_NSWEEP) || sj->fxhpk > 0.0) {
      const double k = sj->effect ? (sj->fxhpk > 0.0 ? sj->fxhpk : NSWEEP_HPK) : sj->nhk;
      const double o1 = k * (sj->nhp[1] + y - sj->nhp[0]);
      sj->nhp[0] = y;
      sj->nhp[1] = o1;
      const double o2 = k * (sj->nhp[3] + o1 - sj->nhp[2]);
      sj->nhp[2] = o1;
      sj->nhp[3] = o2;
      yo = (NHP_TWO ? o2 : o1) + sj->wfloor * ex;
    }
    if (sj->thump > 0.0 && sj->stick < TICKS(THUMP_MS)) { /* low noise (one pole at THUMP_HZ) fading over THUMP_MS */
      sj->thlp += sj->thk * (ex - sj->thlp);
      yo += sj->thump * sj->thlp * (1.0 - (double)sj->stick / TICKS(THUMP_MS));
    }
    v += lvl * f.lv * f.na / 128.0 * 0.86 * yo;
    if (s->flags & FX_BUZZ)
      sj->vbuzz = lvl * f.lv * f.na / 128.0 * 0.86 * y;
  }

  if (s->gate > 0 && !(s->flags & FX_NSWEEP) && sj->tick % s->gate >= s->gate / 2)
    v *= T.gate_depth;
  if (s->flags & FX_DECAY)
    v *= sj_exp(-T.decay_db * t / 20.0 * M_LN10);
  if (s->flags & FX_FADE)
    v *= 1.0 - t; /* the tones' ending, measured: the level falls straight to nothing (-6 dB at half way, -20 at nine tenths) */
  if (s->flags & FX_BUMP)
    v *= sj_sin2pi(0.5 * t);

  /* the sound's attack and release */
  {
    const int att = sj->attack, rel = sj->release;
    if (sj->stick < att)
      v *= (double)sj->stick / att;
    if (sj->stotal - sj->stick < rel)
      v *= (double)(sj->stotal - sj->stick) / rel;
    if (sj->stick >= sj->onset_from && sj->stick < sj->onset_from + sj->onset)
      v *= sj->onset_gain;
    if (sj->onset_from > 0 && sj->stick >= sj->onset_from)
      v *= sj->release_gain;
    if (sj->fxburst > 0.0 && sj->stick < TICKS(sj->fxbms)) {
      sj->fxblp += sj->fxbk * (2.0 * rnd(sj) - sj->fxblp);
      v += sj->fxburst * sj->fxblp * (1.0 - (double)sj->stick / TICKS(sj->fxbms));
    }
  }
  sj->stick++;

  /* the effect's peaking filters (transposed direct form II) */
  for (int k = 0; k < sj->neq; k++) {
    const double *c = sj->eqc[k], y = c[0] * v + sj->eqz[k][0];
    sj->eqz[k][0] = c[1] * v - c[3] * y + sj->eqz[k][1];
    sj->eqz[k][1] = c[2] * v - c[4] * y;
    v = y;
  }
  v = output_stage(sj, v, s->voiced && !(s->flags & (FX_VOICED | FX_PULSED)));

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

/* the two poles driven by x held for d ticks, exactly (e1, e2: their decays over d) */
static void settle(sj_chip *sj, double x, double e1, double e2) {
  const double u1 = sj->z[0] - x;
  sj->z[0] = x + u1 * e1;
  sj->z[1] = x + (sj->z[1] - x) * e2 + u1 * sj->lpf[4] * (e1 - e2);
}

bool sj_render(sj_chip *sj, float *buf, size_t n) {
  const double step = RATE / SJ_SAMPLE_RATE;
  bool sounding = sj->sounding || sj->count > 0 || scp_audible(sj);
  for (size_t i = 0; i < n; i++) {
    /* the held chip samples through the AtariVox's low pass, sampled at the
     * end of this output sample (the zero adds the second pole's input in
     * proportion: y2 + y2' / zero) */
    if (sj->heldpos + step < 1.0) {
      settle(sj, sj->held, sj->lpf[2], sj->lpf[3]);
      sj->heldpos += step;
    } else {
      const double a = 1.0 - sj->heldpos, b = step - a;
      settle(sj, sj->held, exp(-sj->lpf[0] * a), exp(-sj->lpf[1] * a));
      sj->held = tick(sj);
      sounding = sounding || sj->sounding || sj->count > 0 || scp_audible(sj);
      settle(sj, sj->held, exp(-sj->lpf[0] * b), exp(-sj->lpf[1] * b));
      sj->heldpos = b;
    }
    buf[i] = (float)(sj->z[1] + sj->lpf[5] * (sj->z[0] - sj->z[1]));
  }
  sj->speaking = sounding;
  return sounding;
}
