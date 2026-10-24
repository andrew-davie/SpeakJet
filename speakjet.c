/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * speakjet: a C port of Andrew Davie's Go SpeakJet emulation. see speakjet.h
 *
 * the sections below follow the Go source files: synth.go, msa.go, tables.go
 * and speakjet.go. comments are carried over from the Go code
 */

#include "speakjet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SJ_PI 3.14159265358979323846
#define SJ_SQRT2 1.41421356237309504880
#define SJ_MAX_INT32 2147483647.0

static double fmin2(double a, double b) { return a < b ? a : b; }
static double fmax2(double a, double b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* ======================================================================== */
/* synth.go                                                                 */
/* ======================================================================== */

/* limits of the synthesizer registers, from the manual */
#define MAX_FREQUENCY 3999.0
#define MAX_OSC_VOLUME 31.0
#define MAX_MIXER1 63.0
#define MAX_MASTER 127.0

/* envelope wave shapes, as selected by the first two bits of the envelope
 * control register */
typedef enum { ENV_SAW, ENV_SINE, ENV_TRIANGLE, ENV_SQUARE } envelope_wave;

/* frame is a set of synthesizer register values. the MSA sequencer moves the
 * synthesizer from one frame to the next over time */
typedef struct frame {
  /* frequency and volume of oscillators 1 to 5 */
  double freq[5];
  double vol[5];

  /* distortion of oscillators 4 and 5 (0 to 255) */
  double distortion;

  /* frequency of the envelope oscillator. for speech this is the voicing
   * pitch */
  double pitch;

  /* envelope control: the wave shape, whether oscillators 1 to 3 are
   * enveloped and whether oscillators 4 and 5 are half enveloped */
  envelope_wave wave;
  bool voiced;
  bool half_env;
} frame;

/* lerp returns the frame part way between a and b. the envelope control
 * settings are taken from b */
static frame lerp(const frame *a, const frame *b, double t) {
  frame r = *b;
  for (int i = 0; i < 5; i++) {
    r.freq[i] = a->freq[i] + (b->freq[i] - a->freq[i]) * t;
    r.vol[i] = a->vol[i] + (b->vol[i] - a->vol[i]) * t;
  }
  r.distortion = a->distortion + (b->distortion - a->distortion) * t;
  r.pitch = a->pitch + (b->pitch - a->pitch) * t;
  return r;
}

/* synth is the 5-channel synthesizer */
typedef struct synth {
  /* phase of oscillators 1 to 5 and of the envelope oscillator, in cycles */
  double phase[5];
  double env_phase;

  /* noise source for distortion (xorshift) */
  uint32_t noise;

  /* 2-pole low pass output filter and dc blocker state */
  double x1, x2, y1, y2;
  double dc_in, dc_out;
  double b0, b1, b2;
  double a1, a2;
} synth;

static void synth_init(synth *s) {
  memset(s, 0, sizeof(*s));
  s->noise = 0x2545f491u;

  /* butterworth low pass for the "two pole" filter the manual places after
   * the PWM output. its 2200Hz corner was fitted by the openFPGA-7800
   * AtariVox engine (MIT licence) to a recording of the real chip, using a
   * synthesizer like this one; the corner Stella's SpeakJet engine measured
   * independently is close (1700Hz) */
  const double cutoff = 2200.0;
  const double w = 2 * SJ_PI * cutoff / SJ_SAMPLE_RATE;
  const double alpha = sin(w) / SJ_SQRT2;
  const double cw = cos(w);
  const double a0 = 1 + alpha;
  s->b0 = (1 - cw) / 2 / a0;
  s->b1 = (1 - cw) / a0;
  s->b2 = (1 - cw) / 2 / a0;
  s->a1 = -2 * cw / a0;
  s->a2 = (1 - alpha) / a0;
}

static double synth_random(synth *s) {
  s->noise ^= s->noise << 13;
  s->noise ^= s->noise >> 17;
  s->noise ^= s->noise << 5;
  return (double)(int32_t)s->noise / SJ_MAX_INT32;
}

/* sample produces the next output sample, in the range -1 to 1. bend is the
 * frequency multiplier from the Bend control and master is the master volume
 * (0 to 127) */
static double synth_sample(synth *s, const frame *f, double bend, double master) {
  /* envelope oscillator */
  double env = 1.0;
  if (f->pitch > 0) {
    s->env_phase += f->pitch / SJ_SAMPLE_RATE;
    if (s->env_phase >= 1) {
      s->env_phase -= floor(s->env_phase);

      /* restart the formant oscillators with each envelope period */
      if (f->voiced) {
        s->phase[0] = 0;
        s->phase[1] = 0;
        s->phase[2] = 0;
      }
    }
    const double p = s->env_phase;
    switch (f->wave) {
    case ENV_SAW:
      env = 1 - p;
      break;
    case ENV_SINE:
      env = 0.5 + 0.5 * cos(2 * SJ_PI * p);
      break;
    case ENV_TRIANGLE:
      env = 1 - 2 * fabs(p - 0.5);
      break;
    case ENV_SQUARE:
      if (p >= 0.5)
        env = 0;
      break;
    }
  } else if (f->voiced) {
    /* a pitch of zero does not vocalise */
    env = 0;
  }

  /* mixer 1: oscillators 1 to 3 */
  double mix1 = 0, total = 0;
  for (int i = 0; i < 3; i++) {
    const double fr = fmin2(f->freq[i] * bend, MAX_FREQUENCY);
    s->phase[i] += fr / SJ_SAMPLE_RATE;
    s->phase[i] -= floor(s->phase[i]);
    const double v = fmin2(f->vol[i], MAX_OSC_VOLUME);
    total += v;
    mix1 += v * sin(2 * SJ_PI * s->phase[i]);
  }
  if (total > MAX_MIXER1)
    mix1 *= MAX_MIXER1 / total;
  if (f->voiced)
    mix1 *= env;

  /* mixer 2: oscillators 4 and 5, with distortion spreading noise around
   * the oscillator frequency */
  double mix2 = 0;
  const double spread = f->distortion / 255 * 1.5;
  for (int i = 3; i < 5; i++) {
    const double fr = fmin2(f->freq[i] * bend, MAX_FREQUENCY);
    s->phase[i] += fr * (1 + spread * synth_random(s)) / SJ_SAMPLE_RATE;
    s->phase[i] -= floor(s->phase[i]);
    mix2 += fmin2(f->vol[i], MAX_OSC_VOLUME) * sin(2 * SJ_PI * s->phase[i]);
  }
  if (f->half_env)
    mix2 *= 0.5 + 0.5 * env;

  /* mixer 5: summation and master volume */
  const double x = (mix1 + mix2) / (MAX_MIXER1 + 2 * MAX_OSC_VOLUME) *
                   fmin2(master, MAX_MASTER) / MAX_MASTER;

  /* dc blocker */
  const double y = x - s->dc_in + 0.995 * s->dc_out;
  s->dc_in = x;
  s->dc_out = y;

  /* output filter */
  const double out = s->b0 * y + s->b1 * s->x1 + s->b2 * s->x2 -
                     s->a1 * s->y1 - s->a2 * s->y2;
  s->x2 = s->x1;
  s->x1 = y;
  s->y2 = s->y1;
  s->y1 = out;

  return out;
}

/* ======================================================================== */
/* tables.go                                                                */
/* ======================================================================== */

/* the most parts any sound has (A7) */
#define MAX_PARTS 8

/* part is one stage of a sound: the synthesizer glides from wherever it is to
 * the target frame over glide_ms and then holds it for the rest of ms */
typedef struct part {
  double ms;
  double glide_ms;
  frame target;
} part;

/* AUTO_GLIDE in a part's glide_ms means the default transition time for
 * speech sounds (see msa_expand()) */
#define AUTO_GLIDE (-1.0)

/* sound is an allophone or sound effect. speech sounds take their pitch from
 * the Pitch control, their duration from the Speed control, and are affected
 * by Fast, Slow, Stress and Relax. sound effects use the pitch and timing in
 * their parts as they are */
typedef struct sound {
  bool speech;

  /* how much longer the chip plays a speech sound than the manual's length,
   * which the parts add up to */
  double length;

  int nparts;
  part parts[MAX_PARTS];
} sound;

/* the chip's lengths for speech sounds, measured from a recording of a real
 * SpeakJet playing Alex Herbert's vox_test ROM (2004), compared sound by sound
 * with this library playing the same codes. at the default speed the steady
 * sounds (vowels, nasals, liquids, fricatives) last about 1.5 times the
 * manual's length (EH 2.07, IH 1.6, NGE 1.43, and SE 1.67, TH 1.42, NE 1.14
 * when slow), diphthongs about 1.2 times (EYIY 1.0, IHWW 1.5, and OHIY 1.2,
 * AXRR 1.3 when fast), and voiceless stops what the manual gives (TT 1.0). at
 * speed 127 the same steady and stop lengths and the speed curve below give
 * the recording's syllable length to within a few milliseconds */
#define STEADY_LENGTH 1.5
#define DIPHTHONG_LENGTH 1.2
#define STOP_LENGTH 1.0

/* formant targets. frequencies are approximate values for an adult male
 * speaker, from published measurements of English vowels and consonants,
 * except where two independent measurements of the real chip agree.
 *
 * measured on the chip: where two independent measurements of a sound's
 * formant agree, it is set to their mean. they agree when they are within 10%
 * of each other or within 88Hz (the spacing of the voice's harmonics at the
 * default pitch, about as finely as a formant can be read from a recording).
 * the measurements are the openFPGA-7800 AtariVox engine's, read from
 * recordings of games speaking (docs/ATARIVOX.md, MIT licence), and those
 * behind Stella's SpeakJet engine (feature/speakjet branch). formants where the
 * two disagree, or that only one of them measured, keep their published
 * values:
 *
 *   OW: F1 F2      AX: F1 F2 F3   RR: F1 F2 F3   UX: F1 F2   IH: F1 F2 F3
 *   MM: F1 F3      EY: F1 F2 F3   EH: F1 F2      AW: F2 */
typedef struct formants {
  double f[3];
} formants;

#define FM(a, b, c) ((formants){{(a), (b), (c)}})

static const formants fIY = {{270, 2290, 3010}};
static const formants fIH = {{322, 1774, 2490}};
static const formants fEY = {{508, 1820, 2425}};
static const formants fEH = {{600, 1666, 2480}};
static const formants fAY = {{660, 1720, 2410}};
static const formants fAX = {{521, 1510, 2395}};
static const formants fUX = {{613, 1328, 2390}};
static const formants fOH = {{730, 1090, 2440}};
static const formants fAW = {{590, 998, 2540}};
static const formants fOW = {{512, 891, 2350}};
static const formants fUH = {{440, 1020, 2240}};
static const formants fUW = {{300, 870, 2240}};
static const formants fAH = {{700, 1220, 2600}};
static const formants fER = {{480, 1350, 1650}};
static const formants fRR = {{439, 1318, 1720}};
static const formants fLE = {{360, 1300, 2700}};
static const formants fLO = {{400, 900, 2500}};
static const formants fWW = {{300, 650, 2200}};
static const formants fYY = {{280, 2200, 2900}};

/* consonant loci: where the formants are at a consonant's place of
 * articulation. the following sound glides out of them */
static const formants lLabial = {{250, 900, 2100}};
static const formants lLabialBack = {{250, 750, 2100}};
static const formants lDental = {{300, 1400, 2600}};
static const formants lAlveolar = {{250, 1750, 2700}};
static const formants lAlveolarBak = {{250, 1500, 2600}};
static const formants lPalatal = {{280, 1900, 2500}};
static const formants lVelar = {{250, 2300, 2800}};
static const formants lVelarBack = {{250, 1300, 2300}};
static const formants lGlottal = {{500, 1500, 2500}};

/* frame constructors */

static frame voiced_frame(formants f, double v1, double v2, double v3) {
  frame r;
  memset(&r, 0, sizeof(r));
  r.freq[0] = f.f[0];
  r.freq[1] = f.f[1];
  r.freq[2] = f.f[2];
  r.vol[0] = v1;
  r.vol[1] = v2;
  r.vol[2] = v3;
  r.wave = ENV_SAW;
  r.voiced = true;
  return r;
}

static frame vowel_frame(formants f) { return voiced_frame(f, 24, 18, 10); }
static frame nasal_frame(formants f) { return voiced_frame(f, 22, 7, 4); }
static frame liquid_frame(formants f) { return voiced_frame(f, 22, 13, 7); }

/* silence keeps the formant frequencies so that the next sound glides from
 * them */
static frame silent_frame(formants f) {
  frame r;
  memset(&r, 0, sizeof(r));
  r.freq[0] = f.f[0];
  r.freq[1] = f.f[1];
  r.freq[2] = f.f[2];
  r.wave = ENV_SAW;
  return r;
}

/* noise from oscillators 4 and 5. oscillators 1 to 3 are silent but sit at
 * the locus, so the next sound glides out of it */
static frame noise_frame(formants locus, double n1, double n2, double v1,
                         double v2, double dist) {
  frame r;
  memset(&r, 0, sizeof(r));
  r.freq[0] = locus.f[0];
  r.freq[1] = locus.f[1];
  r.freq[2] = locus.f[2];
  r.freq[3] = n1;
  r.freq[4] = n2;
  r.vol[3] = v1;
  r.vol[4] = v2;
  r.distortion = dist;
  r.wave = ENV_SAW;
  return r;
}

/* voiced noise: a weak voiced murmur with half-enveloped noise */
static frame voiced_noise_frame(formants f, double v1, double n1, double n2,
                                double nv1, double nv2, double dist) {
  frame r;
  memset(&r, 0, sizeof(r));
  r.freq[0] = f.f[0];
  r.freq[1] = f.f[1];
  r.freq[2] = f.f[2];
  r.freq[3] = n1;
  r.freq[4] = n2;
  r.vol[0] = v1;
  r.vol[1] = v1 * 0.4;
  r.vol[2] = v1 * 0.2;
  r.vol[3] = nv1;
  r.vol[4] = nv2;
  r.distortion = dist;
  r.wave = ENV_SAW;
  r.voiced = true;
  r.half_env = true;
  return r;
}

/* hush turns the noise oscillators (4 and 5) of a frame down by db decibels */
static frame hush(frame f, double db) {
  const double g = pow(10, -db / 20);
  f.vol[3] *= g;
  f.vol[4] *= g;
  return f;
}

/* how much quieter the chip's noise is than the levels first chosen here.
 * measured from the vox_test recording (see STEADY_LENGTH) by comparing each
 * noise sound's level against the vowel beside it, on the chip and here. on
 * the chip hiss sits 16 to 23dB under the vowels; here it was level with them.
 * the corrections: SE 23dB, CH (the SH frame) 16.4dB, TH 11.8dB, FF 13.7dB,
 * stop bursts 19.5dB (TT 23dB, KE 16dB) and aspiration 14.5dB (TT). sounds the
 * recording doesn't have take the mean of the fricatives that it does */
#define SE_DB 23.0
#define SH_DB 16.4
#define TH_DB 11.8
#define FF_DB 13.7
#define FRICATIVE_DB 17.0
#define BURST_DB 19.5
#define ASPIRATION_DB 14.5

/* sound constructors. each adds to the sound passed to it */

static void add_part(sound *s, double ms, double glide_ms, frame target) {
  if (s->nparts >= MAX_PARTS)
    return;
  s->parts[s->nparts].ms = ms;
  s->parts[s->nparts].glide_ms = glide_ms;
  s->parts[s->nparts].target = target;
  s->nparts++;
}

static void steady(sound *s, double ms, frame f) {
  s->speech = true;
  s->length = STEADY_LENGTH;
  add_part(s, ms, AUTO_GLIDE, f);
}

/* a diphthong holds its first target, slides to its second and briefly
 * holds it */
static void diphthong(sound *s, double ms, frame a, frame b) {
  s->speech = true;
  s->length = DIPHTHONG_LENGTH;
  add_part(s, ms * 0.3, AUTO_GLIDE, a);
  add_part(s, ms * 0.55, ms * 0.55, b);
  add_part(s, ms * 0.15, 0, b);
}

/* stop describes a plosive: a closure (silence, or a voice bar for voiced
 * stops), a burst of noise at the release, aspiration for voiceless stops,
 * and for voiced stops the onset of voicing at the locus */
typedef struct stop {
  double ms, closure, burst;
  bool voiced;
  formants locus;
  double burst_freq[2];
  double burst_vol[2];
  double asp_freq[2];
} stop;

static void plosive(sound *s, const stop *st) {
  frame close;
  if (st->voiced)
    close = voiced_frame(FM(180, st->locus.f[1], st->locus.f[2]), 8, 0, 0);
  else
    close = silent_frame(st->locus);

  frame burst = hush(noise_frame(st->locus, st->burst_freq[0], st->burst_freq[1],
                                 st->burst_vol[0], st->burst_vol[1], 255),
                     BURST_DB);
  if (st->voiced) {
    burst.vol[0] = 6;
    burst.voiced = true;
    burst.half_env = true;
  }

  s->speech = true;
  s->length = STOP_LENGTH;
  add_part(s, st->closure, 6, close);
  add_part(s, st->burst, 2, burst);

  const double rest = st->ms - st->closure - st->burst;
  if (st->voiced) {
    if (rest > 0)
      add_part(s, rest, rest, voiced_frame(st->locus, 14, 6, 3));
  } else if (rest > 0) {
    const frame asp = hush(
        noise_frame(st->locus, st->asp_freq[0], st->asp_freq[1], 10, 8, 230),
        ASPIRATION_DB);
    add_part(s, rest, 4, asp);
  }
}

/* an affricate is a stop released into a fricative */
static void affricate(sound *s, const stop *st, frame fric, double fric_ms) {
  plosive(s, st);
  add_part(s, fric_ms, 5, fric);
}

/* frames for the fricatives, reused by the affricates and TS */
static frame frSE(void) { return hush(noise_frame(lAlveolar, 3800, 3990, 20, 16, 160), SE_DB); }
static frame frSO(void) { return hush(noise_frame(lAlveolarBak, 3500, 3900, 20, 14, 160), SE_DB); }
static frame frSH(void) { return hush(noise_frame(lPalatal, 2300, 3100, 22, 16, 200), SH_DB); }
static frame frZH(void) {
  return hush(voiced_noise_frame(FM(260, 1800, 2500), 14, 2400, 3200, 14, 10, 200),
              FRICATIVE_DB);
}

/* stop descriptions: duration, closure, burst, voiced, locus, burst
 * frequencies, burst volumes, aspiration frequencies */
#define STOP(ms_, cl_, bu_, vo_, lo_, bf0, bf1, bv0, bv1, af0, af1)            \
  (stop) {                                                                     \
    (ms_), (cl_), (bu_), (vo_), (lo_), {(bf0), (bf1)}, {(bv0), (bv1)},         \
        {(af0), (af1)}                                                         \
  }

/* allophones, codes 128 to 199. durations are from the manual */
static const char *const allophone_names[] = {
    "IY",   "IH",   "EY",   "EH",   "AY",   "AX",   "UX",   "OH",   "AW",
    "OW",   "UH",   "UW",   "MM",   "NE",   "NO",   "NGE",  "NGO",  "LE",
    "LO",   "WW",   "RR",   "IYRR", "EYRR", "AXRR", "AWRR", "OWRR", "EYIY",
    "OHIY", "OWIY", "OHIH", "IYEH", "EHLL", "IYUW", "AXUW", "IHWW", "AYWW",
    "OWWW", "JH",   "VV",   "ZZ",   "ZH",   "DH",   "BE",   "BO",   "EB",
    "OB",   "DE",   "DO",   "ED",   "OD",   "GE",   "GO",   "EG",   "OG",
    "CH",   "HE",   "HO",   "WH",   "FF",   "SE",   "SO",   "SH",   "TH",
    "TT",   "TU",   "TS",   "KE",   "KO",   "EK",   "OK",   "PE",   "PO",
};

#define NUM_ALLOPHONES                                                         \
  ((int)(sizeof(allophone_names) / sizeof(allophone_names[0])))

static void allophone(sound *s, int code) {
  stop st;
  switch (code) {
  case 128: steady(s, 70, vowel_frame(fIY)); break;
  case 129: steady(s, 70, vowel_frame(fIH)); break;
  case 130: steady(s, 70, vowel_frame(fEY)); break;
  case 131: steady(s, 70, vowel_frame(fEH)); break;
  case 132: steady(s, 70, vowel_frame(fAY)); break;
  case 133: steady(s, 70, vowel_frame(fAX)); break;
  case 134: steady(s, 70, vowel_frame(fUX)); break;
  case 135: steady(s, 70, vowel_frame(fOH)); break;
  case 136: steady(s, 70, vowel_frame(fAW)); break;
  case 137: steady(s, 70, vowel_frame(fOW)); break;
  case 138: steady(s, 70, vowel_frame(fUH)); break;
  case 139: steady(s, 70, vowel_frame(fUW)); break;
  case 140: steady(s, 70, nasal_frame(FM(268, 1000, 2187))); break; /* MM */
  case 141: steady(s, 70, nasal_frame(FM(250, 1800, 2600))); break; /* NE */
  case 142: steady(s, 70, nasal_frame(FM(250, 1300, 2400))); break; /* NO */
  case 143: steady(s, 70, nasal_frame(FM(250, 2100, 2700))); break; /* NGE */
  case 144: steady(s, 70, nasal_frame(FM(250, 1100, 2300))); break; /* NGO */
  case 145: steady(s, 70, liquid_frame(fLE)); break;
  case 146: steady(s, 70, liquid_frame(fLO)); break;
  case 147: steady(s, 70, liquid_frame(fWW)); break;
  case 148: steady(s, 70, liquid_frame(fRR)); break;
  case 149: diphthong(s, 200, vowel_frame(fIY), liquid_frame(fER)); break; /* IYRR */
  case 150: diphthong(s, 200, vowel_frame(fEH), liquid_frame(fER)); break; /* EYRR */
  case 151: diphthong(s, 190, vowel_frame(FM(500, 1400, 1750)), liquid_frame(fER)); break; /* AXRR */
  case 152: diphthong(s, 200, vowel_frame(fOH), liquid_frame(fER)); break; /* AWRR */
  case 153: diphthong(s, 185, vowel_frame(fAW), liquid_frame(fER)); break; /* OWRR */
  case 154: diphthong(s, 165, vowel_frame(fEY), vowel_frame(fIY)); break; /* EYIY */
  case 155: diphthong(s, 200, vowel_frame(fOH), vowel_frame(fIY)); break; /* OHIY */
  case 156: diphthong(s, 225, vowel_frame(fAW), vowel_frame(fIY)); break; /* OWIY */
  case 157: diphthong(s, 185, vowel_frame(fOH), vowel_frame(fIH)); break; /* OHIH */
  case 158: diphthong(s, 170, vowel_frame(fYY), vowel_frame(fEH)); break; /* IYEH */
  case 159: diphthong(s, 140, vowel_frame(fAX), liquid_frame(fLO)); break; /* EHLL */
  case 160: diphthong(s, 180, vowel_frame(fYY), vowel_frame(fUW)); break; /* IYUW */
  case 161: diphthong(s, 170, vowel_frame(fAH), vowel_frame(fUW)); break; /* AXUW */
  case 162: diphthong(s, 170, vowel_frame(FM(330, 1300, 2300)), vowel_frame(fUW)); break; /* IHWW */
  case 163: diphthong(s, 200, vowel_frame(fAY), vowel_frame(fUH)); break; /* AYWW */
  case 164: diphthong(s, 131, vowel_frame(fOW), vowel_frame(FM(330, 700, 2250))); break; /* OWWW */
  case 165: /* JH */
    st = STOP(30, 25, 5, true, lPalatal, 2600, 3300, 12, 8, 0, 0);
    affricate(s, &st, frZH(), 40);
    break;
  case 166: steady(s, 70, hush(voiced_noise_frame(FM(220, 1100, 2100), 16, 3000, 3800, 8, 5, 255), FRICATIVE_DB)); break; /* VV */
  case 167: steady(s, 70, hush(voiced_noise_frame(FM(240, 1500, 2500), 14, 3700, 3950, 14, 10, 160), FRICATIVE_DB)); break; /* ZZ */
  case 168: steady(s, 70, frZH()); break; /* ZH */
  case 169: steady(s, 70, hush(voiced_noise_frame(FM(250, 1300, 2400), 16, 3000, 3800, 7, 5, 255), FRICATIVE_DB)); break; /* DH */
  case 170: st = STOP(45, 25, 8, true, lLabial, 700, 1200, 10, 6, 0, 0); plosive(s, &st); break; /* BE */
  case 171: st = STOP(45, 25, 8, true, lLabialBack, 600, 1000, 10, 6, 0, 0); plosive(s, &st); break; /* BO */
  case 172: st = STOP(10, 5, 5, true, lLabial, 700, 1200, 8, 5, 0, 0); plosive(s, &st); break; /* EB */
  case 173: st = STOP(10, 5, 5, true, lLabialBack, 600, 1000, 8, 5, 0, 0); plosive(s, &st); break; /* OB */
  case 174: st = STOP(45, 25, 8, true, lAlveolar, 3300, 3900, 12, 8, 0, 0); plosive(s, &st); break; /* DE */
  case 175: st = STOP(45, 25, 8, true, lAlveolarBak, 3000, 3700, 12, 8, 0, 0); plosive(s, &st); break; /* DO */
  case 176: st = STOP(10, 5, 5, true, lAlveolar, 3300, 3900, 10, 6, 0, 0); plosive(s, &st); break; /* ED */
  case 177: st = STOP(10, 5, 5, true, lAlveolarBak, 3000, 3700, 10, 6, 0, 0); plosive(s, &st); break; /* OD */
  case 178: st = STOP(55, 30, 10, true, lVelar, 2300, 2900, 12, 8, 0, 0); plosive(s, &st); break; /* GE */
  case 179: st = STOP(55, 30, 10, true, lVelarBack, 1300, 1800, 12, 8, 0, 0); plosive(s, &st); break; /* GO */
  case 180: st = STOP(55, 35, 10, true, lVelar, 2300, 2900, 10, 6, 0, 0); plosive(s, &st); break; /* EG */
  case 181: st = STOP(55, 35, 10, true, lVelarBack, 1300, 1800, 10, 6, 0, 0); plosive(s, &st); break; /* OG */
  case 182: /* CH */
    st = STOP(30, 25, 5, false, lPalatal, 3000, 3600, 16, 12, 0, 0);
    affricate(s, &st, frSH(), 40);
    break;
  case 183: steady(s, 70, hush(noise_frame(FM(400, 1900, 2600), 1900, 2600, 12, 9, 255), FRICATIVE_DB)); break; /* HE */
  case 184: steady(s, 70, hush(noise_frame(FM(600, 1000, 2400), 1000, 2400, 12, 8, 255), FRICATIVE_DB)); break; /* HO */
  case 185: steady(s, 70, hush(noise_frame(fWW, 700, 1300, 10, 7, 255), FRICATIVE_DB)); break; /* WH */
  case 186: steady(s, 70, hush(noise_frame(lLabial, 2500, 3800, 6, 9, 255), FF_DB)); break; /* FF */
  case 187: steady(s, 40, frSE()); break; /* SE */
  case 188: steady(s, 40, frSO()); break; /* SO */
  case 189: steady(s, 50, frSH()); break; /* SH */
  case 190: steady(s, 40, hush(noise_frame(lDental, 3000, 3900, 6, 6, 255), TH_DB)); break; /* TH */
  case 191: st = STOP(50, 30, 10, false, lAlveolar, 3500, 3950, 18, 14, 1800, 2600); plosive(s, &st); break; /* TT */
  case 192: st = STOP(70, 30, 8, false, lAlveolar, 3500, 3950, 18, 14, 1800, 2600); plosive(s, &st); break; /* TU */
  case 193: /* TS */
    st = STOP(38, 30, 8, false, lAlveolar, 3500, 3950, 18, 14, 0, 0);
    affricate(s, &st, frSE(), 132);
    break;
  case 194: st = STOP(55, 25, 12, false, lVelar, 2300, 2800, 18, 12, 2000, 2800); plosive(s, &st); break; /* KE */
  case 195: st = STOP(55, 25, 12, false, lVelarBack, 1300, 1700, 18, 12, 1100, 2300); plosive(s, &st); break; /* KO */
  case 196: st = STOP(55, 30, 15, false, lVelar, 2300, 2800, 16, 10, 2000, 2800); plosive(s, &st); break; /* EK */
  case 197: st = STOP(45, 25, 15, false, lVelarBack, 1300, 1700, 16, 10, 1100, 2300); plosive(s, &st); break; /* OK */
  case 198: st = STOP(99, 50, 9, false, lLabial, 600, 1500, 12, 9, 1200, 2400); plosive(s, &st); break; /* PE */
  case 199: st = STOP(99, 50, 9, false, lLabialBack, 500, 1200, 12, 9, 1000, 2300); plosive(s, &st); break; /* PO */
  default: break;
  }
}

/* effect constructors. none of the sound effects except DTMF are described
 * by the manual beyond their name and length, so these are approximations
 * built from the same oscillators */

/* tone is a plain sine on oscillator 1 (and optionally 2) with no envelope */
static frame tone(double f1, double f2) {
  frame t;
  memset(&t, 0, sizeof(t));
  t.freq[0] = f1;
  t.freq[1] = f2;
  t.vol[0] = 28;
  if (f2 > 0) {
    t.vol[0] = 24;
    t.vol[1] = 24;
  }
  return t;
}

/* buzz is an enveloped (voiced) sound at a fixed pitch */
static frame buzz(formants f, double pitch, envelope_wave wave) {
  frame b = voiced_frame(f, 26, 18, 10);
  b.pitch = pitch;
  b.wave = wave;
  return b;
}

static frame quiet(frame f) {
  for (int i = 0; i < 5; i++)
    f.vol[i] = 0;
  return f;
}

/* fade turns every oscillator of a frame down by db decibels */
static frame fade(frame f, double db) {
  const double g = pow(10, -db / 20);
  for (int i = 0; i < 5; i++)
    f.vol[i] *= g;
  return f;
}

/* a tone with its own envelope pitch and wave */
static frame pulsed_tone(double f1, double f2, double pitch, envelope_wave wave) {
  frame f = tone(f1, f2);
  f.pitch = pitch;
  f.wave = wave;
  f.voiced = true;
  return f;
}

/* beeps plays a sequence of tones of the given frequencies, on for on ms and
 * off for off ms each */
static void beeps(sound *s, double on, double off, int n, const double *freqs) {
  for (int i = 0; i < n; i++) {
    add_part(s, on, 3, tone(freqs[i], 0));
    add_part(s, off, 3, quiet(tone(freqs[i], 0)));
  }
}

/* sweep glides a tone from one frequency to another */
static void sweep(sound *s, double ms, double from, double to) {
  add_part(s, 3, 3, tone(from, 0));
  add_part(s, ms - 3, ms - 3, tone(to, 0));
}

static void hold(sound *s, double ms, double glide, frame f) {
  add_part(s, ms, glide, f);
}

/* decay starts f and fades it to silence over ms */
static void decay(sound *s, double ms, frame f) {
  add_part(s, 2, 2, f);
  add_part(s, ms - 2, ms - 2, quiet(f));
}

static void dtmf(sound *s, double low, double high) {
  hold(s, 92, 3, tone(low, high));
  hold(s, 3, 3, quiet(tone(low, high)));
}

static void robot(sound *s, double n) {
  const frame f = buzz(FM(300 + 60 * n, 900 + 120 * n, 2000 + 50 * n),
                       60 + 15 * n, ENV_SQUARE);
  hold(s, 70, 8, f);
  hold(s, 10, 8, quiet(f));
}

/* sound effects, codes 200 to 254. lengths are from the manual */
static const char *const effect_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "A0",
    "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9", "B0", "B1",
    "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "C0", "C1", "C2",
    "C3", "C4", "C5", "C6", "C7", "C8", "C9", "D0", "D1", "D2", "D3",
    "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "M0", "M1", "M2",
};

#define NUM_EFFECTS ((int)(sizeof(effect_names) / sizeof(effect_names[0])))

static void effect(sound *s, int code) {
  switch (code) {
  case 200: case 201: case 202: case 203: case 204:
  case 205: case 206: case 207: case 208: case 209:
    robot(s, code - 200); /* R0 to R9 */
    break;

  case 210: /* A0 */
    sweep(s, 297, 600, 1400);
    hold(s, 3, 3, quiet(tone(1400, 0)));
    break;
  case 211: /* A1 */
    hold(s, 50, 3, tone(1000, 0));
    hold(s, 48, 1, tone(700, 0));
    hold(s, 3, 3, quiet(tone(700, 0)));
    break;
  case 212: /* A2 */
    sweep(s, 99, 1400, 700);
    hold(s, 3, 3, quiet(tone(700, 0)));
    break;
  case 213: /* A3 */
    hold(s, 537, 5, pulsed_tone(900, 1100, 10, ENV_SQUARE));
    hold(s, 3, 3, quiet(tone(900, 1100)));
    break;
  case 214: /* A4 */
    sweep(s, 265, 500, 1200);
    sweep(s, 262, 1200, 500);
    hold(s, 3, 3, quiet(tone(500, 0)));
    break;
  case 215: { /* A5 */
    /* measured from the vox_test recording (see STEADY_LENGTH): two steady
     * tones, the second 12dB under the first (set louder here, as the output
     * filter takes 4dB off it), that glide down together by
     * about 295Hz over 240ms and back up again. the ROM sets Bend 1 before
     * it, and the recording has the tones at 486Hz and 2525Hz, gliding to
     * 190Hz and 2232Hz and back to 478Hz and 2522Hz. the frequencies here are
     * those divided by Bend 1's factor (see msa_bend_factor()), so that Bend
     * 1 plays the recording. whether the chip bends its sound effects at all
     * isn't known */
    frame f = tone(612, 3181);
    f.vol[0] = 28;
    f.vol[1] = 12;
    hold(s, 3, 3, f);
    f.freq[0] = 239;
    f.freq[1] = 2812;
    hold(s, 237, 237, f);
    f.freq[0] = 602;
    f.freq[1] = 3178;
    hold(s, 250, 250, f);
    hold(s, 10, 10, quiet(f));
    break;
  }
  case 216: /* A6 */
    sweep(s, 132, 2000, 400);
    hold(s, 3, 3, quiet(tone(400, 0)));
    break;
  case 217: /* A7 */
    sweep(s, 200, 400, 1600);
    sweep(s, 200, 400, 1600);
    sweep(s, 197, 400, 1600);
    hold(s, 3, 3, quiet(tone(1600, 0)));
    break;
  case 218: { /* A8 */
    static const double f[] = {1600, 1600};
    beeps(s, 100, 50, 2, f);
    break;
  }
  case 219: /* A9 */
    hold(s, 245, 5, pulsed_tone(400, 800, 60, ENV_SQUARE));
    hold(s, 5, 5, quiet(tone(400, 800)));
    break;

  case 220: { static const double f[] = {1000}; beeps(s, 150, 50, 1, f); break; } /* B0 */
  case 221: { static const double f[] = {1200, 1200, 1200}; beeps(s, 60, 30, 3, f); break; } /* B1 */
  case 222: { static const double f[] = {800, 1000, 1200}; beeps(s, 70, 70.0 / 3, 3, f); break; } /* B2 */
  case 223: { static const double f[] = {1200, 1000, 800}; beeps(s, 70, 50.0 / 3, 3, f); break; } /* B3 */
  case 224: { static const double f[] = {2000}; beeps(s, 290, 10, 1, f); break; } /* B4 */
  case 225: { static const double f[] = {880}; beeps(s, 90, 10, 1, f); break; } /* B5 */
  case 226: { static const double f[] = {1320}; beeps(s, 94, 10, 1, f); break; } /* B6 */
  case 227: { static const double f[] = {660}; beeps(s, 90, 10, 1, f); break; } /* B7 */
  case 228: { static const double f[] = {1500, 1000}; beeps(s, 120, 15, 2, f); break; } /* B8 */
  case 229: /* B9 */
    sweep(s, 259, 500, 2500);
    hold(s, 3, 3, quiet(tone(2500, 0)));
    break;

  case 230: /* C0 */
    sweep(s, 78, 2000, 3500);
    sweep(s, 79, 2000, 3500);
    hold(s, 3, 3, quiet(tone(3500, 0)));
    break;
  case 231: /* C1 */
    hold(s, 290, 15, buzz(FM(400, 900, 2000), 30, ENV_SAW));
    hold(s, 10, 10, quiet(buzz(FM(400, 900, 2000), 30, ENV_SAW)));
    break;
  case 232: /* C2 */
    hold(s, 172, 15, buzz(FM(300, 700, 1800), 25, ENV_SAW));
    hold(s, 10, 10, quiet(buzz(FM(300, 700, 1800), 25, ENV_SAW)));
    break;
  case 233: /* C3 */
    sweep(s, 117, 3000, 2200);
    hold(s, 3, 3, quiet(tone(2200, 0)));
    break;
  case 234: /* C4 */
    hold(s, 170, 5, pulsed_tone(2200, 0, 20, ENV_SINE));
    hold(s, 5, 5, quiet(tone(2200, 0)));
    break;
  case 235: { /* C5 */
    frame f = buzz(FM(500, 1000, 2300), 45, ENV_SAW);
    f.freq[3] = 800;
    f.freq[4] = 1600;
    f.vol[3] = 10;
    f.vol[4] = 8;
    f.distortion = 255;
    f.half_env = true;
    hold(s, 340, 20, f);
    hold(s, 10, 10, quiet(buzz(FM(500, 1000, 2300), 45, ENV_SAW)));
    break;
  }
  case 236: /* C6 */
    hold(s, 155, 5, pulsed_tone(3600, 0, 40, ENV_SQUARE));
    hold(s, 5, 5, quiet(tone(3600, 0)));
    break;
  case 237: /* C7 */
    hold(s, 90, 20, buzz(fIY, 300, ENV_SAW));
    hold(s, 90, 90, buzz(fAY, 500, ENV_SAW));
    hold(s, 70, 70, buzz(fOW, 250, ENV_SAW));
    hold(s, 10, 10, quiet(buzz(fOW, 250, ENV_SAW)));
    break;
  case 238: decay(s, 95, noise_frame(lGlottal, 2000, 3000, 24, 16, 255)); break; /* C8 */
  case 239: decay(s, 75, noise_frame(lGlottal, 800, 1400, 28, 20, 255)); break; /* C9 */

  case 240: dtmf(s, 941, 1336); break; /* D0 */
  case 241: dtmf(s, 697, 1209); break; /* D1 */
  case 242: dtmf(s, 697, 1336); break; /* D2 */
  case 243: dtmf(s, 697, 1477); break; /* D3 */
  case 244: dtmf(s, 770, 1209); break; /* D4 */
  case 245: dtmf(s, 770, 1336); break; /* D5 */
  case 246: dtmf(s, 770, 1477); break; /* D6 */
  case 247: dtmf(s, 852, 1209); break; /* D7 */
  case 248: dtmf(s, 852, 1336); break; /* D8 */
  case 249: dtmf(s, 852, 1477); break; /* D9 */
  case 250: dtmf(s, 941, 1209); break; /* D10 */
  case 251: dtmf(s, 941, 1477); break; /* D11 */

  case 252: decay(s, 125, tone(1250, 0)); break; /* M0 */
  case 253: { /* M1 */
    /* measured from the vox_test recording (see STEADY_LENGTH): 30ms of
     * noise with most of its energy under 2kHz, then noise around 1 to 2kHz
     * that dies away by 30dB, fastest at the start and the end */
    const frame bang = noise_frame(lGlottal, 500, 1400, 31, 26, 255);
    const frame hiss = noise_frame(lGlottal, 1000, 1700, 31, 26, 160);
    hold(s, 30, 2, bang);
    hold(s, 20, 20, fade(hiss, 5));
    hold(s, 50, 50, fade(hiss, 12));
    hold(s, 100, 100, fade(hiss, 15.5));
    hold(s, 25, 25, fade(hiss, 19.5));
    hold(s, 25, 25, quiet(hiss));
    break;
  }
  case 254: /* M2 */
    hold(s, 80, 15, buzz(fWW, 100, ENV_SAW));
    hold(s, 200, 200, buzz(fAH, 150, ENV_SAW));
    hold(s, 230, 230, buzz(fUW, 90, ENV_SAW));
    hold(s, 20, 20, quiet(buzz(fUW, 90, ENV_SAW)));
    break;
  default: break;
  }
}

/* lookup builds the sound for an MSA code. the Go version builds every sound
 * once into a table; here each is built when it is needed, which costs a few
 * struct copies per command and keeps the library free of global state */
static bool lookup(uint8_t code, sound *s) {
  memset(s, 0, sizeof(*s));
  if (code >= 128 && code < 128 + NUM_ALLOPHONES)
    allophone(s, code);
  else if (code >= 200 && code < 200 + NUM_EFFECTS)
    effect(s, code);
  else
    return false;
  return true;
}

const char *sj_sound_name(uint8_t code) {
  if (code >= 128 && code < 128 + NUM_ALLOPHONES)
    return allophone_names[code - 128];
  if (code >= 200 && code < 200 + NUM_EFFECTS)
    return effect_names[code - 200];
  return NULL;
}

/* ======================================================================== */
/* msa.go                                                                   */
/* ======================================================================== */

/* pause lengths in milliseconds for codes 0 to 6, from the manual */
static const double pause_ms[] = {0, 100, 200, 700, 30, 60, 90};

/* how quickly the volume ramps down at the start of a pause, and at auto
 * silence when the input buffer has emptied. not given by the manual */
#define RAMP_MS 10.0
#define AUTO_SILENCE_MS 15.0

/* segment is a part with its timing converted to samples */
typedef struct segment {
  int samples;
  int glide;
  frame target;

  /* the frequencies are set at the start of the segment rather than glided */
  bool jump;
} segment;

static int to_samples(double ms) {
  return (int)round(ms * SJ_SAMPLE_RATE / 1000);
}

/* msa sequences commands into segments and plays them on the synthesizer.
 *
 * the Go version expands a Repeat into a slice holding every copy of the
 * sound's segments; here the segments of one copy are kept and played
 * 'copies' times, which is the same sequence */
typedef struct msa {
  synth synth;

  int volume;
  int speed;
  int pitch;
  int bend;

  /* modifiers for the next sound */
  bool fast, slow;
  bool stress, relax;
  int repeat;
  bool jump_next_freq;
  bool auto_silenced;

  segment segs[MAX_PARTS];
  int nsegs;  /* segments in one copy */
  int seg;    /* the segment playing; busy while seg < nsegs */
  int copies; /* copies still to play after this one */

  int seg_pos;
  frame seg_start;
  frame current;
} msa;

static void msa_reset_controls(msa *m) {
  m->volume = SJ_DEFAULT_VOLUME;
  m->speed = SJ_DEFAULT_SPEED;
  m->pitch = SJ_DEFAULT_PITCH;
  m->bend = SJ_DEFAULT_BEND;
}

static void msa_init(msa *m) {
  memset(m, 0, sizeof(*m));
  synth_init(&m->synth);
  msa_reset_controls(m);

  /* start silent but at a neutral vowel and the default pitch, so that the
   * first sound fades in rather than sweeping up from 0Hz */
  m->current = silent_frame(fAX);
  m->current.pitch = SJ_DEFAULT_PITCH;
}

/* bend_factor is the multiplier the Bend control applies to oscillator
 * frequencies. the manual only says that bend goes from a "deep-hollow" to a
 * "High-metallic" sounding voice, so the curve is a guess: a semitone per
 * step either side of the default */
static double msa_bend_factor(const msa *m) {
  return pow(2, (double)(m->bend - SJ_DEFAULT_BEND) / 12);
}

/* the duration multiplier from the Speed control. the manual gives the range
 * and default but not the curve. each step of speed changes the length by
 * about 2%: exp(-0.02 (speed - 114)), the curve the openFPGA-7800 AtariVox
 * engine fitted to recordings of games speaking at speeds 80 to 127 (MIT
 * licence). measurements of the chip behind Stella's SpeakJet engine agree
 * with it to within 8% from 60 to 114. above 114 and below 60 the two
 * disagree with each other, though both make sounds shorter than an inverse
 * law would at the top and longer at speed 40, so the curve is used for every
 * speed */
static double msa_speed_factor(const msa *m) {
  return exp(-0.02 * (double)(m->speed - SJ_DEFAULT_SPEED));
}

/* first frame of a sound after the controls in effect now. used to glide the
 * formants towards the next sound during pauses 1 to 3 */
static bool msa_first_target(uint8_t code, frame *f) {
  sound s;
  if (!lookup(code, &s) || s.nparts == 0)
    return false;
  *f = s.parts[0].target;
  return true;
}

static bool msa_busy(const msa *m) { return m->seg < m->nsegs; }

/* start playing the segments just set up */
static void msa_start(msa *m) {
  m->seg = 0;
  m->seg_pos = 0;
  m->seg_start = m->current;
  if (m->nsegs > 0 && m->segs[0].jump)
    memcpy(m->seg_start.freq, m->segs[0].target.freq, sizeof(m->seg_start.freq));
}

/* expand converts a sound into segments, applying the controls and
 * modifiers */
static void msa_expand(msa *m, const sound *s) {
  double scale = 1.0;
  double pitch = (double)m->pitch;
  double f1 = 1.0, f2 = 1.0;

  if (s->speech) {
    scale = msa_speed_factor(m) * s->length;

    /* fast and slow, measured from the vox_test recording (see
     * STEADY_LENGTH): fast plays a sound at 0.53 to 0.67 of its length (NE,
     * JH, AXRR), and slow at 1.14 to 1.67 times (SE, NE, TH, IY) */
    if (m->fast)
      scale *= 0.6;
    if (m->slow)
      scale *= 1.5;

    /* "STRESS, IH sounds more like (but not quite) the IY sound". stress
     * tenses the vowel (lower F1, higher F2) and raises the tone; relax does
     * the opposite. the amounts are not given by the manual */
    if (m->stress) {
      pitch *= 1.12;
      f1 = 0.94;
      f2 = 1.06;
    } else if (m->relax) {
      pitch *= 0.9;
      f1 = 1.06;
      f2 = 0.94;
    }
  }

  m->nsegs = 0;
  for (int i = 0; i < s->nparts; i++) {
    const part *p = &s->parts[i];
    const double ms = p->ms * scale;
    double glide = p->glide_ms * scale;
    if (p->glide_ms == AUTO_GLIDE)
      glide = fmin2(fmax2(ms * 0.35, 20), 50);
    glide = fmin2(glide, ms);

    frame t = p->target;
    if (s->speech) {
      t.pitch = pitch;
      t.freq[0] *= f1;
      t.freq[1] *= f2;
    }
    segment *sg = &m->segs[m->nsegs++];
    sg->samples = to_samples(ms);
    sg->glide = to_samples(glide);
    sg->target = t;
    sg->jump = false;
  }

  m->fast = m->slow = m->stress = m->relax = false;

  if (m->nsegs > 0 && m->jump_next_freq) {
    m->segs[0].jump = true;
    m->jump_next_freq = false;
  }

  m->copies = m->repeat > 1 ? m->repeat - 1 : 0;
  m->repeat = 0;
}

/* silence ramps the volume down and holds silence. if towards is not NULL
 * the formant frequencies glide to it while the volume ramps */
static void msa_silence(msa *m, double ms, const frame *towards) {
  frame t = m->current;
  for (int i = 0; i < 5; i++)
    t.vol[i] = 0;
  if (towards != NULL) {
    memcpy(t.freq, towards->freq, sizeof(t.freq));
    t.freq[3] = m->current.freq[3];
    t.freq[4] = m->current.freq[4];
  }

  m->segs[0].samples = to_samples(RAMP_MS);
  m->segs[0].glide = to_samples(RAMP_MS);
  m->segs[0].target = t;
  m->segs[0].jump = false;
  m->nsegs = 1;
  if (ms > RAMP_MS) {
    m->segs[1].samples = to_samples(ms - RAMP_MS);
    m->segs[1].glide = 0;
    m->segs[1].target = t;
    m->segs[1].jump = false;
    m->nsegs = 2;
  }
  m->copies = 0;
}

/* autoSilence starts the ramp to silence once the input buffer is empty */
static void msa_auto_silence(msa *m) {
  if (m->auto_silenced)
    return;
  m->auto_silenced = true;
  msa_silence(m, AUTO_SILENCE_MS, NULL);
  msa_start(m);
}

/* next produces one sample from the segments currently queued */
static double msa_next(msa *m) {
  if (!msa_busy(m))
    return synth_sample(&m->synth, &m->current, msa_bend_factor(m),
                        (double)m->volume);

  const segment *sg = &m->segs[m->seg];
  if (sg->glide > 0 && m->seg_pos < sg->glide)
    m->current = lerp(&m->seg_start, &sg->target,
                      (double)(m->seg_pos + 1) / (double)sg->glide);
  else
    m->current = sg->target;

  const double v = synth_sample(&m->synth, &m->current, msa_bend_factor(m),
                                (double)m->volume);

  m->seg_pos++;
  if (m->seg_pos >= sg->samples) {
    m->seg++;
    if (m->seg >= m->nsegs && m->copies > 0) {
      m->copies--;
      m->seg = 0;
    }
    m->seg_pos = 0;
    m->seg_start = m->current;
    if (msa_busy(m) && m->segs[m->seg].jump)
      memcpy(m->seg_start.freq, m->segs[m->seg].target.freq,
             sizeof(m->seg_start.freq));
  }

  return v;
}

/* ======================================================================== */
/* speakjet.go                                                              */
/* ======================================================================== */

typedef struct command {
  uint8_t code;
  uint8_t data;
  int size;
} command;

struct sj_chip {
  /* the input buffer, as a ring of commands. every command takes at least a
   * byte, so there are never more than SJ_BUFFER_SIZE of them */
  command queue[SJ_BUFFER_SIZE];
  int head;
  int count;

  /* bytes in the buffer, including the command that is executing. the
   * manual: "each command is removed from the input buffer after the command
   * is executed" */
  int buffered;

  /* size of the command that is executing */
  int executing;

  bool speaking;

  /* sj_write_byte(): a two byte command waiting for its data byte, or -1 */
  int pending_code;

  msa msa;
};

int sj_command_size(uint8_t code) {
  switch (code) {
  case 20: case 21: case 22: case 23: case 24:
  case 25: case 26: case 28: case 29: case 30:
    return 2;
  default:
    return 1;
  }
}

sj_chip *sj_new(void) {
  sj_chip *sj = (sj_chip *)malloc(sizeof(sj_chip));
  if (sj != NULL)
    sj_power_on(sj);
  return sj;
}

void sj_free(sj_chip *sj) { free(sj); }

void sj_power_on(sj_chip *sj) {
  memset(sj, 0, sizeof(*sj));
  sj->pending_code = -1;
  msa_init(&sj->msa);
}

bool sj_write(sj_chip *sj, uint8_t code, uint8_t data) {
  const int size = sj_command_size(code);

  if (sj->buffered + size > SJ_BUFFER_SIZE || sj->count >= SJ_BUFFER_SIZE)
    return false;

  command *c = &sj->queue[(sj->head + sj->count) % SJ_BUFFER_SIZE];
  c->code = code;
  c->data = data;
  c->size = size;
  sj->count++;
  sj->buffered += size;
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

bool sj_buffer_half_full(const sj_chip *sj) {
  return sj->buffered > SJ_BUFFER_HALF_FULL;
}

bool sj_speaking(const sj_chip *sj) { return sj->speaking; }

int sj_buffered(const sj_chip *sj) { return sj->buffered; }

void sj_reset(sj_chip *sj) {
  sj->head = 0;
  sj->count = 0;
  sj->buffered = sj->executing;
  sj->pending_code = -1;
  msa_reset_controls(&sj->msa);
}

/* pop the next command from the buffer. the command still counts towards the
 * buffer until it has finished executing */
static bool pop(sj_chip *sj, command *c) {
  if (sj->count == 0)
    return false;
  *c = sj->queue[sj->head];
  sj->head = (sj->head + 1) % SJ_BUFFER_SIZE;
  sj->count--;
  sj->executing = c->size;
  return true;
}

static bool peek(const sj_chip *sj, uint8_t *code) {
  if (sj->count == 0)
    return false;
  *code = sj->queue[sj->head].code;
  return true;
}

/* the executing command has finished */
static void retire(sj_chip *sj) {
  sj->buffered -= sj->executing;
  sj->executing = 0;
}

/* execute a single command. the return value is false if the command is not
 * recognised */
static bool execute(sj_chip *sj, uint8_t code, uint8_t data) {
  msa *m = &sj->msa;
  m->auto_silenced = false;

  if (code <= 6) {
    /* pauses 1 to 3 "ramp the volume while the formant frequencies are being
     * changed". pauses 4 to 6 "wait for silence before changing the formant
     * frequencies" */
    frame f;
    const frame *towards = NULL;
    if (code >= 1 && code <= 3) {
      uint8_t next;
      if (peek(sj, &next) && msa_first_target(next, &f))
        towards = &f;
    } else if (code >= 4) {
      m->jump_next_freq = true;
    }
    msa_silence(m, pause_ms[code], towards);
  } else {
    switch (code) {
    case 7: m->fast = true; return true;
    case 8: m->slow = true; return true;
    case 14: m->stress = true; return true;
    case 15: m->relax = true; return true;

    case 20: m->volume = imin(data, (int)MAX_MASTER); return true;
    case 21: m->speed = imin(data, 127); return true;
    case 22: m->pitch = data; return true;
    case 23: m->bend = imin(data, 15); return true;

    case 26: m->repeat = data; return true;

    case 30:
      /* "the number of 10ms intervals to delay before continuing" */
      msa_silence(m, (double)data * 10, NULL);
      break;

    case 31: msa_reset_controls(m); return true;

    case 16: case 24: case 25: case 28: case 29:
      /* wait, port control, port output and the EEPROM phrase commands have
       * no effect on the sound here: there is no serial control protocol to
       * restart after a wait, the output pins are not connected, and the
       * contents of the AtariVox's SpeakJet EEPROM are not known */
      return false;

    default: {
      sound s;
      if (!lookup(code, &s))
        return false;
      msa_expand(m, &s);
      break;
    }
    }
  }

  msa_start(m);
  return true;
}

bool sj_render(sj_chip *sj, float *buf, size_t n) {
  msa *m = &sj->msa;
  bool sounding = false;

  for (size_t i = 0; i < n; i++) {
    while (!msa_busy(m)) {
      if (sj->executing > 0)
        retire(sj);

      command c;
      if (!pop(sj, &c)) {
        msa_auto_silence(m);
        break;
      }

      execute(sj, c.code, c.data);
    }

    buf[i] = (float)msa_next(m);
    sounding = sounding || msa_busy(m) || sj->buffered > 0;
  }

  sj->speaking = sounding;
  return sounding;
}

/* ======================================================================== */
/* save state support                                                       */
/* ======================================================================== */

void sj_get_controls(const sj_chip *sj, sj_controls *c) {
  const msa *m = &sj->msa;
  c->volume = (uint8_t)m->volume;
  c->speed = (uint8_t)m->speed;
  c->pitch = (uint8_t)m->pitch;
  c->bend = (uint8_t)m->bend;
  c->repeat = (uint8_t)m->repeat;
  c->fast = m->fast;
  c->slow = m->slow;
  c->stress = m->stress;
  c->relax = m->relax;
  c->pending_code = sj->pending_code;
}

void sj_set_controls(sj_chip *sj, const sj_controls *c) {
  msa *m = &sj->msa;
  m->volume = imin(c->volume, (int)MAX_MASTER);
  m->speed = imin(c->speed, 127);
  m->pitch = c->pitch;
  m->bend = imin(c->bend, 15);
  m->repeat = c->repeat;
  m->fast = c->fast;
  m->slow = c->slow;
  m->stress = c->stress;
  m->relax = c->relax;
  sj->pending_code = c->pending_code >= 0 && c->pending_code <= 255 &&
                             sj_command_size((uint8_t)c->pending_code) == 2
                         ? c->pending_code
                         : -1;
}
