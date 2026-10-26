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
typedef enum { ENV_SAW, ENV_SINE, ENV_TRIANGLE, ENV_SQUARE, ENV_VOICE } envelope_wave;

/* how long the voicing ramp lasts (see ENV_VOICE in synth_sample()) */
#ifndef VOICE_ENV_MS
#define VOICE_ENV_MS 8.0
#endif

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
/* the low pass after the PWM output (the manual places a "two pole" filter
 * there). the openFPGA-7800 AtariVox engine (MIT licence) fitted a 2-pole
 * 2200Hz corner to recordings of games, and Stella's SpeakJet engine measured
 * 1700Hz. the VoxRecord recording's third take measures it directly: the
 * sound effects that sweep a single pure tone (B1 from 1.4 to 3.8kHz and
 * back, and nine others) lose level with frequency alone, and solving for one
 * response common to all of them gives it flat to 1.5kHz, then -2.2dB at
 * 1.9kHz, -5.7 at 2.4, -10.8 at 2.9, -15.8 at 3.6 and -17.2 at 3.9. that is
 * the shape of a third-order butterworth at 2000Hz, but at this sample rate
 * a recursive filter can't keep it that close to 4096Hz, so this is an
 * 11-tap linear-phase FIR designed to it (within 0.9dB RMS of the
 * measurements). it includes whatever filtering the AtariVox and the
 * recorder add */
#define OUTPUT_TAPS 11
static const double output_fir[OUTPUT_TAPS] = {
    0.0070836153,  -0.0046812003, -0.0276894693, -0.0319246056,
    0.2395192408,  0.6353848384,  0.2395192408,  -0.0319246056,
    -0.0276894693, -0.0046812003, 0.0070836153,
};

typedef struct synth {
  /* phase of oscillators 1 to 5 and of the envelope oscillator, in cycles */
  double phase[5];
  double env_phase;

  /* noise source for distortion (xorshift) */
  uint32_t noise;

  /* output filter history (newest first) and dc blocker state */
  double hist[OUTPUT_TAPS];
  double dc_in, dc_out;
} synth;


static void synth_init(synth *s) {
  memset(s, 0, sizeof(*s));
  s->noise = 0x2545f491u;
}

static double synth_random(synth *s) {
  s->noise ^= s->noise << 13;
  s->noise ^= s->noise >> 17;
  s->noise ^= s->noise << 5;
  return (double)(int32_t)s->noise / SJ_MAX_INT32;
}

/* sample produces the next output sample, in the range -1 to 1. master is
 * the master volume (0 to 127) */
static double synth_sample(synth *s, const frame *f, double master) {
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
    case ENV_VOICE:
      /* the chip's voicing: each pulse restarts the formant oscillators at
       * full level and they fall away in a straight line to nothing in a
       * fixed time, whatever the pitch. below 125Hz the rest of the
       * period is silent; above it the next pulse cuts the ramp short.
       * measured from the VoxProbe recording, where at pitch 40 to 70 each
       * pulse stands alone */
      env = fmax2(1 - p / (f->pitch * VOICE_ENV_MS / 1000), 0);
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
    const double fr = fmin2(f->freq[i], MAX_FREQUENCY);
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
    const double fr = fmin2(f->freq[i], MAX_FREQUENCY);
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
  for (int i = OUTPUT_TAPS - 1; i > 0; i--)
    s->hist[i] = s->hist[i - 1];
  s->hist[0] = y;
  double out = 0;
  for (int i = 0; i < OUTPUT_TAPS; i++)
    out += output_fir[i] * s->hist[i];

  return out;
}

/* ======================================================================== */
/* tables.go                                                                */
/* ======================================================================== */

/* the most parts any sound has (a traced effect: see fx_points) */
#define MAX_PARTS 64

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
 *   MM: F1 F3      EY: F1 F2 F3   EH: F1 F2      AW: F2
 *
 * the third take of the VoxRecord recording (see msa_speed_factor()), whose
 * vowels aren't clipped, confirms AX, UX, EH, EY, IH, OW, UH, LO and RR to
 * within about 5% (formants read by linear prediction, the same way on the
 * recording and on this library's own output, so that the method's errors
 * cancel). it corrects five: AW F1 (590 to 695Hz), UW F1 and F2 (300 to 380,
 * 870 to 1065), AY F1 and F2 (660 to 710, 1720 to 1660), OH F1 and F2 (730
 * to 690, 1090 to 1150) and WW F1 and F2 (300 to 495, 650 to 765). IY
 * can't be read this way. the nasals (MM among them) are fitted to its
 * spectra instead (see allophone()) */
typedef struct formants {
  double f[3];
} formants;

#define FM(a, b, c) ((formants){{(a), (b), (c)}})

static const formants fIY = {{270, 2290, 3010}};
static const formants fIH = {{322, 1774, 2490}};
static const formants fEY = {{508, 1820, 2425}};
static const formants fEH = {{600, 1666, 2480}};
static const formants fAY = {{710, 1660, 2410}};
static const formants fAX = {{521, 1510, 2395}};
static const formants fUX = {{613, 1328, 2390}};
static const formants fOH = {{690, 1150, 2440}};
static const formants fAW = {{695, 998, 2540}};
static const formants fOW = {{512, 891, 2350}};
static const formants fUH = {{440, 1020, 2240}};
static const formants fUW = {{380, 1065, 2240}};
static const formants fAH = {{700, 1220, 2600}};
static const formants fER = {{480, 1350, 1650}};
static const formants fRR = {{439, 1318, 1720}};
static const formants fLE = {{360, 1300, 2700}};
static const formants fLO = {{400, 900, 2500}};
static const formants fWW = {{495, 765, 2200}};

/* the start of AWRR, OWRR and OWIY keeps the earlier OH and AW: the third
 * take's corrections to OH and AW (see above) put those three 1 to 3dB
 * further from the chip's spectra, where the diphthongs that start from OH
 * (OHIY, OHIH) came closer */
static const formants fOHr = {{730, 1090, 2440}};
static const formants fAWr = {{590, 998, 2540}};
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
  r.wave = ENV_VOICE;
  r.voiced = true;
  return r;
}

static frame vowel_frame(formants f) { return voiced_frame(f, 24, 18, 10); }
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
  r.wave = ENV_VOICE;
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

/* the chip starts SE, SO and SH with a short low thump before the hiss,
 * about 10ms of a tone near 400 to 600Hz (in both unclipped takes of the
 * VoxRecord recording). onset_ms of the hiss frame with that tone added, then
 * the hiss alone */
static void thumped(sound *s, double onset_ms, double hz, double vol, double ms,
                    frame f) {
  frame o = f;
  o.freq[0] = hz;
  o.vol[0] = vol;
  s->speech = true;
  s->length = STEADY_LENGTH;
  add_part(s, onset_ms, AUTO_GLIDE, o);
  add_part(s, ms, 3, f);
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
  /* the noise widths (distortion) of the burst and the aspiration, the
   * aspiration's volumes, and for voiced stops the voice bar's volume during
   * the closure and a scale on the voicing after the release */
  double burst_dist, asp_dist;
  double asp_vol[2];
  double closure_voice, onset_voice;
} stop;

static void plosive(sound *s, const stop *st) {
  frame close;
  if (st->voiced)
    close = voiced_frame(FM(180, st->locus.f[1], st->locus.f[2]), st->closure_voice, 0, 0);
  else
    close = silent_frame(st->locus);

  frame burst = hush(noise_frame(st->locus, st->burst_freq[0], st->burst_freq[1],
                                 st->burst_vol[0], st->burst_vol[1], st->burst_dist),
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
      add_part(s, rest, rest,
               voiced_frame(st->locus, 14 * st->onset_voice, 6 * st->onset_voice,
                            3 * st->onset_voice));
  } else if (rest > 0 && st->asp_freq[0] + st->asp_freq[1] == 0) {
    /* no aspiration: silence. noise oscillators at 0Hz would hold whatever
     * phase the burst left them at, a step the dc blocker turns into a thump */
    add_part(s, rest, 4, silent_frame(st->locus));
  } else if (rest > 0) {
    const frame asp = hush(
        noise_frame(st->locus, st->asp_freq[0], st->asp_freq[1], st->asp_vol[0],
                    st->asp_vol[1], st->asp_dist),
        ASPIRATION_DB);
    add_part(s, rest, 4, asp);
  }
}

/* an affricate is a stop released into a fricative */
static void affricate(sound *s, const stop *st, frame fric, double fric_ms) {
  plosive(s, st);
  add_part(s, fric_ms, 5, fric);
}

/* frames for the fricatives SE, SO and SH. the noise of the voiceless
 * fricatives (here and HE, HO, WH, FF and TH below) is shaped to the VoxRecord
 * recording (see msa_speed_factor()). HE, HO and WH have their spectra on the
 * chip, measured in third octaves from 250Hz to 4kHz in three takes, fitted
 * through the output filter here. SE, SO, SH, FF and TH, like the stops and
 * affricates (see STOPN()), were fitted further, as whole sounds: each was
 * played here from a set of values, which a search (differential evolution)
 * moved to bring its spectrogram closest to the chip's in the first and third
 * takes together, and its length to the chip's. two takes rather than one,
 * because on the hiss sounds the chip's own takes match each other only 30 to
 * 70%, so fitting one fits that take's noise. the chip's hiss is narrow bands,
 * peaked where these were broad: SE and SO at 3 to 4kHz, SH at 2.5 to  3.2kHz,
 * HE and HO around 1 to 1.5kHz, WH at 0.6 to 0.9kHz. their levels are set by
 * allophone_gain_db */
static frame frSE(void) { return noise_frame(lAlveolar, 3222, 3786, 5.4, 10.3, 23); }
static frame frSO(void) { return noise_frame(lAlveolarBak, 3088, 3412, 2.7, 5.3, 24); }
static frame frSH(void) { return noise_frame(lPalatal, 3128, 2558, 28.4, 23.7, 61); }
static frame frZH(void) {
  return hush(voiced_noise_frame(FM(260, 1800, 2500), 14, 2400, 3200, 14, 10, 200),
              FRICATIVE_DB);
}

/* stop descriptions: duration, closure, burst, voiced, locus, burst
 * frequencies, burst volumes, aspiration frequencies. STOP() gives a stop the
 * first noise widths, aspiration volumes and voicing (see stop), and STOPN()
 * gives them too. the stops that use STOPN() (BE, EB, OB, ED, OD, TT, TU, KE,
 * EK and OK) and the affricates (CH, JH and TS) were fitted as whole sounds to
 * the VoxRecord recording (see frSE()). the chip's bursts and aspiration are
 * narrower noise than first chosen here (TT and TU hiss at 2.6 to 3.9kHz,
 * which the first values spread from 1 to 2kHz), its voiced stops' loci differ
 * from the published ones, and EB, OB, ED and OD last 25 to 35ms on the chip,
 * not 10. a closure that is silent can't be measured from a sound played
 * alone, so those keep the length they had (35 to 42ms, 7ms for ED and OD),
 * and TT's comes from the connected groups (see allophone_length). the other
 * stops' burst and aspiration frequencies were scaled together (by 0.5 to
 * 1.15) to bring each spectrum closest to the recording's third take; most of
 * the chip's bursts are lower than first chosen here (PE and PO by half) */
#define STOP(ms_, cl_, bu_, vo_, lo_, bf0, bf1, bv0, bv1, af0, af1)            \
  STOPN(ms_, cl_, bu_, vo_, lo_, bf0, bf1, bv0, bv1, af0, af1, 255, 230, 10, 8, \
        8, 1)

/* a stop with its noise widths, aspiration volumes and voicing given too
 * (see stop) */
#define STOPN(ms_, cl_, bu_, vo_, lo_, bf0, bf1, bv0, bv1, af0, af1, bd, ad,    \
              av0, av1, cv, ov)                                                \
  (stop) {                                                                     \
    (ms_), (cl_), (bu_), (vo_), (lo_), {(bf0), (bf1)}, {(bv0), (bv1)},         \
        {(af0), (af1)}, (bd), (ad), {(av0), (av1)}, (cv), (ov)                 \
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

/* how much longer than the manual's length the chip plays the vowels,
 * nasals, liquids and diphthongs (codes 128 to 164): a fixed time, rather
 * than a multiple of the length. repeating a sound on the chip (the VoxProbe
 * recording) adds just the manual's length for each extra copy (AW 70ms; IY,
 * UW and MM six times are one of them and five times 70ms, to within 6ms),
 * so the first copy is the manual's length and a fixed time more. 28ms
 * matches the VoxRecord recording's third take nearly as well as a length
 * fitted to each of the 37 sounds did: sounds played alone within 13ms on
 * average (13ms before), the 30 connected groups within 15ms (13ms). it is
 * stretched by Speed with the rest of the sound */
#define TRANSITION_MS 28.0

/* LENGTH-TABLE-BEGIN */
/* how much longer the chip plays each allophone than the manual's length, at
 * the default speed, fitted to the VoxRecord recording's third take in two
 * steps. first each allophone was played on its own and its audible length on
 * the chip, from where it rises out of silence to where it falls back, was
 * matched here measured the same way. but strung together the chip gives
 * sounds longer slots than that (MM IY MM: 320ms on the chip, 235 here), so
 * then one multiplier for each class was fitted to 30 connected groups (AW
 * MM at eight pitches, AW SE at eight bends, TT AW EYIY SE at five speeds,
 * and nine syllables such as HE OH TT and MM IY MM): vowels 0.92, nasals
 * 1.32, diphthongs 1.10, voiceless fricatives 1.14, stops and affricates 1.40
 * (on the class length). that takes the groups from 6% short to within 1%
 * (16ms on average; 22ms when each is left out of the fit), at the cost of
 * sounds played alone running 13ms long on average. the liquids and voiced
 * fricatives, which no group has, keep their first-step lengths.
 *
 * the stops, the affricates and the voiceless fricatives SE, SO, SH, FF and TH
 * that were fitted as whole sounds (see frSE()) have their lengths from that
 * fit instead, matched to each played alone, with TT and SE also fitted to the
 * connected groups. that takes the groups to within 2% (12ms on average)
 *
 * the vowels, nasals, liquids and diphthongs (codes 128 to 164) are 0 here:
 * they are the manual's length and TRANSITION_MS more */
static const double allophone_length[] = {
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.82, 0.84, 0.99, 0.88, 0.88, 1.63, 1.40, 2.67,
    3.27, 1.40, 1.40, 3.74, 3.16, 1.40, 1.40, 1.40, 1.40,
    2.46, 1.17, 1.15, 1.11, 1.49, 2.92, 3.14, 1.78, 1.77,
    2.07, 2.78, 1.37, 2.44, 1.40, 2.40, 2.46, 1.40, 1.40,
};
/* LENGTH-TABLE-END */

/* how much louder (in dB) the chip plays each allophone than the oscillator
 * volumes here give, against AW. fitted to the VoxRecord recording's third
 * take, whose vowels aren't clipped: each allophone's level (where it is
 * within 20dB of its loudest) was matched to the recording's, measured the
 * same way, relative to AW. the vowels needed little; the nasals, liquids and
 * hiss were too loud here (the hiss levels were set from an earlier, clipped
 * recording, which made the vowels read quiet) and BE, BO, PE and PO too
 * quiet */
static const double allophone_gain_db[] = {
    0.8, 1.4, 1.4, 0.6, 1.2, 1.1, 0.7, 0.7, 0.0,
    -0.9, -1.4, -1.4, -3.8, -4.3, -4.2, -4.2, -4.4, -2.8,
    -2.8, -1.9, 0.6, 0.4, 0.8, 0.6, -0.1, -0.7, 0.9,
    0.4, -0.7, 0.6, 0.5, -1.0, -0.7, 0.2, -1.2, -0.8,
    -2.2, -11.8, -1.7, 2.4, -1.6, -2.6, 6.5, 6.9, -2.7,
    2.5, 3.6, 3.6, -4.5, -8.5, 0.2, 0.1, 2.2, 2.7,
    -15.4, -32.0, -31.1, -29.7, -26.0, -14.8, -19.9, -20.9, -26.7,
    -2.0, -3.2, -26.2, 1.6, -2.9, 2.6, -3.7, 6.5, 6.0,
};

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
  /* the nasals' resonances and volumes are fitted to the VoxRecord recording's
   * third take (the middle of each, in sixth octaves from 200Hz to 3.9kHz):
   * besides the low murmur the chip's nasals have strong resonances near 1.4
   * to 1.6kHz and 2.2 to 2.6kHz that the published values here lacked */
  case 140: steady(s, 70, voiced_frame(FM(396, 1388, 2199), 22.0, 8.6, 1.8)); break; /* MM */
  case 141: steady(s, 70, voiced_frame(FM(310, 1564, 2570), 22.0, 7.0, 11.6)); break; /* NE */
  case 142: steady(s, 70, voiced_frame(FM(310, 1097, 2568), 22.0, 6.0, 12.0)); break; /* NO */
  case 143: steady(s, 70, voiced_frame(FM(396, 2100, 2700), 22.0, 7.0, 4.0)); break; /* NGE */
  case 144: steady(s, 70, voiced_frame(FM(396, 1500, 1910), 22.0, 6.5, 8.2)); break; /* NGO */
  case 145: steady(s, 70, liquid_frame(fLE)); break;
  case 146: steady(s, 70, liquid_frame(fLO)); break;
  case 147: steady(s, 70, liquid_frame(fWW)); break;
  case 148: steady(s, 70, liquid_frame(fRR)); break;
  case 149: diphthong(s, 200, vowel_frame(fIY), liquid_frame(fER)); break; /* IYRR */
  case 150: diphthong(s, 200, vowel_frame(fEH), liquid_frame(fER)); break; /* EYRR */
  case 151: diphthong(s, 190, vowel_frame(FM(500, 1400, 1750)), liquid_frame(fER)); break; /* AXRR */
  case 152: diphthong(s, 200, vowel_frame(fOHr), liquid_frame(fER)); break; /* AWRR */
  case 153: diphthong(s, 185, vowel_frame(fAWr), liquid_frame(fER)); break; /* OWRR */
  case 154: diphthong(s, 165, vowel_frame(fEY), vowel_frame(fIY)); break; /* EYIY */
  case 155: diphthong(s, 200, vowel_frame(fOH), vowel_frame(fIY)); break; /* OHIY */
  case 156: diphthong(s, 225, vowel_frame(fAWr), vowel_frame(fIY)); break; /* OWIY */
  case 157: diphthong(s, 185, vowel_frame(fOH), vowel_frame(fIH)); break; /* OHIH */
  case 158: diphthong(s, 170, vowel_frame(fYY), vowel_frame(fEH)); break; /* IYEH */
  case 159: diphthong(s, 140, vowel_frame(fAX), liquid_frame(fLO)); break; /* EHLL */
  case 160: diphthong(s, 180, vowel_frame(fYY), vowel_frame(fUW)); break; /* IYUW */
  case 161: diphthong(s, 170, vowel_frame(fAH), vowel_frame(fUW)); break; /* AXUW */
  case 162: diphthong(s, 170, vowel_frame(FM(330, 1300, 2300)), vowel_frame(fUW)); break; /* IHWW */
  case 163: diphthong(s, 200, vowel_frame(fAY), vowel_frame(fUH)); break; /* AYWW */
  case 164: diphthong(s, 131, vowel_frame(fOW), vowel_frame(FM(330, 700, 2250))); break; /* OWWW */
  case 165: /* JH */
    st = STOP(25, 5.52, 12.89, true, lPalatal, 3165, 2474, 2.1, 29.3, 0, 0);
    affricate(s, &st, voiced_noise_frame(FM(158, 1974, 1842), 10.04, 1922, 3058, 5.99, 8.79, 66), 45);
    break;
  case 166: steady(s, 70, voiced_noise_frame(FM(492, 2083, 3156), 3.6, 2253, 1368, 1.4, 5.62, 78)); break; /* VV */
  case 167: steady(s, 70, hush(voiced_noise_frame(FM(240, 1500, 2500), 14, 3700, 3950, 14, 10, 160), FRICATIVE_DB)); break; /* ZZ */
  case 168: steady(s, 70, frZH()); break; /* ZH */
  case 169: steady(s, 70, hush(voiced_noise_frame(FM(250, 1300, 2400), 16, 3000, 3800, 7, 5, 255), FRICATIVE_DB)); break; /* DH */
  case 170: st = STOPN(45, 31.6, 10.8, true, FM(363, 1741, 2562), 2149, 3662, 22, 16.4, 0, 0, 124, 0, 0, 0, 10, 0.84); plosive(s, &st); break; /* BE */
  case 171: st = STOP(45, 25, 8, true, lLabialBack, 600, 1000, 10, 6, 0, 0); plosive(s, &st); break; /* BO */
  case 172: st = STOPN(10, 0.2, 8.8, true, FM(345, 918, 2893), 1926, 2247, 21.5, 7.8, 0, 0, 115, 0, 0, 0, 15.2, 0.24); plosive(s, &st); break; /* EB */
  case 173: st = STOPN(10, 1, 0.2, true, FM(416, 1414, 2909), 835, 3504, 29.1, 22.8, 0, 0, 201, 0, 0, 0, 2.2, 0.25); plosive(s, &st); break; /* OB */
  case 174: st = STOP(45, 25, 8, true, lAlveolar, 2970, 3510, 12, 8, 0, 0); plosive(s, &st); break; /* DE */
  case 175: st = STOP(45, 25, 8, true, lAlveolarBak, 1800, 2220, 12, 8, 0, 0); plosive(s, &st); break; /* DO */
  case 176: st = STOPN(10, 1.87, 0.77, true, FM(281, 2404, 2711), 3411, 3749, 16.6, 20.2, 0, 0, 189, 0, 0, 0, 0.3, 0.44); plosive(s, &st); break; /* ED */
  case 177: st = STOPN(10, 2.21, 0.46, true, FM(278, 1462, 2581), 2076, 3335, 15.2, 24.3, 0, 0, 208, 0, 0, 0, 0.6, 0.93); plosive(s, &st); break; /* OD */
  case 178: st = STOP(55, 30, 10, true, lVelar, 1610, 2030, 12, 8, 0, 0); plosive(s, &st); break; /* GE */
  case 179: st = STOP(55, 30, 10, true, lVelarBack, 650, 900, 12, 8, 0, 0); plosive(s, &st); break; /* GO */
  case 180: st = STOP(55, 35, 10, true, lVelar, 2300, 2900, 10, 6, 0, 0); plosive(s, &st); break; /* EG */
  case 181: st = STOP(55, 35, 10, true, lVelarBack, 910, 1260, 10, 6, 0, 0); plosive(s, &st); break; /* OG */
  case 182: /* CH */
    st = STOP(48.43, 14.21, 10.22, false, lPalatal, 1340, 2417, 7.3, 1.5, 0, 0);
    affricate(s, &st, noise_frame(lPalatal, 2549, 3255, 15.4, 15.9, 47), 21.57);
    break;
  case 183: steady(s, 70, noise_frame(FM(400, 1900, 2600), 1468, 1865, 24.0, 20.9, 186)); break; /* HE */
  case 184: steady(s, 70, noise_frame(FM(600, 1000, 2400), 1952, 1257, 13.5, 24.0, 201)); break; /* HO */
  case 185: steady(s, 70, noise_frame(fWW, 720, 3171, 24.0, 9.3, 207)); break; /* WH */
  case 186: steady(s, 70, noise_frame(lLabial, 3991, 3311, 27.2, 21.5, 87)); break; /* FF */
  case 187: thumped(s, 2.89, 484, 27.4, 37.11, frSE()); break; /* SE */
  case 188: thumped(s, 2.89, 601, 21.6, 37.11, frSO()); break; /* SO */
  case 189: thumped(s, 13.29, 423, 5, 36.71, frSH()); break; /* SH */
  case 190: steady(s, 40, noise_frame(lDental, 2472, 2618, 23.7, 15.1, 141)); break; /* TH */
  case 191: st = STOPN(50, 31.8, 7.11, false, lAlveolar, 2689, 3257, 15.9, 22.1, 3188, 3038, 73, 19, 3.7, 17.4, 0, 0); plosive(s, &st); break; /* TT */
  case 192: st = STOPN(70, 15.11, 27.16, false, lAlveolar, 3582, 3908, 2.9, 2.8, 3126, 2620, 4, 33, 27.7, 5.4, 0, 0); plosive(s, &st); break; /* TU */
  case 193: /* TS */
    st = STOP(83.07, 30.75, 16.24, false, lAlveolar, 2581, 370, 3.5, 13.9, 0, 0);
    affricate(s, &st, noise_frame(lAlveolar, 327, 3430, 5.9, 19, 205), 86.93);
    break;
  case 194: st = STOPN(55, 14.35, 4.28, false, lVelar, 3094, 2840, 4.8, 19.2, 2199, 529, 177, 34, 0.2, 0, 0, 0); plosive(s, &st); break; /* KE */
  case 195: st = STOP(55, 25, 12, false, lVelarBack, 1300, 1700, 18, 12, 1100, 2300); plosive(s, &st); break; /* KO */
  case 196: st = STOPN(55, 17.49, 1.33, false, lVelar, 3979, 2457, 4.1, 26.1, 3137, 2413, 148, 68, 0.9, 0.2, 0, 0); plosive(s, &st); break; /* EK */
  case 197: st = STOPN(45, 14.24, 5.2, false, lVelarBack, 2252, 1769, 22.3, 29.2, 2402, 3387, 185, 101, 0.4, 0.8, 0, 0); plosive(s, &st); break; /* OK */
  case 198: st = STOP(99, 50, 9, false, lLabial, 300, 750, 12, 9, 600, 1200); plosive(s, &st); break; /* PE */
  case 199: st = STOP(99, 50, 9, false, lLabialBack, 300, 720, 12, 9, 600, 1380); plosive(s, &st); break; /* PO */
  default: break;
  }
  if (allophone_length[code - 128] > 0) {
    s->length = allophone_length[code - 128];
  } else {
    /* the manual's length and TRANSITION_MS more */
    double ms = 0;
    for (int i = 0; i < s->nparts; i++)
      ms += s->parts[i].ms;
    s->length = (ms + TRANSITION_MS) / ms;
  }
  const double g = pow(10, allophone_gain_db[code - 128] / 20);
  for (int i = 0; i < s->nparts; i++)
    for (int j = 0; j < 5; j++)
      s->parts[i].target.vol[j] *= g;
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
 * msa_speed_factor()) all twelve sound for 152 to 154ms, 2.4dB quieter
 * against AW than tone() plays them */
static void dtmf(sound *s, double low, double high) {
  const frame t = fade(tone(low, high), 2.4);
  hold(s, 150, 3, t);
  hold(s, 3, 3, quiet(t));
}

/* the manual describes the sound effects only by name and length. the others
 * here are traced from the third take of the VoxRecord recording (see
 * msa_speed_factor()), which plays each on its own and isn't clipped: every
 * 5ms the level and the sound's kind were measured, and the trace reduced to
 * the points below, between which the sound glides in a straight line. a
 * tone is up to three pure sines (the chip's tones have no harmonics to speak
 * of, 45dB and more down): the strongest peaks, within 15dB of the loudest,
 * that aren't its harmonics (R0 to R9, for example, are three tones each).
 * noise (a flat spectrum) is noise around the frequency. a buzz (a pitch
 * between 40 and 500Hz found by autocorrelation, with six or more peaks) is
 * voiced at that pitch with oscillators 1 to 3 on its three formants, read by
 * linear prediction: A1, A2, A3 and M2 buzz at 87Hz, the default speech
 * pitch, and M2 ("WOW") is a vowel glide. each point is where it is reached
 * (in ms from the start of the sound), its kind, the buzz's pitch, its level
 * in dB (-60 is silence), the three frequencies (0 for none) and the second
 * and third's levels against the first (-60 while a tone that comes and goes
 * is absent, so that it fades rather than sweeping from 0Hz). the levels are the recording's with
 * the output filter's loss at each frequency added back (see output_fir), as
 * the filter here takes it off again. many effects are nothing like their
 * names suggest, and many differ in length from the manual: A1 and A2 last
 * about a second, and M0 is a 1.2 second tone around 1.8kHz that dies away */
enum { FX_TONE, FX_NOISE, FX_BUZZ };

typedef struct fx_point {
  short ms, kind, pitch;
  double db;
  short freq[3];
  double rel[3];
} fx_point;

static const fx_point fx_points[] = {
    /* R0 */
    {5, 0, 0, -24.9, {799, 511, 0}, {0, -15.1, -60.0}},
    {20, 0, 0, -24.9, {799, 511, 0}, {0, -15.1, -60.0}},
    {25, 0, 0, -9.1, {799, 511, 0}, {0, -15.1, -60.0}},
    {30, 0, 0, -2.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {35, 0, 0, 1.4, {799, 511, 0}, {0, -15.1, -60.0}},
    {40, 0, 0, 3.3, {799, 511, 0}, {0, -15.1, -60.0}},
    {45, 0, 0, 4.6, {799, 511, 0}, {0, -15.1, -60.0}},
    {70, 0, 0, 5.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {95, 0, 0, 2.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {100, 0, 0, -0.8, {799, 511, 0}, {0, -15.1, -60.0}},
    {105, 0, 0, -6.5, {799, 511, 0}, {0, -15.1, -60.0}},
    {110, 0, 0, -12.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {115, 0, 0, -19.4, {799, 511, 0}, {0, -15.1, -60.0}},
    {120, 0, 0, -20.9, {799, 511, 0}, {0, -15.1, -60.0}},
    {125, 0, 0, -24.9, {799, 511, 0}, {0, -15.1, -60.0}},
    {135, 0, 0, -24.9, {799, 511, 0}, {0, -15.1, -60.0}},
    /* R1 */
    {5, 0, 0, -28.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    {20, 0, 0, -28.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    {25, 0, 0, -14.8, {1100, 731, 0}, {0, -19.3, -60.0}},
    {30, 0, 0, -5.2, {1100, 731, 0}, {0, -19.3, -60.0}},
    {35, 0, 0, -1.6, {1100, 731, 0}, {0, -19.3, -60.0}},
    {80, 0, 0, 1.9, {1100, 731, 0}, {0, -19.3, -60.0}},
    {100, 0, 0, -4.4, {1100, 731, 0}, {0, -19.3, -60.0}},
    {105, 0, 0, -10.8, {1100, 731, 0}, {0, -19.3, -60.0}},
    {110, 0, 0, -20.4, {1100, 731, 0}, {0, -19.3, -60.0}},
    {115, 0, 0, -25.3, {1100, 731, 0}, {0, -19.3, -60.0}},
    {130, 0, 0, -28.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    /* R2 */
    {5, 0, 0, -29.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {20, 0, 0, -29.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {25, 0, 0, -13.7, {1299, 531, 0}, {0, -21.0, -60.0}},
    {30, 0, 0, -5.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {35, 0, 0, -2.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {60, 0, 0, 0.1, {1299, 531, 0}, {0, -21.0, -60.0}},
    {65, 0, 0, -0.2, {1299, 531, 0}, {0, -21.0, -60.0}},
    {95, 0, 0, -2.9, {1299, 531, 0}, {0, -21.0, -60.0}},
    {100, 0, 0, -5.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {105, 0, 0, -11.7, {1299, 531, 0}, {0, -21.0, -60.0}},
    {110, 0, 0, -16.4, {1299, 531, 0}, {0, -21.0, -60.0}},
    {115, 0, 0, -24.6, {1299, 531, 0}, {0, -21.0, -60.0}},
    {120, 0, 0, -26.2, {1299, 531, 0}, {0, -21.0, -60.0}},
    {125, 0, 0, -29.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {135, 0, 0, -29.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    /* R3 */
    {5, 0, 0, -32.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    {10, 0, 0, -32.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    {15, 0, 0, -32.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    {20, 0, 0, -27.4, {1599, 726, 360}, {0, -14.1, -15.2}},
    {25, 0, 0, -17.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {30, 0, 0, -8.1, {1599, 726, 360}, {0, -14.1, -15.2}},
    {35, 0, 0, -4.4, {1599, 726, 360}, {0, -14.1, -15.2}},
    {40, 0, 0, -2.6, {1599, 726, 360}, {0, -14.1, -15.2}},
    {65, 0, 0, -1.5, {1599, 726, 360}, {0, -14.1, -15.2}},
    {70, 0, 0, -1.5, {1599, 726, 360}, {0, -14.1, -15.2}},
    {90, 0, 0, -3.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    {95, 0, 0, -5.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {100, 0, 0, -9.3, {1599, 726, 360}, {0, -14.1, -15.2}},
    {105, 0, 0, -15.1, {1599, 726, 360}, {0, -14.1, -15.2}},
    {110, 0, 0, -21.9, {1599, 726, 360}, {0, -14.1, -15.2}},
    {115, 0, 0, -25.8, {1599, 726, 360}, {0, -14.1, -15.2}},
    {120, 0, 0, -32.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    {130, 0, 0, -32.2, {1599, 726, 360}, {0, -14.1, -15.2}},
    /* R4 */
    {5, 0, 0, -33.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    {20, 0, 0, -33.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    {25, 0, 0, -17.0, {1898, 305, 561}, {0, -18.5, -23.3}},
    {30, 0, 0, -7.5, {1898, 305, 561}, {0, -18.5, -23.3}},
    {55, 0, 0, -1.4, {1898, 305, 561}, {0, -18.5, -23.3}},
    {60, 0, 0, -1.6, {1898, 305, 561}, {0, -18.5, -23.3}},
    {65, 0, 0, -1.5, {1898, 305, 561}, {0, -18.5, -23.3}},
    {90, 0, 0, -3.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    {95, 0, 0, -7.3, {1898, 305, 561}, {0, -18.5, -23.3}},
    {100, 0, 0, -10.4, {1898, 305, 561}, {0, -18.5, -23.3}},
    {105, 0, 0, -15.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    {110, 0, 0, -19.1, {1898, 305, 561}, {0, -18.5, -23.3}},
    {115, 0, 0, -20.6, {1898, 305, 561}, {0, -18.5, -23.3}},
    {120, 0, 0, -28.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    {130, 0, 0, -33.2, {1898, 305, 561}, {0, -18.5, -23.3}},
    /* R5 */
    {5, 0, 0, -33.6, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {10, 0, 0, -34.2, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {15, 0, 0, -33.8, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {20, 0, 0, -33.4, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {25, 0, 0, -13.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {30, 0, 0, -7.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {45, 0, 0, -2.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {90, 0, 0, -3.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {95, 0, 0, -5.2, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {100, 0, 0, -12.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {105, 0, 0, -16.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {110, 0, 0, -22.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {115, 0, 0, -27.2, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {120, 0, 0, -28.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {125, 0, 0, -33.7, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {130, 0, 0, -33.6, {2198, 381, 2667}, {0, -14.3, -18.3}},
    /* R6 */
    {5, 0, 0, -33.2, {2498, 314, 546}, {0, -19.0, -22.2}},
    {10, 0, 0, -32.9, {2498, 314, 546}, {0, -19.0, -22.2}},
    {15, 0, 0, -32.9, {2498, 314, 546}, {0, -19.0, -22.2}},
    {20, 0, 0, -21.5, {2498, 314, 546}, {0, -19.0, -22.2}},
    {25, 0, 0, -16.4, {2498, 314, 546}, {0, -19.0, -22.2}},
    {30, 0, 0, -11.8, {2498, 314, 546}, {0, -19.0, -22.2}},
    {35, 0, 0, -9.7, {2498, 314, 546}, {0, -19.0, -22.2}},
    {40, 0, 0, -1.5, {2498, 314, 546}, {0, -19.0, -22.2}},
    {65, 0, 0, -0.8, {2498, 314, 546}, {0, -19.0, -22.2}},
    {70, 0, 0, -0.7, {2498, 314, 546}, {0, -19.0, -22.2}},
    {80, 0, 0, -0.8, {2498, 314, 546}, {0, -19.0, -22.2}},
    {85, 0, 0, -8.2, {2498, 314, 546}, {0, -19.0, -22.2}},
    {100, 0, 0, -14.1, {2498, 314, 546}, {0, -19.0, -22.2}},
    {105, 0, 0, -18.3, {2498, 314, 546}, {0, -19.0, -22.2}},
    {110, 0, 0, -20.2, {2498, 314, 546}, {0, -19.0, -22.2}},
    {115, 0, 0, -27.1, {2498, 314, 546}, {0, -19.0, -22.2}},
    {120, 0, 0, -31.2, {2498, 314, 546}, {0, -19.0, -22.2}},
    {125, 0, 0, -32.7, {2498, 314, 546}, {0, -19.0, -22.2}},
    /* R7 */
    {5, 0, 0, -24.4, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {10, 0, 0, -24.8, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {15, 0, 0, -24.2, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {20, 0, 0, -9.9, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {25, 0, 0, -5.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {30, 0, 0, -2.7, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {50, 0, 0, -0.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {55, 0, 0, -0.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {60, 0, 0, -0.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {70, 0, 0, -0.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {80, 0, 0, -0.9, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {85, 0, 0, -1.7, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {95, 0, 0, -5.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {100, 0, 0, -8.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {105, 0, 0, -13.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {110, 0, 0, -16.8, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {115, 0, 0, -17.5, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {120, 0, 0, -24.7, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {125, 0, 0, -24.6, {2797, 253, 1055}, {0, -17.8, -24.6}},
    /* R8 */
    {5, 0, 0, -35.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {15, 0, 0, -35.7, {643, 3097, 321}, {0, 10.7, -16.6}},
    {20, 0, 0, -22.7, {643, 3097, 321}, {0, 10.7, -16.6}},
    {25, 0, 0, -18.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {40, 0, 0, -12.6, {643, 3097, 321}, {0, 10.7, -16.6}},
    {45, 0, 0, -12.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {75, 0, 0, -11.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {80, 0, 0, -11.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {85, 0, 0, -12.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {90, 0, 0, -12.6, {643, 3097, 321}, {0, 10.7, -16.6}},
    {95, 0, 0, -13.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {100, 0, 0, -15.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {105, 0, 0, -18.8, {643, 3097, 321}, {0, 10.7, -16.6}},
    {110, 0, 0, -23.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {115, 0, 0, -26.9, {643, 3097, 321}, {0, 10.7, -16.6}},
    {120, 0, 0, -31.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {125, 0, 0, -35.3, {643, 3097, 321}, {0, 10.7, -16.6}},
    {130, 0, 0, -35.6, {643, 3097, 321}, {0, 10.7, -16.6}},
    /* R9 */
    {5, 0, 0, -35.3, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {15, 0, 0, -35.1, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {20, 0, 0, -24.8, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {25, 0, 0, -19.3, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {40, 0, 0, -11.9, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {45, 0, 0, -11.3, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {50, 0, 0, -11.5, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {85, 0, 0, -11.5, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {90, 0, 0, -12.1, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {95, 0, 0, -13.2, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {100, 0, 0, -14.9, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {110, 0, 0, -21.7, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {115, 0, 0, -25.6, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {120, 0, 0, -29.6, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {125, 0, 0, -35.2, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {130, 0, 0, -35.1, {640, 3397, 1592}, {0, 10.1, -14.2}},
    /* A0 */
    {5, 0, 0, -28.2, {239, 1009, 472}, {0, -60.0, -60.0}},
    {20, 0, 0, -28.2, {239, 1009, 472}, {0, -60.0, -60.0}},
    {25, 0, 0, -8.0, {239, 1009, 472}, {0, -60.0, -60.0}},
    {30, 0, 0, -7.0, {175, 1009, 472}, {0, -0.1, -60.0}},
    {35, 0, 0, -6.8, {175, 1627, 472}, {0, -9.7, -60.0}},
    {40, 0, 0, -3.1, {175, 1644, 472}, {0, -3.0, -60.0}},
    {45, 0, 0, -4.0, {233, 1726, 472}, {0, -7.2, -60.0}},
    {55, 0, 0, -1.6, {233, 1779, 472}, {0, -3.5, -60.0}},
    {60, 0, 0, -3.7, {163, 1779, 472}, {0, -10.0, -60.0}},
    {75, 0, 0, -2.9, {163, 1808, 472}, {0, -11.2, -60.0}},
    {80, 0, 0, -1.2, {175, 1837, 472}, {0, 1.9, -60.0}},
    {85, 0, 0, -2.5, {181, 1895, 472}, {0, -10.8, -60.0}},
    {95, 0, 0, -2.1, {181, 1948, 472}, {0, -11.0, -60.0}},
    {110, 0, 0, -1.0, {198, 1977, 472}, {0, -12.0, -60.0}},
    {115, 0, 0, -0.8, {204, 1977, 472}, {0, -60.0, -60.0}},
    {165, 0, 0, -0.8, {245, 1977, 472}, {0, -60.0, -60.0}},
    {170, 0, 0, 0.6, {251, 157, 472}, {0, -60.0, -60.0}},
    {220, 0, 0, 0.8, {297, 157, 472}, {0, -60.0, -60.0}},
    {225, 0, 0, 0.8, {297, 157, 472}, {0, 0.0, -60.0}},
    {230, 0, 0, 1.0, {309, 449, 472}, {0, -11.9, -60.0}},
    {235, 0, 0, 1.0, {315, 157, 472}, {0, -9.2, -60.0}},
    {240, 0, 0, 0.9, {315, 163, 472}, {0, -11.8, -0.0}},
    {245, 0, 0, 0.9, {321, 163, 478}, {0, -10.1, -11.7}},
    {315, 0, 0, 1.2, {373, 192, 560}, {0, -13.5, -14.4}},
    {320, 0, 0, 0.5, {373, 496, 187}, {0, -12.5, -13.6}},
    {330, 0, 0, -1.5, {356, 496, 157}, {0, -4.9, -10.6}},
    {335, 0, 0, -2.5, {356, 157, 157}, {0, -10.0, -60.0}},
    {340, 0, 0, -4.0, {391, 157, 157}, {0, -60.0, -60.0}},
    {345, 0, 0, -5.1, {391, 157, 157}, {0, -60.0, -60.0}},
    {350, 0, 0, -7.7, {391, 192, 157}, {0, -60.0, -60.0}},
    {355, 0, 0, -9.6, {373, 192, 157}, {0, -60.0, -60.0}},
    {360, 0, 0, -10.8, {362, 192, 157}, {0, 0.0, -60.0}},
    {365, 0, 0, -15.7, {362, 566, 157}, {0, -10.2, -60.0}},
    {370, 0, 0, -17.3, {367, 589, 157}, {0, -11.3, -60.0}},
    {375, 0, 0, -25.3, {367, 636, 157}, {0, -12.5, -60.0}},
    {380, 0, 0, -25.9, {362, 991, 157}, {0, -13.3, -60.0}},
    {385, 0, 0, -28.2, {245, 991, 157}, {0, -13.7, -60.0}},
    {395, 0, 0, -28.2, {239, 991, 157}, {0, -13.0, -60.0}},
    /* A1 */
    {5, 2, 127, -20.4, {790, 2001, 2980}, {0, 10.4, 17.1}},
    {10, 2, 125, -20.4, {790, 2002, 2981}, {0, 10.4, 16.9}},
    {15, 2, 142, -20.4, {790, 2133, 3053}, {0, -4.0, 2.1}},
    {35, 2, 88, 4.5, {790, 2133, 3053}, {0, -4.0, 2.1}},
    {45, 2, 87, 6.7, {790, 2133, 3053}, {0, -4.0, 2.1}},
    {50, 2, 87, 6.2, {763, 2130, 3053}, {0, -20.0, -32.4}},
    {55, 2, 87, 6.6, {763, 2074, 3053}, {0, -19.6, -31.9}},
    {60, 2, 87, 7.9, {763, 2074, 3101}, {0, -37.9, -14.2}},
    {65, 2, 87, 7.5, {772, 1484, 3101}, {0, -13.7, -12.6}},
    {85, 2, 88, 8.5, {825, 1520, 3071}, {0, -8.7, -7.9}},
    {105, 2, 88, 9.6, {833, 1563, 3170}, {0, -8.2, -5.1}},
    {110, 2, 87, 7.9, {833, 1867, 3346}, {0, -9.5, -5.2}},
    {155, 2, 87, 6.8, {875, 1933, 3295}, {0, -20.4, -22.0}},
    {160, 2, 87, 8.3, {880, 1933, 2545}, {0, -19.4, -17.7}},
    {185, 2, 88, 8.2, {905, 1980, 2478}, {0, -17.9, -16.4}},
    {190, 2, 87, 6.8, {908, 2009, 2478}, {0, -17.8, -37.7}},
    {235, 2, 87, 6.2, {941, 2008, 2478}, {0, -15.1, -32.8}},
    {245, 2, 87, 6.9, {975, 2331, 2478}, {0, -13.2, -32.7}},
    {250, 2, 87, 7.6, {981, 2342, 3056}, {0, -13.2, -12.2}},
    {265, 2, 87, 7.7, {1004, 2369, 3097}, {0, -11.4, -9.2}},
    {270, 2, 87, 6.3, {1004, 2388, 3113}, {0, -11.5, -9.5}},
    {275, 2, 87, 7.7, {1004, 2451, 3113}, {0, -10.9, -26.0}},
    {325, 2, 87, 5.8, {979, 2564, 3113}, {0, -10.3, -26.3}},
    {340, 2, 87, 6.5, {967, 3007, 3113}, {0, -9.3, -29.4}},
    {345, 2, 87, 7.6, {982, 3042, 2232}, {0, -9.8, -16.6}},
    {435, 2, 87, 7.8, {1173, 3549, 2555}, {0, -11.6, -17.6}},
    {475, 2, 87, 5.4, {1194, 3515, 2589}, {0, -14.0, -18.6}},
    {480, 2, 87, 6.8, {1192, 3066, 2589}, {0, -13.0, -31.1}},
    {530, 2, 87, 6.2, {1142, 2997, 2589}, {0, -13.0, -29.5}},
    {540, 2, 87, 6.7, {1229, 2392, 2589}, {0, -17.1, -34.2}},
    {840, 2, 87, 4.8, {1236, 2392, 2589}, {0, -17.5, -34.6}},
    {845, 2, 87, 6.1, {1566, 2584, 3295}, {0, 5.1, -16.1}},
    {950, 2, 87, 6.6, {1671, 2883, 3355}, {0, 8.0, -10.8}},
    {975, 2, 89, -0.8, {1673, 2895, 3397}, {0, 7.8, -12.6}},
    {980, 2, 77, -9.7, {1661, 2891, 3535}, {0, 5.6, -8.9}},
    {990, 2, 82, -16.1, {1652, 2879, 3535}, {0, 3.8, -23.9}},
    {995, 2, 119, -19.2, {1652, 2879, 3535}, {0, 3.8, -23.9}},
    {1005, 2, 126, -19.3, {1647, 2876, 3535}, {0, 7.6, -22.9}},
    /* A2 */
    {5, 2, 128, -33.8, {165, 1996, 1996}, {0, -0.0, -35.8}},
    {10, 2, 127, -33.8, {165, 1996, 1996}, {0, -0.1, -36.0}},
    {15, 2, 153, -33.8, {165, 1996, 1996}, {0, 0.3, -35.9}},
    {20, 2, 183, -33.8, {165, 1698, 1455}, {0, 36.6, 28.5}},
    {25, 2, 92, -14.1, {165, 1693, 1455}, {0, 40.4, 29.7}},
    {30, 2, 87, -11.1, {184, 1689, 1455}, {0, 15.7, 3.9}},
    {35, 2, 87, -9.8, {186, 1685, 1476}, {0, 15.4, 3.1}},
    {45, 2, 87, -9.2, {186, 1673, 1497}, {0, 15.1, 4.0}},
    {55, 2, 87, -9.0, {170, 1658, 1504}, {0, 15.7, 5.0}},
    {70, 2, 87, -9.5, {162, 1641, 1515}, {0, 15.4, 6.5}},
    {75, 2, 87, -7.9, {162, 1639, 1497}, {0, 42.4, 31.7}},
    {80, 2, 87, -9.4, {162, 1635, 1497}, {0, 42.7, 32.0}},
    {85, 2, 87, -7.9, {162, 1631, 1497}, {0, 42.6, 0.5}},
    {490, 2, 87, -8.2, {162, 1584, 1497}, {0, 43.2, 0.5}},
    {495, 2, 87, -7.3, {162, 1374, 1239}, {0, 39.9, 36.9}},
    {510, 2, 87, -6.7, {162, 1334, 1254}, {0, 39.3, 40.2}},
    {530, 2, 87, -6.3, {162, 1334, 1254}, {0, 39.3, 40.2}},
    {535, 2, 87, -6.9, {162, 1212, 1531}, {0, 42.7, 29.1}},
    {580, 2, 87, -5.4, {162, 1130, 1491}, {0, 45.9, 27.4}},
    {585, 2, 87, -6.4, {162, 1124, 1491}, {0, 46.2, 0.5}},
    {620, 2, 87, -6.2, {162, 1048, 1491}, {0, 44.3, 0.5}},
    {625, 2, 87, -5.5, {162, 1030, 1304}, {0, 43.0, 29.8}},
    {690, 2, 87, -6.2, {162, 938, 1172}, {0, 38.2, 31.4}},
    {695, 2, 87, -6.6, {162, 919, 1040}, {0, 37.9, 37.7}},
    {750, 2, 87, -5.7, {162, 525, 963}, {0, 33.8, 39.1}},
    {770, 2, 87, -6.0, {162, 525, 963}, {0, 33.8, 39.1}},
    {775, 2, 87, -6.5, {162, 467, 944}, {0, 34.3, 39.5}},
    {815, 2, 87, -6.1, {162, 389, 897}, {0, 38.1, 45.2}},
    {850, 2, 87, -5.1, {162, 317, 865}, {0, 33.5, 42.8}},
    {860, 2, 87, -5.6, {162, 250, 857}, {0, 32.0, 41.0}},
    {865, 2, 87, -4.7, {162, 250, 853}, {0, -0.0, 39.4}},
    {895, 2, 87, -5.1, {162, 250, 845}, {0, -0.0, 34.4}},
    {955, 2, 87, -7.1, {162, 250, 757}, {0, -0.0, 42.8}},
    {980, 2, 82, -15.8, {162, 250, 731}, {0, -0.0, 50.7}},
    {985, 2, 183, -25.5, {162, 250, 731}, {0, -0.0, 50.7}},
    {990, 2, 181, -33.8, {162, 250, 731}, {0, -0.0, 50.7}},
    {995, 2, 71, -33.8, {160, 250, 731}, {0, -39.9, -12.7}},
    {1000, 2, 127, -33.8, {149, 250, 731}, {0, -39.7, -10.4}},
    /* A3 */
    {5, 2, 128, -32.6, {576, 977, 1996}, {0, 27.2, 36.5}},
    {10, 2, 128, -32.6, {576, 980, 2002}, {0, 27.3, 37.5}},
    {15, 2, 468, -32.5, {576, 990, 2010}, {0, 26.3, 38.0}},
    {20, 2, 468, -32.6, {576, 990, 2010}, {0, 26.3, 38.0}},
    {25, 2, 473, -6.5, {577, 990, 2386}, {0, -35.0, -12.1}},
    {30, 2, 478, -5.5, {577, 978, 2386}, {0, -1.6, -13.1}},
    {35, 2, 87, -3.9, {577, 978, 2386}, {0, -1.0, -13.9}},
    {80, 2, 87, -4.6, {578, 1065, 2476}, {0, 1.0, -14.0}},
    {120, 2, 87, -3.5, {626, 1164, 2491}, {0, 0.2, -15.2}},
    {125, 2, 87, -5.5, {632, 1177, 1852}, {0, 0.1, 3.6}},
    {135, 2, 87, -4.9, {645, 1199, 1852}, {0, -0.1, 2.7}},
    {365, 2, 87, -9.1, {1121, 1915, 2760}, {0, 3.0, 6.1}},
    {520, 2, 87, -8.7, {1452, 2357, 3417}, {0, 4.1, 6.0}},
    {525, 2, 483, -10.9, {1462, 2370, 3440}, {0, 4.3, 6.1}},
    {545, 2, 487, -14.2, {1465, 2382, 3447}, {0, 5.2, 6.6}},
    {550, 2, 487, -26.0, {1459, 2382, 3431}, {0, 4.0, 6.5}},
    {555, 2, 487, -28.6, {1304, 2379, 3423}, {0, -21.7, 6.5}},
    {560, 2, 125, -30.0, {1283, 2184, 3423}, {0, 1.5, -11.8}},
    {565, 2, 142, -32.6, {1067, 1987, 2980}, {0, 7.8, 15.7}},
    {570, 2, 135, -32.6, {1011, 1986, 2975}, {0, 9.0, 16.6}},
    /* A4 */
    {5, 0, 0, -15.6, {239, 467, 420}, {0, -60.0, -60.0}},
    {10, 0, 0, -15.6, {239, 467, 420}, {0, -13.9, -60.0}},
    {15, 0, 0, -15.6, {239, 991, 420}, {0, -0.1, -60.0}},
    {20, 0, 0, -15.6, {239, 467, 420}, {0, -15.0, -60.0}},
    {25, 0, 0, -15.6, {239, 467, 420}, {0, -13.9, -60.0}},
    {30, 0, 0, 5.2, {239, 624, 420}, {0, -2.1, -60.0}},
    {35, 0, 0, 9.6, {222, 624, 420}, {0, -8.7, -60.0}},
    {40, 0, 0, 11.6, {222, 461, 420}, {0, -12.1, -60.0}},
    {45, 0, 0, 13.1, {222, 461, 420}, {0, -60.0, -60.0}},
    {75, 0, 0, 13.9, {338, 461, 420}, {0, -60.0, -60.0}},
    {115, 0, 0, 14.3, {496, 461, 420}, {0, -60.0, -60.0}},
    {120, 0, 0, 14.2, {536, 461, 420}, {0, -60.0, -60.0}},
    {125, 0, 0, 13.9, {601, 210, 420}, {0, -60.0, -60.0}},
    {145, 0, 0, 12.6, {846, 210, 420}, {0, -60.0, -60.0}},
    {170, 0, 0, 9.7, {1166, 210, 420}, {0, -60.0, -60.0}},
    {175, 0, 0, 9.8, {1411, 210, 420}, {0, -60.0, -60.0}},
    {195, 0, 0, 9.6, {2169, 210, 420}, {0, -60.0, -60.0}},
    {200, 0, 0, 10.3, {2362, 210, 420}, {0, -5.6, -60.0}},
    {205, 0, 0, 11.0, {2461, 210, 420}, {0, -9.2, -60.0}},
    {210, 0, 0, 9.8, {2840, 472, 420}, {0, -14.1, -60.0}},
    {215, 0, 0, 11.4, {3254, 472, 420}, {0, -24.3, -60.0}},
    {220, 0, 0, 15.4, {3860, 472, 420}, {0, -19.3, -60.0}},
    {225, 0, 0, 11.8, {3895, 472, 420}, {0, -60.0, -60.0}},
    {230, 0, 0, 10.5, {3901, 472, 420}, {0, -60.0, -60.0}},
    {255, 0, 0, 5.0, {3901, 496, 420}, {0, -60.0, -60.0}},
    {260, 0, 0, 3.5, {3901, 496, 420}, {0, -31.3, -60.0}},
    {265, 0, 0, 2.0, {3901, 606, 420}, {0, -30.3, -31.8}},
    {270, 0, 0, 2.0, {3901, 624, 706}, {0, -28.5, -30.1}},
    {275, 0, 0, -15.6, {490, 3901, 706}, {0, 16.0, -1.9}},
    {280, 0, 0, -15.6, {379, 3901, 647}, {0, 11.0, -8.3}},
    {285, 0, 0, -15.6, {239, 3901, 420}, {0, 7.8, -11.7}},
    {290, 0, 0, -15.6, {239, 997, 420}, {0, -14.8, -60.0}},
    {295, 0, 0, -15.6, {239, 991, 420}, {0, -14.5, -60.0}},
    /* A5 */
    {5, 0, 0, -25.6, {239, 484, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -25.6, {239, 484, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -8.3, {636, 484, 0}, {0, -60.0, 0.0}},
    {35, 0, 0, 0.5, {630, 484, 0}, {0, -60.0, 0.0}},
    {130, 0, 0, 3.1, {507, 484, 0}, {0, -60.0, 0.0}},
    {135, 0, 0, 3.1, {169, 484, 0}, {0, -60.0, 0.0}},
    {140, 0, 0, 3.0, {169, 484, 0}, {0, -0.0, 0.0}},
    {190, 0, 0, 3.0, {222, 432, 0}, {0, -0.0, 0.0}},
    {215, 0, 0, 3.5, {257, 408, 0}, {0, -0.5, 0.0}},
    {220, 0, 0, 3.1, {262, 408, 0}, {0, -60.0, 0.0}},
    {245, 0, 0, 3.4, {292, 408, 0}, {0, -60.0, 0.0}},
    {250, 0, 0, 3.9, {327, 408, 0}, {0, -60.0, 0.0}},
    {260, 0, 0, 4.4, {332, 408, 0}, {0, -60.0, 0.0}},
    {280, 0, 0, 1.8, {327, 408, 0}, {0, -60.0, 0.0}},
    {285, 0, 0, 3.0, {303, 408, 0}, {0, -60.0, 0.0}},
    {325, 0, 0, 3.3, {262, 408, 0}, {0, -60.0, 0.0}},
    {330, 0, 0, 3.4, {251, 408, 0}, {0, -0.5, 0.0}},
    {400, 0, 0, 3.0, {169, 490, 0}, {0, -0.7, 0.0}},
    {405, 0, 0, 2.9, {169, 490, 0}, {0, -15.0, 0.0}},
    {410, 0, 0, 2.9, {169, 490, 0}, {0, -1.0, 0.0}},
    {415, 0, 0, 3.0, {507, 490, 0}, {0, -60.0, 0.0}},
    {510, 0, 0, 0.7, {624, 490, 0}, {0, -60.0, 0.0}},
    {545, 0, 0, -4.0, {624, 490, 0}, {0, -60.0, 0.0}},
    {570, 0, 0, -16.9, {618, 490, 0}, {0, -60.0, 0.0}},
    {575, 0, 0, -22.0, {577, 490, 0}, {0, -60.0, 0.0}},
    {580, 0, 0, -25.6, {245, 490, 0}, {0, -60.0, 0.0}},
    {585, 0, 0, -25.6, {239, 490, 0}, {0, -60.0, 0.0}},
    /* A6 */
    {5, 0, 0, -26.1, {239, 991, 0}, {0, -60.0, 0.0}},
    {10, 0, 0, -26.1, {239, 991, 0}, {0, -13.1, 0.0}},
    {20, 0, 0, -26.1, {239, 991, 0}, {0, -14.7, 0.0}},
    {25, 0, 0, -20.5, {239, 700, 0}, {0, -11.5, 0.0}},
    {30, 0, 0, -2.5, {257, 501, 0}, {0, -12.8, 0.0}},
    {35, 0, 0, 1.1, {262, 501, 0}, {0, -1.8, 0.0}},
    {40, 0, 0, 2.7, {303, 665, 0}, {0, -5.4, 0.0}},
    {45, 0, 0, 3.8, {303, 665, 0}, {0, -5.9, 0.0}},
    {65, 0, 0, 2.9, {513, 1230, 0}, {0, -9.8, 0.0}},
    {85, 0, 0, 1.3, {729, 1796, 0}, {0, -11.8, 0.0}},
    {95, 0, 0, 0.8, {799, 1994, 0}, {0, -9.2, 0.0}},
    {110, 0, 0, 0.5, {799, 2000, 0}, {0, -11.6, 0.0}},
    {115, 0, 0, 0.5, {799, 2000, 0}, {0, -60.0, 0.0}},
    {180, 0, 0, -3.9, {799, 2000, 0}, {0, -60.0, 0.0}},
    {200, 0, 0, -11.2, {799, 2000, 0}, {0, -60.0, 0.0}},
    {215, 0, 0, -23.6, {799, 2000, 0}, {0, -60.0, 0.0}},
    {220, 0, 0, -26.1, {245, 2000, 0}, {0, -60.0, 0.0}},
    {230, 0, 0, -26.1, {227, 2000, 0}, {0, -60.0, 0.0}},
    /* A7 */
    {5, 0, 0, -28.0, {239, 373, 770}, {0, -60.0, -60.0}},
    {20, 0, 0, -28.0, {239, 373, 770}, {0, -60.0, -60.0}},
    {25, 0, 0, -9.7, {1131, 373, 770}, {0, -60.0, -60.0}},
    {40, 0, 0, -3.8, {1160, 373, 770}, {0, -60.0, -60.0}},
    {55, 0, 0, -4.2, {1195, 373, 770}, {0, -60.0, -60.0}},
    {60, 0, 0, -4.4, {1207, 373, 770}, {0, -13.3, -14.9}},
    {115, 0, 0, -3.7, {1306, 408, 886}, {0, -0.4, -3.1}},
    {120, 0, 0, -3.7, {420, 1335, 898}, {0, -0.3, -2.6}},
    {130, 0, 0, -3.3, {420, 1335, 921}, {0, -2.1, -2.6}},
    {135, 0, 0, -3.1, {426, 951, 1370}, {0, -2.8, -2.6}},
    {215, 0, 0, 0.4, {484, 1108, 1528}, {0, -3.5, -13.8}},
    {220, 0, 0, 0.5, {484, 1120, 1528}, {0, -3.7, -60.0}},
    {460, 0, 0, 0.3, {805, 1644, 1528}, {0, -4.2, -60.0}},
    {465, 0, 0, 0.2, {811, 1644, 1528}, {0, 1.1, -60.0}},
    {470, 0, 0, 0.2, {822, 1644, 1528}, {0, -4.4, -60.0}},
    {475, 0, 0, 0.1, {828, 1644, 1528}, {0, -60.0, -60.0}},
    {550, 0, 0, -6.5, {927, 1825, 1528}, {0, -60.0, -60.0}},
    {555, 0, 0, -7.5, {933, 1825, 1528}, {0, -2.3, -60.0}},
    {580, 0, 0, -14.7, {962, 1849, 1528}, {0, 0.5, -60.0}},
    {585, 0, 0, -14.7, {1860, 968, 1528}, {0, -2.5, -60.0}},
    {595, 0, 0, -20.2, {1860, 927, 1528}, {0, -13.9, -60.0}},
    {600, 0, 0, -23.5, {1860, 927, 1528}, {0, -60.0, -60.0}},
    {605, 0, 0, -28.0, {1249, 927, 1528}, {0, -60.0, -60.0}},
    {610, 0, 0, -28.1, {1090, 927, 1528}, {0, -60.0, -60.0}},
    {615, 0, 0, -28.0, {245, 927, 1528}, {0, -60.0, -60.0}},
    {625, 0, 0, -28.0, {239, 927, 1528}, {0, -60.0, -60.0}},
    /* A8 */
    {5, 0, 0, -27.0, {239, 997, 0}, {0, -13.8, 0.0}},
    {20, 0, 0, -27.0, {245, 997, 0}, {0, -13.6, 0.0}},
    {25, 0, 0, -27.0, {245, 455, 0}, {0, -5.4, 0.0}},
    {30, 0, 0, -14.3, {245, 426, 0}, {0, -10.5, 0.0}},
    {35, 0, 0, -11.9, {227, 426, 0}, {0, -60.0, 0.0}},
    {50, 0, 0, -3.0, {233, 426, 0}, {0, -60.0, 0.0}},
    {65, 0, 0, 0.6, {344, 426, 0}, {0, -60.0, 0.0}},
    {80, 0, 0, 2.7, {449, 426, 0}, {0, -60.0, 0.0}},
    {85, 0, 0, 3.0, {484, 665, 0}, {0, -60.0, 0.0}},
    {95, 0, 0, 2.1, {501, 665, 0}, {0, -60.0, 0.0}},
    {125, 0, 0, -5.6, {490, 665, 0}, {0, -60.0, 0.0}},
    {130, 0, 0, -7.0, {449, 665, 0}, {0, -4.8, 0.0}},
    {140, 0, 0, -10.9, {303, 665, 0}, {0, -2.9, 0.0}},
    {145, 0, 0, -13.6, {303, 542, 0}, {0, -5.1, 0.0}},
    {150, 0, 0, -17.3, {303, 542, 0}, {0, -1.2, 0.0}},
    {155, 0, 0, -23.0, {239, 542, 0}, {0, -6.7, 0.0}},
    {160, 0, 0, -27.0, {239, 443, 0}, {0, -2.9, 0.0}},
    {165, 0, 0, -27.0, {338, 548, 0}, {0, -13.7, 0.0}},
    {170, 0, 0, -25.3, {338, 548, 0}, {0, -7.5, 0.0}},
    {175, 0, 0, -18.5, {338, 391, 0}, {0, -11.3, 0.0}},
    {180, 0, 0, -13.3, {338, 391, 0}, {0, -4.8, 0.0}},
    {185, 0, 0, -9.5, {338, 548, 0}, {0, -11.8, 0.0}},
    {190, 0, 0, -6.8, {251, 700, 0}, {0, -6.2, 0.0}},
    {195, 0, 0, -4.7, {251, 863, 0}, {0, -7.0, 0.0}},
    {200, 0, 0, -3.1, {251, 863, 0}, {0, -8.2, 0.0}},
    {205, 0, 0, -1.8, {286, 863, 0}, {0, -8.8, 0.0}},
    {210, 0, 0, -0.6, {327, 863, 0}, {0, -60.0, 0.0}},
    {225, 0, 0, 1.5, {443, 863, 0}, {0, -60.0, 0.0}},
    {235, 0, 0, 1.5, {496, 863, 0}, {0, -60.0, 0.0}},
    {280, 0, 0, -10.3, {496, 863, 0}, {0, -60.0, 0.0}},
    {295, 0, 0, -23.5, {490, 863, 0}, {0, -60.0, 0.0}},
    {300, 0, 0, -27.0, {251, 863, 0}, {0, -60.0, 0.0}},
    {310, 0, 0, -27.0, {239, 863, 0}, {0, -60.0, 0.0}},
    /* A9 */
    {5, 0, 0, -28.2, {233, 2443, 828}, {0, -60.0, -60.0}},
    {15, 0, 0, -28.2, {239, 2443, 828}, {0, -60.0, -60.0}},
    {20, 0, 0, -17.3, {245, 2443, 828}, {0, -60.0, -60.0}},
    {25, 0, 0, -10.2, {1295, 2443, 828}, {0, -60.0, -60.0}},
    {40, 0, 0, -5.1, {1295, 2443, 828}, {0, -60.0, -60.0}},
    {85, 0, 0, -4.8, {1248, 2443, 828}, {0, -60.0, -60.0}},
    {90, 0, 0, -4.7, {1219, 2443, 828}, {0, -1.5, -60.0}},
    {140, 0, 0, -2.4, {904, 2169, 828}, {0, -3.6, -60.0}},
    {180, 0, 0, -0.5, {653, 1901, 828}, {0, -5.3, -60.0}},
    {185, 0, 0, -0.3, {618, 1831, 828}, {0, 2.0, -60.0}},
    {195, 0, 0, 0.1, {560, 1761, 828}, {0, -5.9, -60.0}},
    {220, 0, 0, 1.2, {402, 1627, 828}, {0, -6.2, -60.0}},
    {245, 0, 0, 1.7, {245, 1458, 828}, {0, -6.1, -60.0}},
    {255, 0, 0, 1.6, {245, 1388, 828}, {0, -5.8, -60.0}},
    {260, 0, 0, 1.6, {1306, 216, 828}, {0, -0.1, -60.0}},
    {265, 0, 0, 1.1, {1306, 216, 828}, {0, -0.1, -60.0}},
    {270, 0, 0, 0.8, {1306, 216, 828}, {0, -11.3, -60.0}},
    {275, 0, 0, 0.3, {1265, 216, 828}, {0, -3.1, -7.2}},
    {280, 0, 0, -0.3, {216, 478, 828}, {0, -11.0, -13.2}},
    {285, 0, 0, -1.0, {216, 513, 921}, {0, -6.6, -13.1}},
    {290, 0, 0, -1.9, {216, 531, 921}, {0, -9.3, -0.1}},
    {295, 0, 0, -3.0, {216, 513, 764}, {0, -7.1, -11.8}},
    {300, 0, 0, -4.1, {216, 624, 764}, {0, -4.6, -6.4}},
    {310, 0, 0, -7.0, {222, 630, 764}, {0, -7.7, -8.9}},
    {315, 0, 0, -8.8, {222, 601, 770}, {0, -1.4, -1.5}},
    {320, 0, 0, -11.1, {356, 630, 863}, {0, -2.8, -5.8}},
    {325, 0, 0, -14.2, {356, 601, 770}, {0, -1.7, -10.7}},
    {330, 0, 0, -18.1, {257, 490, 863}, {0, -1.0, -9.0}},
    {335, 0, 0, -24.4, {257, 490, 863}, {0, -3.3, -5.5}},
    {340, 0, 0, -28.2, {239, 490, 863}, {0, -10.0, -11.6}},
    {345, 0, 0, -28.2, {239, 478, 682}, {0, -13.2, -0.1}},
    {350, 0, 0, -28.2, {233, 478, 682}, {0, -14.8, -60.0}},
    /* B0 */
    {5, 0, 0, -33.7, {239, 227, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -34.0, {239, 227, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -20.1, {245, 227, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -7.3, {2583, 227, 0}, {0, -60.0, 0.0}},
    {40, 0, 0, -2.7, {2595, 227, 0}, {0, -60.0, 0.0}},
    {135, 0, 0, -2.9, {2595, 227, 0}, {0, -60.0, 0.0}},
    {185, 0, 0, -15.6, {2595, 227, 0}, {0, -60.0, 0.0}},
    {190, 0, 0, -18.3, {2595, 227, 0}, {0, -13.9, 0.0}},
    {195, 0, 0, -20.8, {2595, 455, 0}, {0, -11.8, 0.0}},
    {200, 0, 0, -29.9, {245, 461, 0}, {0, -11.9, 0.0}},
    {210, 0, 0, -33.4, {233, 455, 0}, {0, -12.9, 0.0}},
    /* B1 */
    {5, 0, 0, -28.8, {239, 0, 0}, {0, 0.0, 0.0}},
    {20, 0, 0, -28.8, {239, 0, 0}, {0, 0.0, 0.0}},
    {25, 0, 0, -6.0, {1376, 0, 0}, {0, 0.0, 0.0}},
    {40, 0, 0, 0.7, {1516, 0, 0}, {0, 0.0, 0.0}},
    {95, 0, 0, -1.1, {2473, 0, 0}, {0, 0.0, 0.0}},
    {170, 0, 0, -2.5, {3767, 0, 0}, {0, 0.0, 0.0}},
    {185, 0, 0, -2.4, {3709, 0, 0}, {0, 0.0, 0.0}},
    {230, 0, 0, -1.4, {2752, 0, 0}, {0, 0.0, 0.0}},
    {260, 0, 0, -0.8, {2111, 0, 0}, {0, 0.0, 0.0}},
    {295, 0, 0, 1.5, {1411, 0, 0}, {0, 0.0, 0.0}},
    {375, 0, 0, -7.6, {1400, 0, 0}, {0, 0.0, 0.0}},
    {410, 0, 0, -23.3, {1400, 0, 0}, {0, 0.0, 0.0}},
    {415, 0, 0, -28.8, {257, 0, 0}, {0, 0.0, 0.0}},
    {430, 0, 0, -28.8, {239, 0, 0}, {0, 0.0, 0.0}},
    /* B2 */
    {5, 0, 0, -26.8, {239, 198, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -26.8, {239, 198, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -5.8, {1376, 198, 0}, {0, -60.0, 0.0}},
    {40, 0, 0, 3.4, {1405, 198, 0}, {0, -60.0, 0.0}},
    {140, 0, 0, 3.3, {1405, 198, 0}, {0, -60.0, 0.0}},
    {145, 0, 0, 2.8, {1429, 198, 0}, {0, -10.4, 0.0}},
    {150, 0, 0, 1.9, {1429, 198, 0}, {0, -3.8, 0.0}},
    {160, 0, 0, 1.7, {1977, 198, 0}, {0, -2.8, 0.0}},
    {165, 0, 0, 4.7, {2484, 198, 0}, {0, -11.6, 0.0}},
    {175, 0, 0, 4.3, {2484, 198, 0}, {0, -13.1, 0.0}},
    {180, 0, 0, 0.3, {1709, 198, 0}, {0, -1.3, 0.0}},
    {190, 0, 0, 2.3, {1697, 198, 0}, {0, -10.5, 0.0}},
    {230, 0, 0, 2.9, {1697, 204, 0}, {0, -11.6, 0.0}},
    {255, 0, 0, 2.9, {1697, 397, 0}, {0, -12.3, 0.0}},
    {280, 0, 0, 2.9, {1697, 595, 0}, {0, -12.8, 0.0}},
    {355, 0, 0, -2.4, {1703, 601, 0}, {0, -13.2, 0.0}},
    {400, 0, 0, -13.2, {1703, 601, 0}, {0, -16.1, 0.0}},
    {405, 0, 0, -15.7, {1703, 472, 0}, {0, -1.3, 0.0}},
    {410, 0, 0, -18.7, {1703, 472, 0}, {0, -15.8, 0.0}},
    {415, 0, 0, -22.9, {1703, 472, 0}, {0, -14.9, 0.0}},
    {420, 0, 0, -25.5, {1703, 478, 0}, {0, -3.3, 0.0}},
    {425, 0, 0, -26.8, {356, 478, 0}, {0, -10.6, 0.0}},
    {430, 0, 0, -26.8, {239, 478, 0}, {0, -4.3, 0.0}},
    {435, 0, 0, -26.8, {233, 478, 0}, {0, -60.0, 0.0}},
    /* B3 */
    {5, 0, 0, -30.8, {239, 490, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -30.8, {239, 490, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -20.8, {239, 490, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -13.8, {595, 490, 0}, {0, 0.0, 0.0}},
    {30, 0, 0, -11.3, {595, 3096, 0}, {0, 9.4, 0.0}},
    {130, 0, 0, -7.1, {595, 3096, 0}, {0, 8.2, 0.0}},
    {135, 0, 0, -4.4, {595, 1423, 0}, {0, -5.2, 0.0}},
    {140, 0, 0, -2.4, {1394, 601, 0}, {0, -5.9, 0.0}},
    {255, 0, 0, -0.9, {1394, 601, 0}, {0, -9.9, 0.0}},
    {310, 0, 0, -12.2, {1400, 601, 0}, {0, -11.6, 0.0}},
    {325, 0, 0, -21.2, {1394, 589, 0}, {0, -15.1, 0.0}},
    {330, 0, 0, -26.5, {1394, 589, 0}, {0, -6.9, 0.0}},
    {335, 0, 0, -30.3, {478, 991, 0}, {0, -4.5, 0.0}},
    {340, 0, 0, -30.8, {245, 991, 0}, {0, -12.9, 0.0}},
    {345, 0, 0, -30.8, {239, 991, 0}, {0, -13.8, 0.0}},
    /* B4 */
    {5, 0, 0, -30.9, {239, 1551, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -30.9, {239, 1551, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -9.4, {1819, 1551, 0}, {0, -60.0, 0.0}},
    {35, 0, 0, -4.1, {1831, 1551, 0}, {0, -60.0, 0.0}},
    {55, 0, 0, -3.2, {1825, 1551, 0}, {0, -60.0, 0.0}},
    {60, 0, 0, -2.5, {1825, 1551, 0}, {0, -6.6, 0.0}},
    {70, 0, 0, -2.2, {1825, 1551, 0}, {0, -5.6, 0.0}},
    {120, 0, 0, -1.0, {1825, 962, 0}, {0, -2.8, 0.0}},
    {130, 0, 0, -0.5, {1825, 962, 0}, {0, -2.1, 0.0}},
    {135, 0, 0, -2.3, {781, 1825, 0}, {0, 1.7, 0.0}},
    {155, 0, 0, -1.4, {548, 1825, 0}, {0, 0.4, 0.0}},
    {170, 0, 0, -1.0, {373, 1825, 0}, {0, -0.4, 0.0}},
    {180, 0, 0, -0.9, {373, 1825, 0}, {0, -0.8, 0.0}},
    {185, 0, 0, -1.0, {402, 1825, 0}, {0, -0.4, 0.0}},
    {200, 0, 0, -1.5, {577, 1825, 0}, {0, 0.5, 0.0}},
    {220, 0, 0, -2.3, {811, 1825, 0}, {0, 1.8, 0.0}},
    {225, 0, 0, -0.6, {1825, 986, 0}, {0, -2.2, 0.0}},
    {235, 0, 0, -1.0, {1825, 986, 0}, {0, -2.9, 0.0}},
    {280, 0, 0, -2.4, {1825, 1510, 0}, {0, -5.4, 0.0}},
    {290, 0, 0, -2.4, {1825, 1510, 0}, {0, -6.2, 0.0}},
    {295, 0, 0, -3.0, {1825, 1510, 0}, {0, -60.0, 0.0}},
    {305, 0, 0, -4.9, {1819, 1510, 0}, {0, -60.0, 0.0}},
    {310, 0, 0, -5.4, {1825, 1994, 0}, {0, -60.0, 0.0}},
    {320, 0, 0, -3.3, {1825, 1994, 0}, {0, -60.0, 0.0}},
    {325, 0, 0, -3.2, {1825, 1994, 0}, {0, -4.9, 0.0}},
    {360, 0, 0, -8.6, {1825, 1994, 0}, {0, -5.2, 0.0}},
    {390, 0, 0, -19.7, {1825, 1994, 0}, {0, -6.8, 0.0}},
    {395, 0, 0, -23.7, {1825, 991, 0}, {0, -2.0, 0.0}},
    {400, 0, 0, -28.4, {1825, 991, 0}, {0, -2.9, 0.0}},
    {405, 0, 0, -30.9, {239, 991, 0}, {0, -14.2, 0.0}},
    {415, 0, 0, -30.9, {239, 991, 0}, {0, -13.8, 0.0}},
    /* B5 */
    {5, 0, 0, -24.0, {233, 490, 204}, {0, -60.0, -60.0}},
    {10, 0, 0, -24.0, {239, 490, 204}, {0, -60.0, -60.0}},
    {15, 0, 0, -24.0, {239, 490, 204}, {0, -0.0, -60.0}},
    {20, 0, 0, -24.0, {239, 490, 204}, {0, -0.0, -60.0}},
    {25, 0, 0, -3.3, {951, 501, 204}, {0, -8.9, -60.0}},
    {30, 0, 0, -0.2, {951, 781, 204}, {0, -12.9, -60.0}},
    {35, 0, 0, 1.7, {951, 3493, 204}, {0, -0.2, -60.0}},
    {50, 0, 0, 3.7, {811, 3493, 204}, {0, -0.4, -60.0}},
    {55, 0, 0, 4.0, {764, 3493, 204}, {0, -60.0, -60.0}},
    {85, 0, 0, 5.4, {472, 3493, 204}, {0, -60.0, -60.0}},
    {95, 0, 0, 5.8, {379, 3493, 204}, {0, -60.0, -60.0}},
    {105, 0, 0, 6.0, {280, 3493, 204}, {0, -60.0, -60.0}},
    {110, 0, 0, 5.9, {280, 175, 204}, {0, -60.0, -60.0}},
    {115, 0, 0, 5.5, {280, 175, 204}, {0, -60.0, -60.0}},
    {120, 0, 0, 19.8, {3493, 175, 204}, {0, -60.0, -60.0}},
    {130, 0, 0, 17.9, {3499, 175, 204}, {0, -60.0, -60.0}},
    {155, 0, 0, 13.3, {3499, 175, 204}, {0, -60.0, -60.0}},
    {160, 0, 0, 12.6, {3499, 175, 204}, {0, -14.5, -60.0}},
    {165, 0, 0, 11.0, {3499, 175, 204}, {0, -29.1, -60.0}},
    {170, 0, 0, 8.7, {3499, 181, 204}, {0, -26.1, -29.0}},
    {175, 0, 0, 7.3, {3499, 233, 472}, {0, -22.0, -24.8}},
    {180, 0, 0, 3.2, {3499, 461, 472}, {0, -20.1, -20.5}},
    {185, 0, 0, -13.9, {338, 461, 472}, {0, -4.7, -5.0}},
    {190, 0, 0, -17.4, {239, 531, 204}, {0, -1.6, -4.1}},
    {195, 0, 0, -24.0, {239, 986, 204}, {0, -0.1, -60.0}},
    {200, 0, 0, -24.0, {239, 531, 204}, {0, -13.0, -60.0}},
    {205, 0, 0, -24.0, {233, 531, 204}, {0, -60.0, -60.0}},
    /* B6 */
    {5, 0, 0, -33.8, {239, 367, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -33.4, {239, 367, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -22.5, {239, 367, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -3.7, {3481, 367, 0}, {0, -60.0, 0.0}},
    {30, 0, 0, -1.5, {3493, 367, 0}, {0, -60.0, 0.0}},
    {40, 0, 0, 0.9, {3493, 367, 0}, {0, -60.0, 0.0}},
    {45, 0, 0, -8.1, {1802, 367, 0}, {0, -60.0, 0.0}},
    {70, 0, 0, -5.5, {1796, 367, 0}, {0, -60.0, 0.0}},
    {75, 0, 0, 5.3, {3493, 367, 0}, {0, -60.0, 0.0}},
    {85, 0, 0, 5.2, {3493, 367, 0}, {0, -60.0, 0.0}},
    {90, 0, 0, -5.6, {1796, 367, 0}, {0, -60.0, 0.0}},
    {140, 0, 0, -5.0, {1796, 367, 0}, {0, -60.0, 0.0}},
    {180, 0, 0, -18.6, {1796, 367, 0}, {0, -60.0, 0.0}},
    {185, 0, 0, -22.4, {1796, 367, 0}, {0, -13.0, 0.0}},
    {190, 0, 0, -28.3, {251, 467, 0}, {0, -2.1, 0.0}},
    {195, 0, 0, -31.3, {239, 991, 0}, {0, -13.9, 0.0}},
    {200, 0, 0, -33.5, {239, 991, 0}, {0, -14.1, 0.0}},
    {205, 0, 0, -33.5, {239, 467, 0}, {0, -14.9, 0.0}},
    /* B7 */
    {5, 0, 0, -33.5, {239, 268, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -33.3, {239, 268, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -23.5, {239, 268, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -4.6, {2892, 268, 0}, {0, -60.0, 0.0}},
    {40, 0, 0, 0.7, {2898, 268, 0}, {0, -60.0, 0.0}},
    {60, 0, 0, 0.4, {2898, 268, 0}, {0, -60.0, 0.0}},
    {70, 0, 0, -2.9, {2898, 268, 0}, {0, -60.0, 0.0}},
    {80, 0, 0, -12.4, {2898, 268, 0}, {0, -60.0, 0.0}},
    {85, 0, 0, -1.3, {3895, 268, 0}, {0, -60.0, 0.0}},
    {125, 0, 0, 2.6, {3895, 268, 0}, {0, -60.0, 0.0}},
    {160, 0, 0, -3.4, {3895, 268, 0}, {0, -60.0, 0.0}},
    {165, 0, 0, -4.9, {3895, 268, 0}, {0, -31.7, 0.0}},
    {170, 0, 0, -6.7, {3895, 379, 0}, {0, -17.6, 0.0}},
    {175, 0, 0, -26.2, {422, 414, 0}, {0, -4.2, 0.0}},
    {180, 0, 0, -27.7, {391, 997, 0}, {0, -0.4, 0.0}},
    {185, 0, 0, -30.1, {373, 997, 0}, {0, -4.3, 0.0}},
    {190, 0, 0, -30.7, {274, 1003, 0}, {0, -10.1, 0.0}},
    {195, 0, 0, -31.5, {239, 997, 0}, {0, -0.1, 0.0}},
    {200, 0, 0, -33.1, {239, 997, 0}, {0, -60.0, 0.0}},
    /* B8 */
    {5, 0, 0, -33.8, {239, 0, 0}, {0, 0.0, 0.0}},
    {15, 0, 0, -33.8, {239, 0, 0}, {0, 0.0, 0.0}},
    {20, 0, 0, -17.3, {245, 0, 0}, {0, 0.0, 0.0}},
    {25, 0, 0, -9.5, {2053, 0, 0}, {0, 0.0, 0.0}},
    {35, 0, 0, -5.9, {2053, 0, 0}, {0, 0.0, 0.0}},
    {150, 0, 0, -4.2, {1481, 0, 0}, {0, 0.0, 0.0}},
    {220, 0, 0, -3.7, {1324, 0, 0}, {0, 0.0, 0.0}},
    {290, 0, 0, -5.5, {1300, 0, 0}, {0, 0.0, 0.0}},
    {320, 0, 0, -13.1, {1300, 0, 0}, {0, 0.0, 0.0}},
    {340, 0, 0, -28.1, {1271, 0, 0}, {0, 0.0, 0.0}},
    {345, 0, 0, -33.6, {239, 0, 0}, {0, 0.0, 0.0}},
    {355, 0, 0, -33.8, {227, 0, 0}, {0, 0.0, 0.0}},
    /* B9 */
    {5, 0, 0, -34.7, {233, 898, 2700}, {0, -60.0, -60.0}},
    {20, 0, 0, -28.7, {245, 898, 2700}, {0, -60.0, -60.0}},
    {25, 0, 0, -15.5, {894, 898, 2700}, {0, -60.0, -60.0}},
    {30, 0, 0, -12.1, {896, 898, 2700}, {0, -0.0, -60.0}},
    {35, 0, 0, -8.2, {1802, 898, 2700}, {0, -4.4, 1.4}},
    {80, 0, 0, -7.6, {1802, 904, 2700}, {0, -3.9, 1.2}},
    {85, 0, 0, -10.8, {900, 268, 2700}, {0, 0.1, -60.0}},
    {95, 0, 0, -15.2, {787, 268, 2700}, {0, -12.6, -60.0}},
    {100, 0, 0, -12.6, {776, 408, 2700}, {0, -11.9, -60.0}},
    {115, 0, 0, -8.8, {676, 362, 2700}, {0, -11.6, -60.0}},
    {120, 0, 0, -9.3, {589, 344, 2700}, {0, -13.8, -60.0}},
    {125, 0, 0, -6.3, {563, 344, 2700}, {0, -60.0, -60.0}},
    {140, 0, 0, -5.2, {524, 344, 2700}, {0, -60.0, -60.0}},
    {145, 2, 87, -5.3, {524, 344, 268}, {0, -60.0, -60.0}},
    {150, 2, 87, -4.8, {525, 344, 268}, {0, -60.0, -60.0}},
    {155, 0, 0, -6.1, {525, 286, 268}, {0, -60.0, -60.0}},
    {160, 2, 87, -4.8, {525, 286, 268}, {0, -60.0, -60.0}},
    {165, 0, 0, -6.8, {525, 286, 268}, {0, -60.0, -60.0}},
    {175, 0, 0, -6.2, {543, 286, 268}, {0, -60.0, -60.0}},
    {180, 0, 0, -6.0, {590, 286, 268}, {0, 0.1, -60.0}},
    {185, 0, 0, -5.8, {636, 414, 268}, {0, -10.9, -60.0}},
    {195, 0, 0, -6.3, {717, 437, 268}, {0, -11.1, -60.0}},
    {200, 0, 0, -8.5, {764, 507, 268}, {0, 0.1, 0.1}},
    {210, 0, 0, -6.9, {892, 531, 268}, {0, -10.9, -14.9}},
    {215, 0, 0, -7.1, {892, 898, 601}, {0, -0.0, 0.1}},
    {220, 0, 0, -4.9, {1802, 898, 3493}, {0, -3.0, 3.0}},
    {270, 0, 0, -7.6, {1802, 904, 3493}, {0, -6.7, 2.8}},
    {275, 0, 0, -8.3, {1802, 3493, 898}, {0, 2.8, -12.0}},
    {285, 0, 0, -10.6, {1802, 3493, 892}, {0, 2.8, -1.9}},
    {290, 0, 0, -12.0, {1802, 3493, 881}, {0, 2.8, -16.3}},
    {295, 0, 0, -14.1, {1802, 3493, 881}, {0, 2.7, -60.0}},
    {300, 0, 0, -16.2, {1802, 3493, 881}, {0, 2.8, -60.0}},
    {305, 0, 0, -21.4, {932, 1802, 881}, {0, 1.9, -60.0}},
    {310, 0, 0, -24.9, {884, 986, 881}, {0, 0.0, -60.0}},
    {315, 0, 0, -28.6, {257, 986, 881}, {0, -60.0, -60.0}},
    {320, 0, 0, -31.1, {245, 467, 881}, {0, -13.3, -60.0}},
    {325, 0, 0, -34.7, {239, 467, 881}, {0, -60.0, -60.0}},
    {330, 0, 0, -34.7, {239, 467, 881}, {0, -60.0, -60.0}},
    /* C0 */
    {5, 0, 0, -26.4, {239, 991, 1545}, {0, -14.9, -60.0}},
    {15, 0, 0, -26.4, {239, 991, 1545}, {0, -14.9, -60.0}},
    {20, 0, 0, -26.4, {239, 991, 1545}, {0, -0.1, -60.0}},
    {25, 0, 0, -4.2, {2315, 991, 1545}, {0, -5.4, -60.0}},
    {30, 0, 0, -3.9, {2315, 2058, 1545}, {0, -14.2, -60.0}},
    {35, 0, 0, -0.8, {2315, 2058, 1545}, {0, -16.1, -60.0}},
    {40, 0, 0, -2.1, {2058, 2058, 1545}, {0, -2.2, -60.0}},
    {45, 0, 0, -2.7, {2053, 2058, 1545}, {0, 0.0, -2.6}},
    {55, 0, 0, -4.0, {1790, 1767, 2327}, {0, -13.5, -10.6}},
    {70, 0, 0, -1.1, {1528, 1767, 2327}, {0, -11.8, -9.0}},
    {75, 0, 0, -1.0, {1265, 1767, 2327}, {0, -3.5, -4.9}},
    {80, 0, 0, -1.4, {1265, 2298, 1277}, {0, -6.9, -14.6}},
    {85, 0, 0, 0.5, {1260, 1516, 2327}, {0, -0.6, -2.9}},
    {90, 0, 0, -1.4, {1009, 1015, 2239}, {0, -11.7, -9.3}},
    {95, 0, 0, 1.4, {991, 1003, 2239}, {0, -8.1, -5.9}},
    {100, 0, 0, 0.7, {781, 764, 2239}, {0, -10.2, -8.0}},
    {105, 0, 0, 1.7, {764, 577, 2239}, {0, -13.7, 4.8}},
    {110, 0, 0, 2.3, {723, 402, 245}, {0, -12.3, -13.3}},
    {115, 0, 0, 0.2, {647, 309, 245}, {0, 0.1, -60.0}},
    {120, 0, 0, 2.8, {630, 309, 245}, {0, -60.0, -60.0}},
    {125, 0, 0, 0.9, {519, 309, 245}, {0, -60.0, -60.0}},
    {130, 0, 0, 2.5, {501, 309, 245}, {0, -60.0, -60.0}},
    {135, 0, 0, 3.1, {362, 309, 245}, {0, -60.0, -60.0}},
    {140, 0, 0, 0.6, {344, 309, 245}, {0, -60.0, -60.0}},
    {145, 0, 0, 3.6, {338, 455, 245}, {0, -60.0, -60.0}},
    {150, 0, 0, 2.0, {198, 455, 583}, {0, -60.0, -60.0}},
    {165, 0, 0, -5.1, {198, 455, 583}, {0, -60.0, -60.0}},
    {170, 0, 0, -6.2, {297, 455, 583}, {0, -6.3, -60.0}},
    {175, 0, 0, -5.8, {327, 595, 583}, {0, -0.1, -60.0}},
    {180, 0, 0, -7.0, {297, 455, 583}, {0, -6.0, -14.7}},
    {185, 0, 0, -9.9, {327, 595, 787}, {0, -2.7, -6.1}},
    {190, 0, 0, -9.0, {327, 595, 898}, {0, -6.4, -9.9}},
    {200, 0, 0, -13.4, {251, 484, 787}, {0, -8.2, -13.8}},
    {205, 0, 0, -12.7, {251, 566, 881}, {0, -9.3, -12.0}},
    {210, 0, 0, -14.5, {251, 484, 881}, {0, -0.0, -0.1}},
    {215, 0, 0, -17.3, {216, 490, 881}, {0, -8.8, -60.0}},
    {220, 0, 0, -17.1, {216, 490, 881}, {0, -0.0, -60.0}},
    {225, 0, 0, -20.7, {268, 490, 881}, {0, -10.7, -60.0}},
    {240, 0, 0, -26.4, {239, 461, 881}, {0, -0.0, -60.0}},
    {245, 0, 0, -26.4, {239, 461, 881}, {0, -60.0, -60.0}},
    /* C1 */
    {5, 0, 0, -28.7, {239, 449, 426}, {0, -60.0, -60.0}},
    {10, 0, 0, -28.7, {239, 449, 426}, {0, -13.5, -60.0}},
    {15, 0, 0, -28.7, {245, 449, 426}, {0, -14.1, -60.0}},
    {20, 0, 0, -24.4, {653, 478, 426}, {0, -1.8, -14.5}},
    {25, 0, 0, -17.8, {671, 676, 426}, {0, -0.0, 0.1}},
    {30, 0, 0, -9.4, {1096, 676, 682}, {0, -1.5, -8.1}},
    {40, 0, 0, -4.5, {1155, 834, 682}, {0, -1.1, -7.9}},
    {45, 0, 0, -3.2, {1155, 1388, 933}, {0, -3.6, -6.0}},
    {55, 0, 0, -1.4, {1423, 1400, 933}, {0, -4.9, -8.5}},
    {65, 0, 0, -0.7, {986, 332, 1609}, {0, -3.9, -3.6}},
    {70, 0, 0, 0.2, {741, 332, 1609}, {0, -2.9, -4.0}},
    {80, 0, 0, 1.2, {741, 350, 1522}, {0, -9.0, -13.9}},
    {95, 0, 0, -1.6, {834, 420, 869}, {0, -0.5, -2.4}},
    {100, 0, 0, -2.4, {1283, 1347, 869}, {0, -0.1, -13.0}},
    {105, 0, 0, -2.5, {1341, 1347, 846}, {0, 0.0, -0.3}},
    {110, 0, 0, 0.5, {2029, 1265, 840}, {0, -4.4, -10.4}},
    {120, 0, 0, -0.1, {1989, 1598, 840}, {0, -4.2, -8.0}},
    {125, 0, 0, -0.4, {1989, 1598, 408}, {0, -4.5, -9.8}},
    {140, 0, 0, -3.5, {1528, 1895, 385}, {0, -2.8, -5.9}},
    {145, 0, 0, -4.2, {1528, 1895, 1353}, {0, 0.4, -2.3}},
    {185, 0, 0, -6.4, {1446, 1994, 1219}, {0, 1.5, -6.9}},
    {190, 0, 0, -7.3, {1405, 1994, 1954}, {0, 1.2, -4.9}},
    {195, 0, 0, -8.7, {1359, 1994, 1160}, {0, -2.2, -5.9}},
    {225, 0, 0, -12.2, {1114, 1749, 1353}, {0, -0.0, -2.4}},
    {235, 0, 0, -13.5, {1079, 986, 1674}, {0, -0.6, -2.1}},
    {245, 0, 0, -15.0, {1073, 986, 1662}, {0, -3.1, -2.0}},
    {265, 0, 0, -18.3, {834, 2507, 1662}, {0, 2.5, -4.3}},
    {270, 0, 0, -18.7, {787, 2682, 1528}, {0, 4.0, -3.7}},
    {280, 0, 0, -22.3, {741, 2688, 239}, {0, 3.0, -6.2}},
    {285, 0, 0, -23.0, {741, 2688, 227}, {0, 7.4, -4.0}},
    {290, 0, 0, -16.5, {2805, 630, 227}, {0, -10.1, -13.6}},
    {295, 0, 0, -16.6, {2805, 222, 192}, {0, -9.2, -9.2}},
    {300, 0, 0, -18.2, {2805, 222, 192}, {0, -14.5, -17.8}},
    {305, 0, 0, -27.5, {519, 222, 490}, {0, 0.0, 0.0}},
    {310, 0, 0, -28.7, {490, 280, 799}, {0, -3.2, -4.5}},
    {315, 0, 0, -28.7, {245, 472, 886}, {0, -2.0, -4.9}},
    {325, 0, 0, -28.7, {239, 472, 799}, {0, -11.2, -12.6}},
    {330, 0, 0, -28.7, {227, 472, 799}, {0, -14.7, -60.0}},
    /* C2 */
    {5, 0, 0, -29.0, {233, 216, 974}, {0, -60.0, -60.0}},
    {10, 0, 0, -29.0, {239, 216, 974}, {0, -60.0, -60.0}},
    {15, 0, 0, -29.0, {239, 216, 974}, {0, 0.0, -60.0}},
    {25, 0, 0, -24.1, {455, 216, 974}, {0, -2.1, -60.0}},
    {30, 0, 0, -24.1, {478, 910, 974}, {0, -5.1, -0.1}},
    {35, 0, 0, -6.8, {499, 904, 974}, {0, -0.1, -0.1}},
    {40, 0, 0, -6.4, {501, 904, 974}, {0, -2.9, -60.0}},
    {45, 0, 0, -6.4, {507, 904, 169}, {0, -2.8, -11.0}},
    {50, 0, 0, -0.8, {507, 910, 2210}, {0, -0.1, 4.5}},
    {55, 0, 0, -1.4, {507, 916, 2198}, {0, -2.7, -2.8}},
    {60, 0, 0, -1.5, {507, 921, 175}, {0, -2.6, -10.3}},
    {65, 0, 0, 0.9, {519, 933, 2012}, {0, -2.9, -7.5}},
    {75, 0, 0, -1.6, {525, 986, 2006}, {0, -3.2, -6.4}},
    {80, 0, 0, 0.7, {531, 1009, 239}, {0, -4.3, -9.4}},
    {85, 0, 0, -1.0, {543, 1020, 1814}, {0, -0.0, 1.9}},
    {90, 0, 0, -1.9, {548, 986, 239}, {0, -8.3, -10.9}},
    {95, 0, 0, 0.1, {549, 986, 239}, {0, -60.0, -60.0}},
    {100, 0, 0, -1.6, {566, 1114, 239}, {0, -0.0, 0.1}},
    {105, 0, 0, -2.9, {566, 1114, 239}, {0, -60.0, -60.0}},
    {110, 0, 0, -1.6, {566, 1155, 245}, {0, -60.0, -60.0}},
    {115, 0, 0, -3.9, {566, 1155, 245}, {0, 0.0, 0.1}},
    {135, 0, 0, -9.7, {560, 1172, 245}, {0, -3.5, -9.9}},
    {140, 0, 0, -9.5, {554, 1172, 344}, {0, -7.0, -12.4}},
    {165, 0, 0, -25.7, {449, 1155, 251}, {0, -14.3, 0.0}},
    {170, 0, 0, -25.2, {449, 1155, 251}, {0, -9.1, -60.0}},
    {175, 0, 0, -22.7, {449, 175, 251}, {0, 0.0, -60.0}},
    {180, 0, 0, -9.3, {3312, 175, 251}, {0, -60.0, -60.0}},
    {185, 0, 0, -8.0, {3336, 175, 251}, {0, -60.0, -60.0}},
    {190, 0, 0, -3.9, {3336, 175, 251}, {0, -13.1, -60.0}},
    {195, 0, 0, -4.4, {3336, 175, 618}, {0, -26.6, -60.0}},
    {200, 0, 0, -4.2, {3336, 175, 618}, {0, -60.0, -60.0}},
    {205, 0, 0, 0.3, {3341, 192, 618}, {0, -13.2, -60.0}},
    {215, 0, 0, -1.0, {3341, 216, 618}, {0, -26.5, -60.0}},
    {220, 0, 0, -0.8, {3341, 297, 618}, {0, -17.5, -22.8}},
    {225, 0, 0, -20.7, {1674, 315, 618}, {0, -6.6, -10.5}},
    {230, 0, 0, -21.3, {1668, 315, 618}, {0, -7.2, -10.7}},
    {235, 0, 0, -22.4, {262, 472, 618}, {0, -2.9, -9.2}},
    {240, 0, 0, -28.0, {245, 472, 484}, {0, -14.6, -0.0}},
    {245, 0, 0, -29.0, {239, 472, 484}, {0, -12.0, -60.0}},
    {250, 0, 0, -29.0, {233, 472, 484}, {0, -60.0, -60.0}},
    /* C3 */
    {5, 0, 0, -28.8, {222, 490, 414}, {0, -60.0, -60.0}},
    {10, 0, 0, -27.7, {239, 490, 414}, {0, -12.0, -60.0}},
    {15, 0, 0, -27.0, {239, 490, 414}, {0, -13.2, -60.0}},
    {20, 0, 0, -22.9, {268, 490, 414}, {0, -7.4, -60.0}},
    {25, 0, 0, -9.4, {3143, 216, 414}, {0, -11.7, -60.0}},
    {30, 0, 0, -8.3, {3161, 175, 414}, {0, -19.1, -60.0}},
    {40, 0, 0, -3.0, {3166, 187, 414}, {0, -11.8, -60.0}},
    {45, 0, 0, -3.2, {3172, 175, 414}, {0, -25.7, -60.0}},
    {50, 0, 0, -0.9, {3166, 175, 414}, {0, -60.0, -60.0}},
    {115, 0, 0, 3.8, {3376, 175, 414}, {0, -60.0, -60.0}},
    {120, 0, 0, 3.9, {3470, 204, 414}, {0, -60.0, -60.0}},
    {180, 0, 0, 4.3, {3785, 204, 414}, {0, -60.0, -60.0}},
    {185, 0, 0, 4.4, {3785, 204, 414}, {0, -16.8, -60.0}},
    {190, 0, 0, 4.4, {3785, 204, 414}, {0, -16.8, -60.0}},
    {195, 0, 0, 4.4, {3785, 204, 414}, {0, -26.4, -60.0}},
    {200, 0, 0, 0.5, {3785, 338, 414}, {0, -18.9, -60.0}},
    {205, 0, 0, 0.7, {3773, 338, 414}, {0, -30.2, -60.0}},
    {210, 0, 0, 0.6, {3773, 356, 414}, {0, -31.1, -16.8}},
    {215, 0, 0, 0.6, {3773, 356, 414}, {0, -27.2, -31.4}},
    {220, 0, 0, -18.9, {233, 3779, 571}, {0, 10.3, -8.6}},
    {225, 0, 0, -20.3, {233, 3779, 741}, {0, 16.5, -10.4}},
    {230, 0, 0, -20.5, {233, 3779, 571}, {0, 16.7, -8.9}},
    {235, 0, 0, -20.3, {239, 3755, 571}, {0, 10.1, -8.0}},
    {240, 0, 0, -27.3, {239, 997, 571}, {0, -13.4, -60.0}},
    {250, 0, 0, -28.6, {239, 991, 571}, {0, -13.5, -60.0}},
    /* C4 */
    {5, 0, 0, -28.7, {239, 991, 0}, {0, -14.7, 0.0}},
    {20, 0, 0, -28.7, {239, 991, 0}, {0, -14.9, 0.0}},
    {25, 0, 0, -19.4, {927, 402, 0}, {0, -8.5, 0.0}},
    {35, 0, 0, -12.2, {962, 350, 0}, {0, -13.7, 0.0}},
    {45, 0, 0, -9.2, {962, 338, 0}, {0, 0.1, 0.0}},
    {50, 0, 0, -3.9, {962, 735, 0}, {0, -7.4, 0.0}},
    {60, 0, 0, -2.2, {962, 735, 0}, {0, -14.8, 0.0}},
    {70, 0, 0, -1.9, {962, 735, 0}, {0, 0.0, 0.0}},
    {75, 0, 0, 0.9, {956, 735, 0}, {0, -60.0, 0.0}},
    {85, 0, 0, 0.8, {956, 735, 0}, {0, -60.0, 0.0}},
    {90, 0, 0, 1.0, {839, 735, 0}, {0, -60.0, 0.0}},
    {95, 0, 0, 0.3, {839, 735, 0}, {0, -60.0, 0.0}},
    {100, 2, 87, 1.1, {839, 257, 0}, {0, -60.0, 0.0}},
    {105, 0, 0, -0.5, {839, 257, 0}, {0, -60.0, 0.0}},
    {110, 0, 0, 1.2, {956, 257, 0}, {0, -60.0, 0.0}},
    {115, 0, 0, -0.6, {956, 257, 0}, {0, -60.0, 0.0}},
    {120, 0, 0, -0.7, {956, 257, 0}, {0, 0.1, 0.0}},
    {125, 0, 0, -1.5, {956, 962, 0}, {0, -4.1, 0.0}},
    {130, 0, 0, -2.8, {1913, 986, 0}, {0, -2.5, 0.0}},
    {135, 0, 0, -2.5, {1913, 1330, 0}, {0, -10.2, 0.0}},
    {140, 0, 0, -4.4, {1913, 986, 0}, {0, -17.1, 0.0}},
    {145, 0, 0, -3.5, {1913, 986, 0}, {0, -10.8, 0.0}},
    {150, 1, 0, -5.1, {1913, 986, 0}, {0, -60.0, 0.0}},
    {155, 1, 0, -5.7, {974, 986, 0}, {0, -60.0, 0.0}},
    {160, 1, 0, -7.6, {974, 986, 0}, {0, -60.0, 0.0}},
    {165, 1, 0, -5.5, {1913, 986, 0}, {0, -60.0, 0.0}},
    {175, 1, 0, -9.3, {1914, 986, 0}, {0, -60.0, 0.0}},
    {180, 1, 0, -1.0, {2957, 222, 0}, {0, -60.0, 0.0}},
    {185, 1, 0, -3.8, {2957, 222, 0}, {0, -60.0, 0.0}},
    {190, 0, 0, -2.5, {2957, 222, 0}, {0, -60.0, 0.0}},
    {205, 0, 0, -2.8, {2962, 222, 0}, {0, -60.0, 0.0}},
    {210, 0, 0, -3.4, {2980, 222, 0}, {0, -25.3, 0.0}},
    {215, 0, 0, -3.2, {2986, 222, 0}, {0, -10.5, 0.0}},
    {220, 0, 0, -7.0, {2986, 222, 0}, {0, -24.2, 0.0}},
    {230, 0, 0, -8.5, {2986, 227, 0}, {0, -10.5, 0.0}},
    {235, 0, 0, -9.9, {2986, 239, 0}, {0, -21.4, 0.0}},
    {240, 0, 0, -20.6, {239, 688, 0}, {0, -4.7, 0.0}},
    {245, 0, 0, -27.1, {239, 997, 0}, {0, -12.9, 0.0}},
    {255, 0, 0, -28.7, {239, 997, 0}, {0, -15.0, 0.0}},
    {265, 0, 0, -28.7, {239, 478, 0}, {0, -12.0, 0.0}},
    /* C5 */
    {5, 0, 0, -28.7, {239, 467, 297}, {0, -60.0, -60.0}},
    {10, 0, 0, -28.7, {239, 467, 297}, {0, -0.0, -60.0}},
    {15, 0, 0, -28.7, {239, 665, 297}, {0, -14.5, -60.0}},
    {20, 0, 0, -28.7, {239, 665, 297}, {0, -2.1, -60.0}},
    {25, 0, 0, -24.8, {309, 764, 297}, {0, -10.9, -60.0}},
    {30, 0, 0, -12.4, {2694, 764, 297}, {0, -11.0, -60.0}},
    {40, 0, 0, -9.1, {2700, 764, 297}, {0, -10.6, -60.0}},
    {70, 0, 0, -8.6, {2694, 764, 297}, {0, -10.2, -60.0}},
    {75, 0, 0, -15.1, {770, 764, 297}, {0, 0.0, -60.0}},
    {80, 0, 0, -12.5, {746, 892, 297}, {0, -10.8, -60.0}},
    {110, 0, 0, -1.8, {618, 898, 297}, {0, -3.3, -60.0}},
    {115, 0, 0, -1.2, {606, 898, 297}, {0, -3.3, -13.8}},
    {125, 0, 0, -0.8, {595, 898, 297}, {0, -4.0, -5.3}},
    {130, 0, 0, -0.4, {595, 297, 898}, {0, -2.8, -4.9}},
    {145, 0, 0, -0.1, {595, 297, 892}, {0, -0.1, -14.9}},
    {150, 0, 0, 0.0, {297, 297, 892}, {0, 0.0, -60.0}},
    {160, 0, 0, -0.1, {297, 297, 892}, {0, -0.3, -60.0}},
    {165, 0, 0, -0.1, {496, 297, 892}, {0, -0.5, -60.0}},
    {175, 0, 0, -0.1, {496, 297, 892}, {0, -0.7, -60.0}},
    {185, 0, 0, -0.2, {432, 297, 892}, {0, -0.0, -60.0}},
    {190, 0, 0, -0.1, {338, 192, 892}, {0, -14.9, -60.0}},
    {195, 0, 0, -0.3, {379, 181, 892}, {0, 0.0, -60.0}},
    {200, 0, 0, -0.8, {338, 181, 892}, {0, -60.0, -60.0}},
    {205, 0, 0, -0.3, {309, 181, 892}, {0, -60.0, -60.0}},
    {215, 0, 0, 0.2, {297, 181, 892}, {0, -60.0, -60.0}},
    {240, 0, 0, -3.0, {297, 181, 892}, {0, -60.0, -60.0}},
    {245, 0, 0, -3.1, {268, 181, 892}, {0, -60.0, -60.0}},
    {250, 0, 0, -3.2, {181, 181, 892}, {0, -60.0, -60.0}},
    {270, 0, 0, -5.6, {175, 181, 892}, {0, -60.0, -60.0}},
    {275, 0, 0, -6.0, {175, 362, 892}, {0, -60.0, -60.0}},
    {345, 0, 0, -7.4, {175, 362, 892}, {0, -60.0, -60.0}},
    {350, 0, 0, -9.8, {175, 362, 892}, {0, -0.0, -60.0}},
    {355, 0, 0, -10.9, {175, 373, 892}, {0, -13.6, -60.0}},
    {360, 0, 0, -27.4, {187, 612, 892}, {0, -11.8, -60.0}},
    {365, 0, 0, -28.4, {198, 612, 892}, {0, -9.6, -60.0}},
    {370, 0, 0, -28.7, {239, 991, 892}, {0, -5.0, -60.0}},
    {375, 0, 0, -28.7, {239, 991, 892}, {0, -14.5, -60.0}},
    {380, 0, 0, -28.7, {239, 991, 892}, {0, -13.8, -60.0}},
    /* C6 */
    {5, 0, 0, -28.2, {233, 478, 583}, {0, -60.0, -60.0}},
    {10, 0, 0, -28.2, {233, 478, 583}, {0, -0.0, -60.0}},
    {15, 0, 0, -28.2, {239, 980, 583}, {0, -13.4, -0.1}},
    {20, 0, 0, -27.6, {239, 980, 583}, {0, -11.9, -12.9}},
    {25, 0, 0, -22.5, {2012, 991, 595}, {0, -14.7, -15.1}},
    {30, 0, 0, -22.1, {2029, 980, 997}, {0, -10.5, -15.7}},
    {35, 0, 0, -13.4, {2041, 227, 595}, {0, -7.5, -9.8}},
    {40, 0, 0, -13.0, {2029, 227, 1569}, {0, -14.8, -17.3}},
    {55, 0, 0, -8.2, {2023, 227, 1569}, {0, -3.1, -60.0}},
    {65, 0, 0, -4.2, {2023, 227, 1569}, {0, -60.0, -60.0}},
    {70, 0, 0, -4.9, {2023, 1475, 1125}, {0, -60.0, -60.0}},
    {75, 0, 0, -5.2, {2023, 1475, 1125}, {0, -2.6, -60.0}},
    {80, 0, 0, -1.4, {2023, 1475, 1125}, {0, -60.0, -60.0}},
    {90, 0, 0, -1.9, {1738, 1475, 1125}, {0, -0.9, -1.5}},
    {100, 0, 0, -0.6, {1376, 1143, 1125}, {0, -14.2, -14.8}},
    {110, 0, 0, 1.1, {1067, 1143, 2799}, {0, -9.8, -2.4}},
    {130, 0, 0, -1.4, {904, 647, 641}, {0, 0.0, 0.0}},
    {135, 0, 0, -2.3, {904, 1942, 641}, {0, 2.7, 0.0}},
    {140, 0, 0, -2.8, {904, 1942, 910}, {0, 1.8, -7.0}},
    {145, 0, 0, -1.9, {1930, 1942, 910}, {0, -1.8, -6.2}},
    {150, 0, 0, -1.8, {1936, 1942, 1679}, {0, -9.1, -13.1}},
    {160, 0, 0, -2.0, {1930, 898, 910}, {0, -5.1, -8.5}},
    {165, 0, 0, -3.0, {906, 898, 647}, {0, -1.4, -10.3}},
    {175, 0, 0, -1.8, {898, 898, 647}, {0, -60.0, -60.0}},
    {180, 0, 0, 0.7, {904, 641, 2782}, {0, -60.0, -60.0}},
    {185, 0, 0, 1.2, {904, 641, 2782}, {0, -13.7, -5.0}},
    {200, 0, 0, -0.0, {1225, 1096, 2898}, {0, -0.5, 7.4}},
    {205, 0, 0, -0.4, {1330, 1073, 1382}, {0, -10.7, -12.7}},
    {240, 0, 0, -6.0, {2029, 1096, 1195}, {0, -60.0, -60.0}},
    {245, 0, 0, -5.1, {2035, 338, 338}, {0, -60.0, -60.0}},
    {265, 0, 0, -9.7, {2035, 338, 338}, {0, -60.0, -60.0}},
    {280, 0, 0, -16.1, {2035, 571, 531}, {0, -7.5, -11.0}},
    {285, 0, 0, -28.2, {239, 571, 531}, {0, -8.0, -10.3}},
    {295, 0, 0, -28.2, {239, 764, 566}, {0, -14.6, -0.1}},
    {300, 0, 0, -28.2, {239, 764, 566}, {0, -10.4, -60.0}},
    {305, 0, 0, -28.2, {239, 478, 566}, {0, -13.0, -60.0}},
    /* C7 */
    {5, 0, 0, -28.4, {239, 472, 332}, {0, -60.0, -60.0}},
    {10, 0, 0, -28.4, {239, 472, 332}, {0, -12.2, -60.0}},
    {20, 0, 0, -28.4, {245, 472, 332}, {0, -12.2, -60.0}},
    {25, 0, 0, -26.0, {490, 262, 332}, {0, -4.9, -60.0}},
    {35, 0, 0, -14.6, {513, 210, 332}, {0, 0.0, -60.0}},
    {45, 0, 0, -13.3, {519, 210, 332}, {0, -60.0, -60.0}},
    {50, 0, 0, -7.3, {519, 210, 332}, {0, -9.2, -60.0}},
    {65, 0, 0, -3.8, {519, 309, 332}, {0, -6.1, -60.0}},
    {70, 0, 0, -5.0, {519, 548, 332}, {0, -0.0, -60.0}},
    {75, 0, 0, -2.6, {525, 630, 332}, {0, -8.5, -14.1}},
    {90, 0, 0, -3.9, {1382, 939, 1073}, {0, -5.6, -6.6}},
    {110, 0, 0, -7.3, {2035, 1458, 1726}, {0, -8.6, -9.5}},
    {120, 0, 0, -4.4, {2035, 1458, 1306}, {0, -7.0, -7.9}},
    {130, 0, 0, -4.3, {945, 729, 711}, {0, -0.3, -1.3}},
    {140, 0, 0, -4.3, {945, 729, 811}, {0, -11.8, -14.8}},
    {155, 0, 0, -4.5, {1860, 1499, 1580}, {0, -3.4, -13.9}},
    {165, 0, 0, -4.2, {1860, 2035, 1213}, {0, -3.0, -9.5}},
    {175, 0, 0, -3.1, {758, 1230, 997}, {0, -2.0, -2.7}},
    {185, 0, 0, -1.8, {758, 986, 904}, {0, -3.0, -5.8}},
    {200, 0, 0, -3.9, {1580, 1277, 1330}, {0, -1.2, -9.3}},
    {215, 0, 0, -2.1, {1668, 1271, 1487}, {0, -2.0, -3.5}},
    {220, 0, 0, -1.7, {571, 1230, 968}, {0, -1.3, -2.9}},
    {230, 0, 0, 0.0, {571, 793, 776}, {0, -0.2, -9.3}},
    {240, 0, 0, -0.7, {1015, 974, 781}, {0, -3.3, -10.7}},
    {260, 0, 0, -1.1, {1260, 1020, 1032}, {0, -3.4, -6.4}},
    {270, 0, 0, 0.8, {706, 606, 589}, {0, -5.3, -11.0}},
    {280, 0, 0, 1.3, {706, 688, 443}, {0, -3.8, -10.8}},
    {285, 0, 0, 0.9, {706, 688, 227}, {0, -9.1, -11.2}},
    {300, 0, 0, -0.3, {945, 951, 694}, {0, -5.3, -12.3}},
    {310, 0, 0, 0.5, {676, 951, 694}, {0, -0.3, -3.9}},
    {315, 0, 0, 1.0, {641, 659, 239}, {0, -3.1, -8.9}},
    {335, 0, 0, 0.6, {426, 601, 204}, {0, -9.5, -12.3}},
    {360, 0, 0, -4.4, {484, 321, 157}, {0, -9.4, -14.5}},
    {365, 0, 0, -5.7, {484, 292, 157}, {0, -10.4, -60.0}},
    {405, 0, 0, -27.2, {484, 227, 157}, {0, -4.2, -60.0}},
    {410, 0, 0, -28.4, {239, 227, 157}, {0, -13.8, -60.0}},
    {420, 0, 0, -28.4, {239, 227, 157}, {0, -60.0, -60.0}},
    /* C8 */
    {5, 0, 0, -29.7, {239, 280, 402}, {0, -60.0, -60.0}},
    {10, 0, 0, -29.7, {239, 280, 402}, {0, -60.0, -60.0}},
    {15, 0, 0, -29.7, {239, 280, 402}, {0, -13.1, -60.0}},
    {20, 0, 0, -17.8, {951, 991, 402}, {0, -1.2, -1.3}},
    {25, 0, 0, -12.0, {974, 991, 402}, {0, -2.2, -4.6}},
    {30, 0, 0, -9.6, {1335, 280, 402}, {0, -0.5, -1.2}},
    {35, 0, 0, -6.3, {1335, 1195, 402}, {0, -0.2, -0.2}},
    {40, 0, 0, -1.0, {1335, 886, 729}, {0, -0.3, -0.3}},
    {45, 0, 0, -0.1, {1184, 886, 729}, {0, -60.0, -60.0}},
    {50, 0, 0, -0.1, {1184, 886, 729}, {0, -60.0, -60.0}},
    {55, 1, 0, 0.1, {1172, 886, 729}, {0, -0.1, -0.1}},
    {60, 1, 0, 0.3, {1178, 886, 729}, {0, -0.1, -0.1}},
    {65, 0, 0, -0.0, {1178, 886, 729}, {0, -60.0, -60.0}},
    {70, 0, 0, 1.2, {1691, 886, 1370}, {0, -60.0, -60.0}},
    {75, 1, 0, 0.1, {1178, 886, 1370}, {0, -0.1, 0.3}},
    {80, 1, 0, 0.2, {1178, 886, 676}, {0, -0.1, -0.1}},
    {85, 0, 0, 1.2, {1691, 886, 676}, {0, -60.0, -60.0}},
    {90, 0, 0, 0.1, {1306, 892, 676}, {0, -60.0, -60.0}},
    {95, 0, 0, 0.2, {1178, 892, 676}, {0, -0.1, -0.1}},
    {100, 0, 0, -0.1, {1178, 892, 531}, {0, -0.1, -0.0}},
    {105, 0, 0, -0.6, {1190, 986, 531}, {0, -0.9, -2.3}},
    {110, 0, 0, -1.0, {840, 986, 531}, {0, -0.5, -1.4}},
    {115, 0, 0, -2.6, {840, 1125, 531}, {0, -2.4, -2.9}},
    {120, 0, 0, -16.5, {793, 986, 373}, {0, -0.0, 0.1}},
    {125, 0, 0, -29.3, {251, 986, 373}, {0, -2.7, -60.0}},
    {130, 0, 0, -29.7, {245, 536, 373}, {0, -12.9, -60.0}},
    {135, 0, 0, -29.7, {245, 536, 373}, {0, -14.6, -60.0}},
    {140, 0, 0, -29.7, {239, 478, 373}, {0, -11.2, -60.0}},
    {145, 0, 0, -29.7, {239, 478, 373}, {0, -60.0, -60.0}},
    /* C9 */
    {5, 0, 0, -25.6, {239, 997, 1114}, {0, -14.5, -60.0}},
    {15, 0, 0, -25.6, {239, 997, 1114}, {0, -14.9, -60.0}},
    {20, 0, 0, -24.0, {239, 997, 1114}, {0, -0.1, -60.0}},
    {25, 0, 0, -12.0, {898, 362, 1114}, {0, -4.5, -60.0}},
    {30, 0, 0, -8.6, {1003, 362, 1114}, {0, 0.1, -1.2}},
    {35, 0, 0, -5.0, {1709, 362, 1114}, {0, -1.3, -1.4}},
    {40, 0, 0, -0.7, {1709, 1155, 1114}, {0, -1.8, -2.6}},
    {45, 0, 0, 3.5, {1709, 1143, 595}, {0, -1.3, -1.4}},
    {50, 0, 0, 3.9, {1709, 1155, 1353}, {0, -2.3, -4.5}},
    {55, 0, 0, 2.7, {1102, 1155, 746}, {0, -0.1, -3.4}},
    {60, 0, 0, 3.3, {869, 1155, 1254}, {0, -0.5, -1.2}},
    {65, 0, 0, 3.3, {869, 1143, 1254}, {0, -0.3, -1.2}},
    {70, 0, 0, 3.4, {1102, 1475, 1260}, {0, -0.5, -3.8}},
    {75, 0, 0, 3.6, {980, 1073, 1260}, {0, -1.4, -2.7}},
    {80, 0, 0, 4.3, {799, 1067, 1260}, {0, -0.7, -1.6}},
    {85, 0, 0, 3.4, {799, 991, 1260}, {0, -0.8, -1.2}},
    {90, 0, 0, 2.9, {980, 991, 1283}, {0, -1.6, -6.2}},
    {95, 0, 0, 2.1, {980, 980, 1143}, {0, -2.0, -7.9}},
    {100, 0, 0, -2.4, {694, 991, 525}, {0, -0.3, -2.9}},
    {105, 0, 0, -19.0, {694, 991, 525}, {0, -0.0, 0.0}},
    {110, 0, 0, -25.6, {239, 397, 525}, {0, -0.8, -7.7}},
    {115, 0, 0, -25.6, {239, 397, 525}, {0, -13.5, -14.8}},
    {120, 0, 0, -25.6, {222, 397, 525}, {0, -0.0, -0.0}},
    {125, 0, 0, -25.6, {222, 397, 525}, {0, -60.0, -60.0}},
    /* M0 */
    {5, 0, 0, -30.5, {239, 986, 1627}, {0, -13.9, -60.0}},
    {15, 0, 0, -30.5, {239, 991, 1627}, {0, -14.8, -60.0}},
    {25, 0, 0, -2.4, {1755, 1609, 1627}, {0, -11.4, -60.0}},
    {35, 0, 0, 1.1, {1790, 1609, 1627}, {0, -12.9, -60.0}},
    {60, 0, 0, 0.8, {1814, 1574, 1627}, {0, -60.0, -60.0}},
    {180, 0, 0, -2.1, {1819, 1598, 1627}, {0, -60.0, -60.0}},
    {185, 0, 0, -2.1, {1819, 1598, 1627}, {0, -1.0, -60.0}},
    {210, 0, 0, -2.9, {1784, 1574, 1627}, {0, -0.9, -60.0}},
    {215, 0, 0, -3.1, {1784, 1574, 1627}, {0, -60.0, -60.0}},
    {930, 0, 0, -24.3, {1796, 1580, 1627}, {0, -14.7, -60.0}},
    {935, 0, 0, -24.8, {1796, 251, 1627}, {0, -16.4, -60.0}},
    {950, 0, 0, -24.7, {1819, 222, 1627}, {0, -15.6, -60.0}},
    {980, 0, 0, -24.7, {1773, 251, 1627}, {0, -60.0, -60.0}},
    {985, 0, 0, -24.8, {1796, 251, 1627}, {0, -11.5, -60.0}},
    {990, 0, 0, -25.0, {1825, 251, 1627}, {0, -12.3, -0.9}},
    {995, 0, 0, -25.7, {1825, 1475, 1627}, {0, -13.8, -13.4}},
    {1000, 0, 0, -26.1, {1825, 1475, 1627}, {0, -11.2, -15.2}},
    {1005, 0, 0, -26.6, {1814, 251, 1627}, {0, -14.1, -13.7}},
    {1010, 0, 0, -26.9, {1805, 245, 478}, {0, -12.5, -14.4}},
    {1030, 0, 0, -26.9, {1800, 245, 478}, {0, -60.0, -60.0}},
    {1035, 0, 0, -27.1, {1795, 245, 478}, {0, -1.7, -60.0}},
    {1060, 0, 0, -26.8, {1808, 233, 583}, {0, -10.6, -14.1}},
    {1075, 0, 0, -26.7, {1831, 227, 1679}, {0, -11.5, -12.2}},
    {1085, 0, 0, -26.9, {1819, 513, 1679}, {0, -13.5, -15.3}},
    {1110, 0, 0, -27.1, {1796, 292, 443}, {0, -14.4, -16.5}},
    {1130, 0, 0, -28.9, {1779, 612, 840}, {0, -8.9, -12.9}},
    {1135, 2, 128, -30.6, {957, 1781, 2639}, {0, 26.7, 11.7}},
    {1160, 2, 128, -30.6, {1014, 1784, 2636}, {0, 24.6, 10.0}},
    {1165, 0, 0, -28.8, {1790, 239, 1586}, {0, -9.2, -13.0}},
    {1210, 0, 0, -28.8, {1802, 251, 461}, {0, -4.6, -9.0}},
    {1215, 0, 0, -30.5, {239, 496, 443}, {0, -5.8, -7.1}},
    {1220, 0, 0, -30.5, {239, 991, 443}, {0, -12.5, -14.5}},
    {1230, 0, 0, -30.5, {233, 496, 397}, {0, -2.2, -60.0}},
    /* M2 */
    {5, 2, 128, -33.3, {540, 1687, 2856}, {0, 37.6, 43.5}},
    {10, 2, 177, -33.7, {540, 1707, 2858}, {0, 38.1, 44.6}},
    {15, 2, 498, -33.2, {540, 1707, 2866}, {0, 4.2, 8.3}},
    {20, 2, 498, -11.0, {539, 1703, 2866}, {0, 2.2, 6.6}},
    {25, 2, 85, -9.1, {530, 1703, 2866}, {0, 2.2, 6.6}},
    {45, 2, 87, -4.3, {502, 1724, 2919}, {0, 2.9, 7.1}},
    {155, 2, 87, -7.1, {735, 1954, 3166}, {0, 3.2, 7.2}},
    {285, 2, 87, -6.6, {987, 2198, 3417}, {0, 5.9, 9.2}},
    {410, 2, 87, -5.4, {714, 1934, 3145}, {0, 2.8, 6.9}},
    {525, 2, 87, -6.4, {488, 1710, 2903}, {0, 2.8, 7.5}},
    {530, 2, 103, -6.7, {488, 1710, 2903}, {0, 2.5, 7.2}},
    {535, 2, 492, -13.6, {489, 1710, 2902}, {0, 2.4, 7.2}},
    {540, 2, 83, -13.8, {489, 1710, 2902}, {0, 2.7, 7.2}},
    {545, 2, 492, -22.2, {491, 1712, 2903}, {0, 4.7, 9.4}},
    {550, 2, 492, -31.0, {491, 1710, 2901}, {0, 5.4, 9.9}},
    {555, 2, 492, -33.4, {491, 1707, 2899}, {0, 38.2, 43.8}},
};

/* where each effect's points start in fx_points, and how many there are,
 * for codes 200 to 254. effects without points are built in effect() */
static const struct {
  short first, count;
} fx_index[] = {
    {0, 16}, {16, 11}, {27, 15}, {42, 18}, {60, 15}, {75, 16}, {91, 18},
    {109, 19}, {128, 18}, {146, 16}, {162, 38}, {200, 38}, {238, 38},
    {276, 20}, {296, 33}, {329, 27}, {356, 18}, {374, 26}, {400, 33},
    {433, 32}, {465, 11}, {476, 14}, {490, 24}, {514, 15}, {529, 31},
    {560, 27}, {587, 18}, {605, 19}, {624, 12}, {636, 38}, {674, 40},
    {714, 38}, {752, 40}, {792, 25}, {817, 40}, {857, 38}, {895, 36},
    {931, 37}, {968, 29}, {997, 24}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1021, 33},
    {0, 0}, {1054, 16},
};

/* the effects the chip gates: their tones pulse on and off at a steady rate,
 * which the chip's envelope oscillator does. on the VoxRecord recording's
 * third take, measured from each effect's envelope a millisecond at a time:
 * C0 and C4 pulse at the default speech pitch (85 and 87Hz), C2 at 66Hz, C3
 * at 59Hz, C6 at 125Hz, C8 and C9 at 53Hz, on for a quarter to two fifths of
 * each cycle (B9 pulses at 87Hz too, but plays closer to the recording
 * ungated). the envelope shape for each is the one that brought it closest to
 * the recording. 0 is not gated. for codes 200 to 254 */
/* FX-GATE-BEGIN */
static const struct {
  short hz;
  envelope_wave wave;
} fx_gate[] = {
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {85, ENV_SQUARE}, {0, ENV_SAW}, {66, ENV_SINE}, {59, ENV_SAW},
    {87, ENV_SINE}, {0, ENV_SAW}, {125, ENV_SAW}, {0, ENV_SAW},
    {53, ENV_SQUARE}, {53, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW},
};
/* FX-GATE-END */

static frame fx_frame(const fx_point *p, int code) {
  const double g = pow(10, p->db / 20.0);
  frame f;
  memset(&f, 0, sizeof(f));
  if (p->kind == FX_NOISE) {
    f = noise_frame(lGlottal, p->freq[0] * 0.85, p->freq[0] * 1.15, 24 * g, 18 * g, 200);
  } else {
    const double base = p->freq[1] > 0 ? 24 : 28;
    for (int i = 0; i < 3; i++) {
      f.freq[i] = p->freq[i];
      if (p->freq[i] > 0)
        f.vol[i] = base * g * pow(10, p->rel[i] / 20.0);
    }
    if (p->kind == FX_BUZZ) {
      f.pitch = p->pitch;
      f.wave = ENV_VOICE;
      f.voiced = true;
    } else if (fx_gate[code - 200].hz > 0) {
      f.pitch = fx_gate[code - 200].hz;
      f.wave = fx_gate[code - 200].wave;
      f.voiced = true;
    }
  }
  if (p->db <= -60)
    return quiet(f);
  return f;
}

/* contour plays a traced effect: a quick start on the first point, then a
 * glide to each of the others in turn, then 3ms to silence */
static void contour(sound *s, const fx_point *p, int n, int code) {
  hold(s, p[0].ms, 2, fx_frame(&p[0], code));
  for (int i = 1; i < n; i++) {
    const double ms = p[i].ms - p[i - 1].ms;
    hold(s, ms, ms, fx_frame(&p[i], code));
  }
  hold(s, 3, 3, quiet(fx_frame(&p[n - 1], code)));
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
    contour(s, &fx_points[fx_index[code - 200].first], fx_index[code - 200].count,
            code);
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
 * 200, 700, 30, 60 and 90. the VoxProbe recording plays AW, a pause and AW
 * three times for each pause, and these lengths give the same silence between
 * the AWs as the chip, within about 5ms (the AWs measured the same way on
 * both): 21, 109, 212, 661, 26, 47 and 73ms. pause 0 is not nothing: the chip
 * still leaves a gap between the sounds */
static const double pause_ms[] = {14, 107, 203, 645, 25, 43, 70};

/* Delay is longer than its 10ms intervals: on the chip (the VoxProbe
 * recording) delays of 30, 60 and 90ms leave 44, 75 and 116ms between two AWs,
 * against 26ms for pause 4. it doesn't change with Speed (500ms is 500 to
 * 530ms at speeds 40, 114 and 127) */
#define DELAY_EXTRA_MS 12.0

/* how much Fast and Slow change a sound's length */
#define FAST_SCALE 0.65
#define SLOW_SCALE 1.35

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

  /* the timing in the copies after the first, when the sound is repeated */
  int copy_samples;
  int copy_glide;
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
  bool in_copy; /* playing a copy after the first */

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

/* Bend doesn't multiply the frequencies but adds to them. the manual only
 * says that bend goes from a "deep-hollow" to a "High-metallic" sounding voice.
 * measured from three takes of the VoxRecord recording (see
 * msa_speed_factor()): D1's two tones move together and keep their 512Hz
 * spacing, by -198, -154, 0, +220 and +439Hz at Bends 0, 1, 5, 10 and 15, and
 * A5 by the same at 0 and 1; AW's three formants all move by about 40Hz a
 * step (F3 from 2480 to 2912Hz at Bend 15, where a multiplier would put it
 * near 4400). that is 44Hz a step from 5 up, and from 1 down; between 1 and 5
 * it is 22Hz short, which these Bends can't place, so the shortfall is taken
 * up at 5. it moves oscillators 1 to 3, the formants and tones; SE's hiss
 * (oscillators 4 and 5) doesn't move at all */
static double msa_bend_hz(const msa *m) {
  if (m->bend >= SJ_DEFAULT_BEND)
    return 44.0 * (m->bend - SJ_DEFAULT_BEND);
  return 44.0 * m->bend - 198.0;
}

/* the envelope (voicing) frequency for a Pitch setting. the chip's voice is
 * slightly flat, more so the higher the pitch: three takes of the VoxRecord
 * recording give 87.1Hz for Pitch 88, 156.9 for 160, 195.2 for 200 and 242.6
 * for 250, which is each period one sample (at 8192Hz) longer than Pitch
 * asks for */
static double voice_hz(double pitch) {
  return pitch / (1 + pitch / SJ_SAMPLE_RATE);
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

/* noise that starts (oscillators 4 and 5 silent before, sounding in the
 * segment ahead) starts at its own frequencies rather than gliding up to
 * them: on the chip SE and SO start as hiss, where gliding made them sweep
 * up from the previous sound's frequencies */
static void msa_noise_onset(msa *m) {
  if (!msa_busy(m))
    return;
  const frame *t = &m->segs[m->seg].target;
  if (m->seg_start.vol[3] + m->seg_start.vol[4] == 0 &&
      t->vol[3] + t->vol[4] > 0) {
    m->seg_start.freq[3] = t->freq[3];
    m->seg_start.freq[4] = t->freq[4];
  }
}

/* start playing the segments just set up */
static void msa_start(msa *m) {
  m->seg = 0;
  m->seg_pos = 0;
  m->seg_start = m->current;
  if (m->nsegs > 0 && m->segs[0].jump)
    memcpy(m->seg_start.freq, m->segs[0].target.freq, sizeof(m->seg_start.freq));
  msa_noise_onset(m);
}

/* expand converts a sound into segments, applying the controls and
 * modifiers */
/* how far Stress raises F2 and F3 (see msa_expand()) */
#define STRESS_HZ 80.0

static void msa_expand(msa *m, const sound *s) {
  /* Speed stretches every sound, the effects too: on the chip (the VoxProbe
   * recording) A1, B1 and D1 take 2.6 to 2.7 times as long at speed 60 and
   * 0.56 to 0.59 times at speed 127, as the speech does */
  double scale = msa_speed_factor(m);
  double stress_hz = 0;

  /* the copies after the first, when the sound is repeated, take the
   * manual's length (scaled by Speed) rather than the chip's longer first
   * one: on the chip each extra AW adds 70ms, and repeating IY, UW and MM
   * six times takes one of them plus five times 70ms, to within 6ms */
  const double copy_scale = scale;

  if (s->speech) {
    scale *= s->length;

    /* fast and slow, measured from the VoxProbe recording: fast plays OWIY and
     * AXRR at 0.66 and 0.63 of their length, and slow at 1.42 and 1.31 times */
    if (m->fast)
      scale *= FAST_SCALE;
    if (m->slow)
      scale *= SLOW_SCALE;

    /* "STRESS, IH sounds more like (but not quite) the IY sound". on the
     * chip (the VoxRecord recording's third take) stress raises F2 and F3 by
     * 65 to 100Hz in AW, EYIY and NE and leaves the pitch and F1 alone.
     * relax changes nothing that can be measured, and so does nothing here */
    if (m->stress)
      stress_hz = STRESS_HZ;
  }

  const double bend_hz = msa_bend_hz(m);

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
      t.pitch = voice_hz((double)m->pitch);
      t.freq[1] += stress_hz;
      t.freq[2] += stress_hz;
    } else if (t.wave == ENV_VOICE) {
      /* a voiced effect follows the Pitch control: on the chip A1 and M2 are
       * voiced at 60.3, 87.9 and 149.5Hz at pitch 60, 88 and 150. their
       * traced pitch is the chip's at the default pitch */
      t.pitch *= voice_hz((double)m->pitch) / voice_hz(SJ_DEFAULT_PITCH);
    }
    for (int j = 0; j < 3; j++)
      if (t.freq[j] > 0)
        t.freq[j] = fmax2(t.freq[j] + bend_hz, 0);
    segment *sg = &m->segs[m->nsegs++];
    sg->samples = to_samples(ms);
    sg->glide = to_samples(glide);
    sg->target = t;
    sg->jump = false;

    const double copy_ms = p->ms * copy_scale;
    double copy_glide = p->glide_ms * copy_scale;
    if (p->glide_ms == AUTO_GLIDE)
      copy_glide = fmin2(fmax2(copy_ms * 0.35, 20), 50);
    sg->copy_samples = to_samples(copy_ms);
    sg->copy_glide = to_samples(fmin2(copy_glide, copy_ms));
  }

  m->fast = m->slow = m->stress = m->relax = false;

  if (m->nsegs > 0 && m->jump_next_freq) {
    m->segs[0].jump = true;
    m->jump_next_freq = false;
  }

  m->copies = m->repeat > 1 ? m->repeat - 1 : 0;
  m->in_copy = false;
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
  m->in_copy = false;
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
    return synth_sample(&m->synth, &m->current, (double)m->volume);

  const segment *sg = &m->segs[m->seg];
  const int glide = m->in_copy ? sg->copy_glide : sg->glide;
  const int samples = m->in_copy ? sg->copy_samples : sg->samples;
  if (glide > 0 && m->seg_pos < glide)
    m->current = lerp(&m->seg_start, &sg->target,
                      (double)(m->seg_pos + 1) / (double)glide);
  else
    m->current = sg->target;

  const double v = synth_sample(&m->synth, &m->current, (double)m->volume);

  m->seg_pos++;
  if (m->seg_pos >= samples) {
    m->seg++;
    if (m->seg >= m->nsegs && m->copies > 0) {
      m->copies--;
      m->in_copy = true;
      m->seg = 0;
    }
    m->seg_pos = 0;
    m->seg_start = m->current;
    if (msa_busy(m) && m->segs[m->seg].jump)
      memcpy(m->seg_start.freq, m->segs[m->seg].target.freq,
             sizeof(m->seg_start.freq));
    msa_noise_onset(m);
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
      msa_silence(m, (double)data * 10 + DELAY_EXTRA_MS, NULL);
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
