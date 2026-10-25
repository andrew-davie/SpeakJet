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

/* the most parts any sound has (a traced effect: see fx_points) */
#define MAX_PARTS 41

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
 * the recording's syllable length to within a few milliseconds. the stops and
 * affricates still use these; the other allophones now have their own lengths
 * from a later recording (see allophone_length) */
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
 * measured from the vox_test recording (see sound.length) by comparing each
 * noise sound's level against the vowel beside it, on the chip and here. on
 * the chip hiss sits 16 to 23dB under the vowels; here it was level with them.
 * the corrections: SE 23dB, CH (the SH frame) 16.4dB, TH 11.8dB, FF 13.7dB,
 * stop bursts 19.5dB (TT 23dB, KE 16dB) and aspiration 14.5dB (TT). sounds the
 * recording doesn't have take the mean of the fricatives that it does. the
 * voiceless fricatives now have these levels built into their frames (see
 * frSE()) */
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

/* frames for the fricatives, reused by the affricates and TS.
 *
 * the noise of the voiceless fricatives (here and HE, HO, WH, FF and TH
 * below) is shaped to the VoxRecord recording (see msa_speed_factor()): their
 * spectra on the chip, which the recording has unclipped, were measured in
 * third octaves from 250Hz to 4kHz in two takes (which agree to 2 to 4dB),
 * averaged, and each frame's two noise oscillators
 * fitted to that shape through the output filter here. the chip's hiss is
 * peaked where these are broad: SE and SO at 3.2 to 4kHz, SH at 2 to 3kHz,
 * HE and HO around 1 to 1.5kHz, WH at 0.6 to 0.9kHz. each keeps the overall
 * level the vox_test recording gave it (see FRICATIVE_DB) */
static frame frSE(void) { return noise_frame(lAlveolar, 3494, 3032, 15.2, 0.6, 9); }
static frame frSO(void) { return noise_frame(lAlveolarBak, 3471, 3049, 9.8, 3.2, 10); }
static frame frSH(void) { return noise_frame(lPalatal, 3275, 2532, 7.4, 4.6, 35); }
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

/* LENGTH-TABLE-BEGIN */
/* how much longer the chip plays each allophone than the manual's length, at
 * the default speed. 0 keeps the class's length (see STEADY_LENGTH). fitted
 * to the mean of two takes of the VoxRecord recording (see
 * msa_speed_factor()), which agree to 2.5ms on average: each allophone was
 * played on its own and its audible length on the chip, from where it rises
 * out of silence to where it falls back, was matched here measured the same
 * way. the chip plays the vowels IH to OH for about 140ms, the back vowels and
 * RR 120 to 125ms, the nasals and liquids 90 to 110ms, the voiced fricatives
 * 90 to 110ms and HE, HO, WH and FF about 75ms. the stops and affricates keep
 * their class's length: where a stop's sound starts and ends on a recording
 * says nothing about its silent closure */
static const double allophone_length[] = {
    1.47, 1.62, 1.81, 1.77, 1.88, 1.80, 1.82, 1.79, 1.56,
    1.45, 1.42, 1.34, 1.14, 0.96, 1.03, 0.84, 0.92, 1.09,
    1.19, 1.10, 1.53, 1.12, 1.16, 1.21, 1.14, 1.14, 1.17,
    1.12, 1.06, 1.17, 1.20, 1.17, 1.06, 1.12, 1.26, 1.16,
    1.34, 0.00, 0.80, 1.11, 0.86, 1.04, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 1.00, 1.00, 0.99, 1.02, 2.14, 2.40, 1.12, 0.96,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
};
/* LENGTH-TABLE-END */

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
  case 183: steady(s, 70, noise_frame(FM(400, 1900, 2600), 1446, 3706, 1.3, 1.7, 141)); break; /* HE */
  case 184: steady(s, 70, noise_frame(FM(600, 1000, 2400), 1265, 3569, 1.6, 1.5, 155)); break; /* HO */
  case 185: steady(s, 70, noise_frame(fWW, 3026, 780, 0.4, 1.6, 163)); break; /* WH */
  case 186: steady(s, 70, noise_frame(lLabial, 3847, 3741, 8.9, 15.6, 21)); break; /* FF */
  case 187: steady(s, 40, frSE()); break; /* SE */
  case 188: steady(s, 40, frSO()); break; /* SO */
  case 189: steady(s, 50, frSH()); break; /* SH */
  case 190: steady(s, 40, noise_frame(lDental, 3218, 1576, 3.8, 1.0, 77)); break; /* TH */
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
  if (allophone_length[code - 128] > 0)
    s->length = allophone_length[code - 128];
}

/* effect constructors */

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

static void hold(sound *s, double ms, double glide, frame f) {
  add_part(s, ms, glide, f);
}

/* the manual gives the DTMF tones 95ms, but on the VoxRecord recording (see
 * msa_speed_factor()) all twelve sound for 152 to 154ms */
static void dtmf(sound *s, double low, double high) {
  hold(s, 150, 3, tone(low, high));
  hold(s, 3, 3, quiet(tone(low, high)));
}

/* the manual describes the sound effects only by name and length. the others
 * here are traced from the VoxRecord recording (see msa_speed_factor()), which
 * plays each on its own: every 5ms the strongest frequency between 150Hz and
 * 4kHz and the level were measured, and the trace reduced to the points
 * below, between which the sound glides in a straight line. each point is
 * where it is reached (in ms from the start of the sound), the frequency
 * there, its level in dB (-60 is silence), what kind of sound it is, and for
 * a buzz its pitch. a tone is played as a sine. noise (a flat spectrum) is
 * played as noise around the frequency. a buzz (a pitch, found by
 * autocorrelation, at a third of the strongest frequency or lower) is played
 * voiced at that pitch with oscillator 1 at the frequency: A1, A2, A3 and M2
 * buzz at 86 to 88Hz, the default speech pitch. the recording is clipped,
 * which flattens the levels and hides the waveforms, and a sound with more
 * than one tone at once keeps only its strongest. many are
 * nothing like their names suggest, and many differ in length from the
 * manual: A1 and A2 last nearly a second, and M0 is a 1.2 second tone around
 * 1.8kHz that dies away */
enum { FX_TONE, FX_NOISE, FX_BUZZ };

typedef struct fx_point {
  short ms, freq, db, kind, pitch;
} fx_point;

static const fx_point fx_points[] = {
    /* R0 */
    {5, 783, -13, 0, 0}, {10, 788, -6, 0, 0}, {25, 799, -1, 0, 0},
    {75, 798, -1, 0, 0}, {80, 401, -2, 0, 0}, {95, 401, -10, 0, 0},
    {105, 262, -18, 0, 0},
    /* R1 */
    {5, 1086, -13, 0, 0}, {25, 1101, -1, 0, 0}, {85, 1094, -4, 0, 0},
    {90, 787, -8, 0, 0}, {95, 1086, -13, 0, 0}, {100, 1086, -24, 0, 0},
    /* R2 */
    {5, 483, -13, 0, 0}, {10, 1279, -6, 0, 0}, {25, 1299, -1, 0, 0},
    {80, 1298, -3, 0, 0}, {85, 1114, -4, 0, 0}, {100, 1100, -16, 0, 0},
    {105, 238, -24, 0, 0},
    /* R3 */
    {5, 372, -12, 0, 0}, {10, 1599, -6, 0, 0}, {25, 1599, -1, 0, 0},
    {80, 1598, -3, 0, 0}, {85, 690, -6, 0, 0}, {90, 1592, -10, 0, 0},
    {95, 681, -14, 0, 0}, {100, 210, -23, 0, 0},
    /* R4 */
    {5, 251, -16, 0, 0}, {10, 1891, -7, 0, 0}, {20, 1897, -2, 0, 0},
    {80, 1898, -3, 0, 0}, {85, 727, -5, 0, 0}, {90, 700, -8, 0, 0},
    {95, 382, -11, 0, 0}, {100, 255, -22, 0, 0},
    /* R5 */
    {5, 268, -13, 0, 0}, {10, 2184, -7, 0, 0}, {25, 2198, -1, 0, 0},
    {80, 2197, -3, 0, 0}, {85, 477, -5, 0, 0}, {100, 462, -15, 0, 0},
    /* R6 */
    {5, 357, -18, 0, 0}, {10, 375, -8, 0, 0}, {15, 2498, -5, 0, 0},
    {80, 2498, -4, 0, 0}, {85, 725, -6, 0, 0}, {90, 716, -9, 0, 0},
    {95, 343, -14, 0, 0}, {100, 271, -17, 0, 0},
    /* R7 */
    {5, 779, -14, 0, 0}, {10, 828, -8, 0, 0}, {15, 2794, -5, 0, 0},
    {75, 2797, -3, 0, 0}, {80, 673, -5, 0, 0}, {90, 653, -9, 0, 0},
    {95, 359, -14, 0, 0}, {100, 252, -16, 0, 0},
    /* R8 */
    {5, 620, -17, 0, 0}, {10, 596, -8, 0, 0}, {20, 665, -4, 0, 0},
    {25, 665, -2, 0, 0}, {75, 593, -2, 0, 0}, {100, 589, -13, 0, 0},
    /* R9 */
    {5, 247, -21, 0, 0}, {10, 624, -13, 0, 0}, {20, 646, -5, 0, 0},
    {40, 664, -2, 0, 0}, {75, 599, -2, 0, 0}, {80, 663, -3, 0, 0},
    {85, 572, -4, 0, 0}, {105, 564, -15, 0, 0},
    /* A0 */
    {5, 1647, -21, 2, 1647}, {15, 1647, -10, 2, 1647},
    {35, 1647, -8, 2, 1647}, {40, 1737, -7, 0, 0}, {75, 1938, -7, 0, 0},
    {80, 1956, -7, 1, 0}, {120, 2121, -8, 1, 0}, {125, 237, -8, 1, 0},
    {160, 264, -5, 1, 0}, {165, 264, -4, 0, 0}, {285, 373, -1, 0, 0},
    {310, 369, -2, 0, 0}, {325, 395, -3, 0, 0}, {335, 381, -3, 0, 0},
    {340, 401, -4, 0, 0}, {345, 368, -5, 0, 0}, {360, 368, -14, 0, 0},
    {365, 210, -21, 0, 0},
    /* A1 */
    {5, 679, -10, 0, 0}, {30, 698, -7, 0, 0}, {35, 698, -8, 1, 0},
    {40, 694, -9, 1, 0}, {45, 444, -10, 2, 88}, {70, 436, -14, 2, 88},
    {75, 284, -11, 2, 88}, {110, 278, -7, 2, 88}, {115, 349, -7, 2, 88},
    {145, 359, -6, 2, 88}, {155, 437, -6, 2, 88}, {170, 463, -6, 2, 88},
    {175, 518, -8, 2, 88}, {205, 542, -9, 2, 88}, {210, 612, -8, 2, 88},
    {240, 626, -7, 2, 87}, {245, 702, -6, 2, 87}, {260, 712, -6, 2, 87},
    {265, 788, -7, 2, 87}, {325, 888, -7, 2, 87}, {340, 1038, -6, 2, 87},
    {375, 1059, -5, 2, 87}, {385, 1134, -5, 2, 87}, {525, 1229, -9, 2, 87},
    {535, 1316, -8, 2, 87}, {660, 1405, -10, 2, 87}, {665, 1743, -11, 2, 87},
    {680, 1751, -10, 2, 87}, {690, 1741, -10, 2, 87}, {695, 1486, -11, 2, 87},
    {770, 1479, -11, 2, 87}, {775, 525, -11, 2, 87}, {905, 511, -13, 2, 87},
    {910, 527, -12, 2, 87}, {915, 1654, -12, 2, 87}, {935, 1655, -12, 2, 87},
    {940, 1655, -13, 1, 0}, {965, 1624, -17, 1, 0}, {970, 260, -17, 0, 0},
    {975, 1592, -24, 0, 0},
    /* A2 */
    {5, 235, -21, 0, 0}, {10, 260, -15, 1, 0}, {25, 265, -12, 1, 0},
    {30, 1648, -12, 1, 0}, {35, 1648, -11, 2, 87}, {40, 1648, -12, 2, 87},
    {45, 521, -12, 2, 87}, {205, 526, -11, 2, 87}, {210, 1470, -12, 2, 87},
    {260, 1481, -10, 2, 87}, {265, 1801, -10, 2, 87}, {295, 1730, -10, 2, 87},
    {300, 1407, -11, 2, 87}, {530, 1215, -8, 2, 87}, {615, 1032, -7, 2, 87},
    {635, 862, -8, 2, 87}, {695, 780, -6, 2, 87}, {700, 694, -6, 2, 87},
    {745, 588, -7, 2, 86}, {755, 516, -8, 2, 86}, {785, 509, -7, 2, 86},
    {815, 338, -6, 2, 86}, {860, 327, -8, 2, 86}, {865, 770, -10, 2, 86},
    {870, 332, -9, 2, 86}, {875, 332, -10, 2, 86}, {880, 437, -11, 2, 86},
    {915, 442, -9, 2, 87}, {920, 689, -9, 2, 87}, {935, 690, -7, 2, 87},
    {940, 690, -7, 2, 346}, {945, 346, -8, 1, 0}, {950, 697, -8, 0, 0},
    {965, 675, -13, 0, 0}, {975, 518, -24, 0, 0},
    /* A3 */
    {5, 478, -15, 2, 478}, {15, 478, -7, 2, 478}, {20, 526, -4, 2, 88},
    {35, 526, -4, 2, 88}, {45, 550, -4, 2, 88}, {50, 598, -5, 2, 88},
    {65, 616, -5, 2, 88}, {95, 626, -5, 2, 88}, {100, 691, -4, 2, 88},
    {145, 713, -5, 2, 87}, {155, 787, -5, 2, 87}, {190, 788, -5, 2, 87},
    {205, 873, -4, 2, 87}, {240, 891, -4, 2, 87}, {245, 906, -5, 2, 853},
    {250, 959, -4, 2, 87}, {255, 962, -5, 2, 885}, {260, 901, -5, 2, 901},
    {285, 955, -4, 2, 955}, {290, 979, -5, 2, 87}, {295, 1043, -4, 2, 87},
    {370, 1151, -5, 2, 87}, {375, 1217, -4, 2, 87}, {490, 1402, -5, 2, 87},
    {495, 1410, -5, 2, 468}, {500, 1470, -5, 0, 0}, {525, 1464, -8, 0, 0},
    {530, 1399, -11, 0, 0}, {535, 304, -18, 0, 0}, {540, 258, -19, 0, 0},
    /* A4 */
    {5, 266, -24, 0, 0}, {10, 270, -14, 0, 0}, {35, 267, -4, 0, 0},
    {95, 504, -1, 0, 0}, {130, 955, -1, 0, 0}, {145, 1097, -1, 0, 0},
    {155, 1400, -2, 0, 0}, {160, 1803, -2, 0, 0}, {170, 1975, -3, 0, 0},
    {175, 2382, -3, 0, 0}, {185, 2695, -4, 0, 0}, {190, 2720, -5, 0, 0},
    {195, 1327, -5, 0, 0}, {200, 1327, -5, 0, 0}, {205, 3897, -5, 0, 0},
    {235, 3901, -10, 0, 0}, {255, 3900, -20, 0, 0}, {260, 489, -24, 0, 0},
    /* A5 */
    {5, 637, -8, 2, 637}, {55, 584, -6, 2, 646}, {60, 193, -6, 2, 193},
    {115, 260, -4, 2, 260}, {120, 504, -3, 2, 165}, {125, 496, -2, 2, 165},
    {130, 165, -3, 2, 165}, {135, 451, -4, 2, 451}, {150, 451, -5, 2, 451},
    {155, 220, -5, 2, 220}, {210, 387, -3, 2, 132}, {215, 132, -2, 2, 132},
    {220, 329, -2, 2, 329}, {225, 380, -2, 0, 0}, {275, 334, -1, 0, 0},
    {280, 329, -2, 2, 329}, {285, 334, -2, 2, 329}, {290, 132, -2, 2, 132},
    {300, 132, -3, 2, 132}, {305, 397, -3, 2, 132}, {350, 220, -5, 2, 220},
    {355, 451, -4, 2, 451}, {390, 473, -10, 2, 473}, {395, 501, -6, 2, 260},
    {400, 257, -6, 2, 257}, {415, 260, -5, 2, 260}, {430, 547, -6, 2, 278},
    {435, 553, -6, 0, 0}, {440, 559, -5, 0, 0}, {445, 189, -5, 0, 0},
    {450, 570, -5, 0, 0}, {455, 576, -5, 0, 0}, {460, 583, -6, 2, 194},
    {465, 589, -5, 2, 646}, {555, 629, -10, 2, 629}, {560, 629, -18, 0, 0},
    /* A6 */
    {5, 255, -11, 0, 0}, {10, 273, -8, 0, 0}, {20, 278, -3, 0, 0},
    {30, 375, -2, 0, 0}, {45, 532, -1, 0, 0}, {70, 786, -1, 0, 0},
    {175, 801, -2, 0, 0}, {190, 796, -8, 0, 0}, {200, 796, -24, 0, 0},
    /* A7 */
    {5, 1165, -13, 0, 0}, {20, 1155, -2, 0, 0}, {55, 1231, -1, 0, 0},
    {60, 419, -1, 2, 419}, {65, 419, -1, 2, 419}, {70, 1264, -1, 2, 419},
    {85, 1297, -2, 2, 430}, {90, 1306, -2, 2, 434}, {95, 438, -2, 2, 438},
    {215, 531, -1, 2, 531}, {220, 499, -1, 2, 531}, {225, 537, -1, 2, 537},
    {230, 506, -1, 0, 0}, {240, 511, -1, 0, 0}, {245, 555, -1, 0, 0},
    {250, 516, -1, 0, 0}, {280, 561, -1, 0, 0}, {285, 605, -1, 0, 0},
    {290, 576, -1, 0, 0}, {295, 584, -1, 2, 612}, {300, 620, -1, 2, 620},
    {380, 735, -1, 2, 735}, {550, 937, -2, 2, 937}, {570, 937, -5, 2, 937},
    {590, 937, -18, 2, 937}, {595, 1857, -23, 0, 0},
    /* A8 */
    {5, 266, -21, 0, 0}, {15, 285, -9, 0, 0}, {20, 312, -7, 0, 0},
    {30, 312, -4, 0, 0}, {35, 346, -3, 0, 0}, {45, 346, -2, 0, 0},
    {55, 421, -2, 0, 0}, {60, 492, -1, 2, 492}, {110, 498, -3, 2, 498},
    {115, 417, -3, 0, 0}, {120, 417, -5, 0, 0}, {130, 302, -11, 0, 0},
    {135, 293, -17, 0, 0}, {140, 293, -60, 0, 0}, {145, 286, -24, 0, 0},
    {150, 286, -15, 0, 0}, {155, 360, -11, 0, 0}, {160, 410, -8, 0, 0},
    {170, 410, -6, 0, 0}, {175, 302, -5, 0, 0}, {185, 302, -2, 0, 0},
    {205, 456, -1, 0, 0}, {215, 497, -1, 0, 0}, {255, 499, -2, 0, 0},
    {270, 491, -9, 0, 0}, {275, 369, -17, 0, 0},
    /* A9 */
    {5, 1291, -10, 0, 0}, {20, 1296, -2, 0, 0}, {60, 1288, -1, 0, 0},
    {80, 1194, -1, 0, 0}, {115, 951, -1, 0, 0}, {175, 580, -1, 0, 0},
    {205, 387, -1, 0, 0}, {220, 293, -2, 0, 0}, {225, 293, -4, 0, 0},
    {230, 314, -6, 1, 0}, {235, 609, -7, 1, 0}, {245, 609, -9, 1, 0},
    {250, 326, -10, 1, 0}, {260, 297, -12, 1, 0}, {265, 100, -12, 2, 100},
    {280, 100, -13, 2, 100}, {285, 300, -13, 2, 100}, {290, 100, -14, 2, 100},
    {295, 300, -15, 0, 0}, {315, 277, -23, 0, 0}, {320, 231, -24, 0, 0},
    /* B0 */
    {5, 2570, -8, 0, 0}, {20, 2593, -2, 0, 0}, {130, 2593, -2, 0, 0},
    {155, 2593, -6, 0, 0}, {175, 2588, -19, 0, 0}, {180, 345, -24, 0, 0},
    /* B1 */
    {5, 1493, -14, 0, 0}, {10, 1393, -6, 0, 0}, {20, 1487, -2, 0, 0},
    {55, 2087, -1, 0, 0}, {105, 2949, -2, 0, 0}, {150, 3722, -6, 0, 0},
    {165, 3754, -6, 0, 0}, {230, 2376, -1, 0, 0}, {275, 1429, -1, 0, 0},
    {370, 1400, -2, 0, 0}, {395, 1393, -14, 0, 0}, {400, 507, -23, 0, 0},
    /* B2 */
    {5, 1405, -22, 0, 0}, {10, 1397, -6, 0, 0}, {25, 1406, -1, 0, 0},
    {130, 1417, -1, 0, 0}, {150, 2477, -1, 0, 0}, {160, 2496, -1, 0, 0},
    {165, 838, -1, 0, 0}, {180, 838, -1, 0, 0}, {185, 1699, -1, 0, 0},
    {375, 1701, -2, 0, 0}, {395, 1700, -7, 0, 0}, {410, 1706, -24, 0, 0},
    /* B3 */
    {5, 620, -8, 0, 0}, {20, 594, -2, 0, 0}, {110, 594, -2, 0, 0},
    {115, 629, -1, 2, 629}, {120, 703, -1, 2, 703}, {135, 703, -1, 2, 703},
    {140, 1397, -1, 0, 0}, {280, 1397, -1, 0, 0}, {300, 1397, -6, 0, 0},
    {310, 1396, -14, 0, 0}, {315, 242, -23, 0, 0},
    /* B4 */
    {5, 1771, -8, 0, 0}, {20, 1818, -1, 0, 0}, {65, 1824, -1, 0, 0},
    {70, 612, -1, 2, 612}, {80, 612, -1, 2, 612}, {85, 919, -1, 2, 919},
    {115, 901, -1, 2, 901}, {120, 732, -1, 0, 0}, {140, 498, -1, 0, 0},
    {150, 380, -1, 0, 0}, {160, 380, -1, 0, 0}, {165, 404, -1, 0, 0},
    {180, 586, -1, 0, 0}, {195, 751, -1, 0, 0}, {200, 901, -1, 2, 901},
    {230, 937, -1, 2, 937}, {235, 612, -1, 2, 612}, {245, 612, -1, 2, 612},
    {250, 1824, -1, 0, 0}, {355, 1824, -3, 0, 0}, {370, 1825, -8, 0, 0},
    {385, 1837, -24, 0, 0},
    /* B5 */
    {5, 947, -7, 0, 0}, {15, 947, -2, 0, 0}, {35, 764, -1, 0, 0},
    {60, 520, -1, 0, 0}, {80, 327, -2, 0, 0}, {90, 327, -5, 0, 0},
    {95, 290, -8, 1, 0}, {105, 309, -13, 1, 0}, {125, 312, -16, 1, 0},
    {130, 313, -16, 0, 0}, {145, 313, -19, 0, 0}, {150, 3498, -20, 0, 0},
    {160, 3498, -22, 0, 0}, {165, 488, -24, 0, 0}, {170, 240, -24, 0, 0},
    /* B6 */
    {5, 275, -13, 0, 0}, {10, 3487, -8, 0, 0}, {15, 1165, -6, 2, 1165},
    {25, 1165, -3, 2, 1165}, {30, 1769, -3, 2, 1769}, {70, 1769, -2, 2, 1769},
    {75, 885, -2, 2, 885}, {85, 885, -1, 2, 885}, {90, 1769, -2, 2, 1769},
    {100, 1769, -2, 2, 1769}, {105, 1797, -1, 0, 0}, {150, 1796, -3, 0, 0},
    {175, 1776, -21, 0, 0},
    /* B7 */
    {5, 251, -14, 0, 0}, {10, 2900, -7, 0, 0}, {25, 2900, -2, 0, 0},
    {55, 2896, -5, 0, 0}, {60, 1405, -9, 0, 0}, {65, 1405, -16, 0, 0},
    {70, 1405, -9, 0, 0}, {75, 3897, -7, 0, 0}, {125, 3897, -6, 0, 0},
    {165, 3896, -18, 0, 0}, {170, 394, -22, 0, 0}, {175, 368, -24, 0, 0},
    /* B8 */
    {5, 2054, -10, 0, 0}, {25, 2030, -1, 0, 0}, {100, 1592, -1, 0, 0},
    {105, 1599, -1, 2, 1592}, {235, 1291, -1, 2, 1291}, {240, 1298, -1, 0, 0},
    {285, 1299, -2, 0, 0}, {290, 1291, -2, 2, 1291}, {310, 1291, -5, 2, 1291},
    {325, 1291, -18, 2, 1291},
    /* B9 */
    {5, 901, -14, 2, 901}, {25, 901, -2, 2, 901}, {70, 901, -4, 2, 901},
    {75, 882, -4, 0, 0}, {80, 775, -6, 0, 0}, {90, 759, -4, 0, 0},
    {95, 680, -5, 0, 0}, {100, 664, -4, 0, 0}, {110, 561, -3, 0, 0},
    {115, 561, -3, 0, 0}, {120, 522, -3, 0, 0}, {145, 523, -2, 0, 0},
    {150, 620, -3, 0, 0}, {155, 530, -3, 0, 0}, {160, 547, -3, 0, 0},
    {175, 703, -4, 0, 0}, {180, 712, -3, 0, 0}, {185, 797, -4, 0, 0},
    {190, 818, -4, 0, 0}, {195, 885, -3, 2, 885}, {270, 901, -2, 2, 901},
    {275, 1769, -3, 2, 1769}, {290, 1769, -10, 2, 1769},
    {300, 1769, -21, 2, 1769},
    /* C0 */
    {5, 211, -23, 0, 0}, {10, 1657, -23, 0, 0}, {15, 1691, -9, 0, 0},
    {20, 2201, -7, 0, 0}, {30, 2201, -4, 0, 0}, {35, 1965, -5, 0, 0},
    {40, 1953, -4, 0, 0}, {45, 1697, -4, 0, 0}, {50, 1692, -4, 0, 0},
    {55, 1433, -3, 0, 0}, {65, 1426, -3, 0, 0}, {70, 1168, -4, 0, 0},
    {75, 1162, -3, 0, 0}, {80, 911, -4, 0, 0}, {85, 905, -4, 0, 0},
    {90, 802, -3, 0, 0}, {100, 715, -3, 0, 0}, {105, 612, -4, 0, 0},
    {110, 589, -4, 0, 0}, {115, 439, -3, 0, 0}, {125, 412, -4, 0, 0},
    {130, 310, -6, 0, 0}, {140, 310, -8, 0, 0}, {145, 345, -14, 0, 0},
    {155, 363, -17, 0, 0}, {160, 345, -21, 0, 0}, {165, 286, -19, 0, 0},
    {185, 276, -21, 0, 0}, {195, 332, -21, 0, 0}, {200, 569, -23, 0, 0},
    {210, 569, -24, 0, 0}, {215, 317, -24, 0, 0},
    /* C1 */
    {5, 749, -7, 0, 0}, {15, 912, -3, 0, 0}, {20, 1366, -2, 0, 0},
    {35, 1393, -1, 0, 0}, {40, 1005, -1, 0, 0}, {45, 964, -1, 0, 0},
    {50, 690, -1, 1, 0}, {60, 690, -1, 1, 0}, {65, 760, -1, 1, 0},
    {70, 713, -1, 1, 0}, {75, 713, -1, 0, 0}, {85, 703, -2, 0, 0},
    {90, 1342, -2, 0, 0}, {95, 1995, -1, 0, 0}, {110, 1978, -1, 0, 0},
    {120, 1763, -1, 0, 0}, {125, 1837, -1, 0, 0}, {130, 1707, -1, 0, 0},
    {160, 1936, -1, 0, 0}, {165, 1896, -1, 0, 0}, {170, 1837, -1, 0, 0},
    {175, 1352, -2, 0, 0}, {195, 1190, -2, 0, 0}, {220, 1140, -3, 0, 0},
    {235, 979, -4, 0, 0}, {240, 853, -5, 2, 853}, {260, 853, -9, 2, 853},
    {265, 2776, -10, 0, 0}, {290, 2785, -19, 0, 0}, {295, 343, -21, 0, 0},
    /* C2 */
    {5, 464, -9, 2, 464}, {25, 464, -6, 2, 464}, {70, 586, -5, 2, 569},
    {125, 569, -9, 2, 569}, {130, 569, -11, 2, 569}, {135, 569, -23, 2, 569},
    {140, 569, -22, 2, 569}, {145, 3336, -20, 0, 0}, {175, 3338, -8, 0, 0},
    {180, 1647, -9, 2, 1647}, {205, 1687, -12, 2, 1647},
    {210, 1647, -24, 0, 0},
    /* C3 */
    {5, 368, -24, 0, 0}, {10, 3158, -21, 0, 0}, {25, 3176, -12, 0, 0},
    {55, 3161, -7, 0, 0}, {160, 3771, -10, 0, 0}, {215, 3778, -14, 0, 0},
    {220, 392, -17, 0, 0}, {225, 392, -24, 0, 0}, {235, 382, -24, 0, 0},
    /* C4 */
    {5, 254, -22, 0, 0}, {10, 939, -21, 0, 0}, {20, 949, -7, 0, 0},
    {35, 961, -4, 0, 0}, {100, 957, -4, 0, 0}, {105, 955, -4, 2, 955},
    {125, 955, -4, 2, 955}, {130, 1914, -5, 2, 1911},
    {145, 1915, -5, 2, 1911}, {150, 975, -5, 2, 975}, {170, 995, -6, 2, 995},
    {175, 2957, -7, 0, 0}, {205, 2973, -7, 0, 0}, {230, 2963, -18, 0, 0},
    {235, 526, -20, 0, 0},
    /* C5 */
    {5, 885, -20, 2, 885}, {10, 885, -10, 2, 885}, {15, 2696, -7, 2, 885},
    {55, 2692, -3, 2, 885}, {60, 759, -3, 2, 869}, {65, 733, -2, 0, 0},
    {75, 694, -1, 0, 0}, {80, 713, -1, 0, 0}, {85, 647, -1, 2, 713},
    {90, 308, -1, 2, 308}, {150, 291, -2, 2, 291}, {155, 485, -2, 0, 0},
    {190, 371, -2, 0, 0}, {195, 312, -2, 0, 0}, {200, 303, -2, 0, 0},
    {215, 297, -2, 0, 0}, {230, 314, -5, 0, 0}, {235, 302, -6, 0, 0},
    {240, 518, -11, 2, 3981}, {245, 518, -13, 2, 3981},
    {250, 521, -13, 2, 87}, {325, 510, -14, 2, 87}, {330, 282, -14, 2, 87},
    {335, 276, -16, 0, 0}, {345, 237, -24, 0, 0},
    /* C6 */
    {5, 2039, -13, 0, 0}, {65, 1933, -4, 0, 0}, {70, 1553, -4, 0, 0},
    {75, 1524, -4, 0, 0}, {80, 1281, -3, 0, 0}, {85, 1212, -3, 0, 0},
    {90, 991, -2, 0, 0}, {95, 983, -2, 0, 0}, {100, 903, -1, 0, 0},
    {110, 903, -2, 0, 0}, {115, 995, -2, 2, 995}, {150, 995, -3, 2, 995},
    {155, 905, -2, 0, 0}, {165, 912, -2, 0, 0}, {175, 1250, -2, 0, 0},
    {180, 1253, -2, 0, 0}, {185, 1480, -3, 0, 0}, {190, 1508, -3, 0, 0},
    {195, 1822, -4, 0, 0}, {210, 2040, -6, 0, 0}, {235, 2037, -6, 0, 0},
    {265, 2033, -15, 0, 0},
    /* C7 */
    {5, 519, -10, 0, 0}, {15, 501, -7, 0, 0}, {55, 549, -4, 0, 0},
    {60, 555, -3, 2, 555}, {80, 569, -4, 2, 569}, {85, 2277, -4, 0, 0},
    {100, 2299, -5, 0, 0}, {105, 2114, -3, 0, 0}, {110, 1231, -4, 0, 0},
    {125, 1231, -3, 0, 0}, {130, 1677, -2, 0, 0}, {145, 1681, -2, 0, 0},
    {150, 1427, -2, 0, 0}, {155, 893, -1, 0, 0}, {165, 893, -1, 0, 0},
    {170, 1223, -1, 0, 0}, {175, 1271, -1, 0, 0}, {185, 1652, -1, 0, 0},
    {195, 1652, -1, 0, 0}, {200, 797, -1, 0, 0}, {215, 819, -1, 0, 0},
    {225, 1276, -1, 0, 0}, {240, 1299, -1, 0, 0}, {245, 568, -1, 0, 0},
    {250, 568, -1, 0, 0}, {255, 242, -1, 2, 242}, {265, 241, -1, 2, 241},
    {270, 960, -1, 2, 241}, {285, 965, -1, 2, 241}, {290, 462, -1, 0, 0},
    {315, 435, -1, 0, 0}, {320, 478, -1, 0, 0}, {325, 478, -1, 0, 0},
    {330, 483, -1, 2, 483}, {340, 496, -2, 2, 483}, {345, 483, -2, 0, 0},
    {355, 479, -3, 0, 0}, {360, 504, -4, 0, 0}, {380, 491, -13, 0, 0},
    /* C8 */
    {5, 218, -15, 0, 0}, {10, 932, -7, 0, 0}, {15, 1295, -4, 0, 0},
    {20, 1727, -2, 1, 0}, {40, 1702, -1, 1, 0}, {45, 1191, -1, 1, 0},
    {55, 2470, -1, 1, 0}, {60, 1702, -1, 1, 0}, {65, 1378, -1, 1, 0},
    {70, 2619, -1, 1, 0}, {75, 2469, -1, 1, 0}, {80, 1378, -1, 1, 0},
    {85, 1182, -1, 1, 0}, {90, 1863, -1, 1, 0}, {105, 1863, -8, 1, 0},
    {110, 1863, -21, 0, 0}, {115, 479, -24, 0, 0},
    /* C9 */
    {5, 927, -8, 0, 0}, {10, 944, -5, 0, 0}, {15, 1725, -3, 1, 0},
    {25, 1725, -1, 1, 0}, {30, 2311, -1, 1, 0}, {35, 2290, -1, 1, 0},
    {40, 1164, -1, 1, 0}, {45, 1151, -1, 1, 0}, {50, 1898, -1, 1, 0},
    {55, 1582, -1, 1, 0}, {60, 1037, -1, 1, 0}, {65, 983, -1, 1, 0},
    {70, 1396, -1, 1, 0}, {80, 1396, -5, 1, 0}, {85, 1396, -9, 0, 0},
    {90, 1396, -19, 0, 0}, {95, 332, -24, 0, 0},
    /* M0 */
    {5, 1769, -9, 0, 0}, {25, 1789, -1, 0, 0}, {545, 1789, -2, 0, 0},
    {1200, 1764, -24, 0, 0},
    /* M2 */
    {5, 292, -12, 0, 0}, {10, 531, -12, 0, 0}, {15, 457, -7, 0, 0},
    {25, 510, -5, 0, 0}, {35, 530, -5, 0, 0}, {60, 537, -4, 0, 0},
    {65, 595, -3, 0, 0}, {70, 615, -4, 0, 0}, {100, 617, -4, 0, 0},
    {105, 682, -5, 0, 0}, {110, 699, -4, 0, 0}, {140, 715, -4, 0, 0},
    {150, 790, -5, 0, 0}, {155, 758, -4, 0, 0}, {175, 824, -4, 0, 0},
    {180, 793, -4, 0, 0}, {190, 869, -4, 0, 0}, {220, 887, -5, 0, 0},
    {230, 965, -5, 0, 0}, {300, 940, -5, 0, 0}, {310, 862, -5, 0, 0},
    {340, 858, -4, 0, 0}, {345, 816, -5, 0, 0}, {350, 810, -4, 2, 810},
    {355, 777, -5, 2, 87}, {380, 772, -5, 2, 87}, {385, 766, -3, 0, 0},
    {395, 693, -3, 0, 0}, {420, 683, -4, 0, 0}, {430, 611, -3, 0, 0},
    {460, 592, -4, 0, 0}, {470, 512, -5, 0, 0}, {510, 499, -4, 0, 0},
    {525, 491, -10, 0, 0},
};

/* where each effect's points start in fx_points, and how many there are,
 * for codes 200 to 254. effects without points are built in effect() */
static const struct {
  short first, count;
} fx_index[] = {
    {0, 7}, {7, 6}, {13, 7}, {20, 8}, {28, 8}, {36, 6}, {42, 8}, {50, 8},
    {58, 6}, {64, 8}, {72, 18}, {90, 40}, {130, 35}, {165, 30}, {195, 18},
    {213, 36}, {249, 9}, {258, 26}, {284, 26}, {310, 21}, {331, 6}, {337, 12},
    {349, 12}, {361, 11}, {372, 22}, {394, 15}, {409, 13}, {422, 12},
    {434, 10}, {444, 24}, {468, 32}, {500, 30}, {530, 12}, {542, 9},
    {551, 15}, {566, 25}, {591, 22}, {613, 39}, {652, 17}, {669, 17}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {686, 4}, {0, 0}, {690, 34},
};

static frame fx_frame(const fx_point *p) {
  const double g = pow(10, p->db / 20.0);
  if (p->db <= -60)
    return quiet(tone(p->freq, 0));
  if (p->kind == FX_NOISE)
    return noise_frame(lGlottal, p->freq * 0.85, p->freq * 1.15, 24 * g, 18 * g, 200);
  frame f = tone(p->freq, 0);
  f.vol[0] *= g;
  if (p->kind == FX_BUZZ) {
    f.pitch = p->pitch;
    f.voiced = true;
  }
  return f;
}

/* contour plays a traced effect: a quick start on the first point, then a
 * glide to each of the others in turn, then 3ms to silence */
static void contour(sound *s, const fx_point *p, int n) {
  hold(s, p[0].ms, 2, fx_frame(&p[0]));
  for (int i = 1; i < n; i++) {
    const double ms = p[i].ms - p[i - 1].ms;
    hold(s, ms, ms, fx_frame(&p[i]));
  }
  hold(s, 3, 3, quiet(fx_frame(&p[n - 1])));
}

/* sound effects, codes 200 to 254 */
static const char *const effect_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "A0",
    "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9", "B0", "B1",
    "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "C0", "C1", "C2",
    "C3", "C4", "C5", "C6", "C7", "C8", "C9", "D0", "D1", "D2", "D3",
    "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "M0", "M1", "M2",
};

#define NUM_EFFECTS ((int)(sizeof(effect_names) / sizeof(effect_names[0])))

static void effect(sound *s, int code) {
  if (fx_index[code - 200].count > 0) {
    contour(s, &fx_points[fx_index[code - 200].first], fx_index[code - 200].count);
    return;
  }
  switch (code) {
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

/* pause lengths in milliseconds for codes 0 to 6. the manual gives 0, 100,
 * 200, 700, 30, 60 and 90. two takes of the VoxRecord recording (see
 * msa_speed_factor()) measure the silence between two AWs, against the two
 * delays (whose length is exact) as a reference: pauses 3, 5 and 6 are
 * shorter than the manual says, about 650, 45 and 70ms. the takes differ by
 * up to 35ms on a pause, so the others, within 10ms of the manual, keep it */
static const double pause_ms[] = {0, 100, 200, 650, 30, 45, 70};

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

  /* the sound playing is speech rather than a sound effect: Bend works
   * differently on the two (see msa_effect_bend_hz()) */
  bool speech;

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
  m->speech = true;
}

/* bend_factor is the multiplier the Bend control applies to oscillator
 * frequencies in speech. the manual only says that bend goes from a
 * "deep-hollow" to a "High-metallic" sounding voice, so the curve is a guess:
 * a semitone per step either side of the default. the recording's clipped
 * vowels can't confirm it. sound effects are bent by msa_effect_bend_hz()
 * instead */
static double msa_bend_factor(const msa *m) {
  if (!m->speech)
    return 1.0;
  return pow(2, (double)(m->bend - SJ_DEFAULT_BEND) / 12);
}

/* on a sound effect Bend doesn't multiply the frequencies but adds to them.
 * measured from two takes of the VoxRecord recording (see
 * msa_speed_factor()), which plays D1 and A5 at Bends 0, 1, 5, 10 and 15:
 * D1's two tones move together and keep their 512Hz spacing, by -198, -154,
 * 0, +220 and +439Hz, and A5 by the same at Bends 0 and 1. that is 44Hz a step
 * from 5 up, and from 1 down; between 1 and 5 it is 22Hz short, which these
 * five Bends can't place, so the shortfall is taken up at 5. the offset is
 * applied to oscillators 1 to 3, the tones; whether it moves the noise
 * oscillators isn't known */
static double msa_effect_bend_hz(const msa *m) {
  if (m->bend >= SJ_DEFAULT_BEND)
    return 44.0 * (m->bend - SJ_DEFAULT_BEND);
  return 44.0 * m->bend - 198.0;
}

/* the duration multiplier from the Speed control. the manual gives the range
 * and default but not the curve. measured from a recording of a real
 * AtariVox playing the VoxRecord ROM (TT AW EYIY SE at speeds 0 to 127): the
 * length falls in a straight line with speed, 2115ms at speed 0 and 955ms at
 * 80, and the line predicts speeds 20, 40 and 60 to within 2%. it reaches
 * zero at about 146, so relative to the default speed the multiplier is
 * (146 - speed) / 32: 4.6 at speed 0, 0.59 at 127. pauses are lengthened by
 * it too (see execute()) */
#define SPEED_ZERO 146.0
static double msa_speed_factor(const msa *m) {
  return (SPEED_ZERO - (double)m->speed) / (SPEED_ZERO - SJ_DEFAULT_SPEED);
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

  m->speech = s->speech;
  const double bend_hz = s->speech ? 0 : msa_effect_bend_hz(m);

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
    } else {
      for (int j = 0; j < 3; j++)
        if (t.freq[j] > 0)
          t.freq[j] = fmax2(t.freq[j] + bend_hz, 0);
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
    /* on the chip a pause stretches with the Speed control just as the
     * sounds do: at speed 0 two pause 2s are 1845ms of silence */
    msa_silence(m, pause_ms[code] * msa_speed_factor(m), towards);
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
