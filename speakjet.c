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
static int imax(int a, int b) { return a > b ? a : b; }

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

/* the chip's oscillators run on a grid of frequencies FREQ_STEP_HZ apart:
 * on the second VoxProbe recording all seven DTMF frequencies are whole
 * multiples of it (697.17Hz is 159 steps, 1468.75Hz 335, each within 0.12Hz),
 * a step of Bend is 10 of them and the short step between Bends 4 and 5 is 5.
 * that is the step of a phase accumulator. on_grid() puts a frequency on it */
#define FREQ_STEP_HZ 4.3845
static double on_grid(double hz) { return floor(hz / FREQ_STEP_HZ + 0.5) * FREQ_STEP_HZ; }

/* the chip's voicing fade: the level of the formant oscillators after each
 * voicing pulse, every VOICE_FADE_STEP_MS, straight lines in between, nothing
 * after the last (see ENV_VOICE in synth_sample()). measured from single
 * pulses at pitch 40 on two units: full level, a dip to 0.7 within half a
 * millisecond, back to 0.8, held to 2ms, then a near straight fall to nothing
 * by 7.5ms. Volume, Bend and Pitch don't change it */
#define VOICE_FADE_STEP_MS 0.25
static const double voice_fade[] = {
    1.000, 0.807, 0.701, 0.892, 0.787, 0.800, 0.756, 0.734,
    0.753, 0.687, 0.660, 0.612, 0.598, 0.547, 0.513, 0.469,
    0.426, 0.386, 0.331, 0.288, 0.252, 0.246, 0.193, 0.134,
    0.150, 0.085, 0.096, 0.054, 0.040, 0.043, 0.003, 0.008,
    0.012, 0.000,
};
#define VOICE_FADE_N ((int)(sizeof(voice_fade) / sizeof(voice_fade[0])))

/* how long the chip takes to bring voicing up from silence, in a straight
 * line: vowels, liquids and diphthongs ONSET_MS, nasals NASAL_ONSET_MS. a
 * time, not a number of periods (the same at every pitch). voiced stops and
 * fricatives have none */
#define ONSET_MS 40.0
#define NASAL_ONSET_MS 25.0

/* the onset each allophone has (see ONSET_MS): the vowels, liquids and
 * diphthongs ONSET_MS, the nasals NASAL_ONSET_MS, the rest none */
static double onset_ms(int code) {
  if (code > 164)
    return 0;
  if (code >= 140 && code <= 144)
    return NASAL_ONSET_MS;
  return ONSET_MS;
}

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

  /* the phase (in cycles) oscillators 2 and 3 restart at with each voicing
   * pulse, against oscillator 1 (see formant_phases) */
  double phase0[3];

  /* how long the voicing takes to fade in when it starts from silence (see
   * ONSET_MS). 0 is no fade */
  double onset_ms;

  /* how deeply the envelope gates oscillators 4 and 5 (0 to 1), when they
   * aren't half enveloped (see NOISE_GATE) */
  double noise_depth;

  /* how far Bend moves oscillators 4 and 5 of a sound effect, in hertz,
   * added after their distortion spread (see msa_expand()) */
  double noise_shift;

  /* how long each random value of the distortion is held, in ms; 0 for a new
   * one every sample (see c_noise()) */
  double noise_hold_ms;

  /* a gated effect that pulses as the voicing does (see fx_pulse): its rate
   * is pulse_offset hertz from the voicing, and its pulses fade along
   * voice_fade stretched fade_scale times (0 is 1) */
  bool pulsed;
  double pulse_offset;
  double fade_scale;

  /* oscillators 1 to 3 that the voicing doesn't envelope: not restarted by
   * its pulses nor faded after them (bit 0 is oscillator 1). the manual has
   * the envelope control choose which oscillators it envelopes (see c1()) */
  int free_osc;

  /* the shaping of the consonants (see sound_eq): the gain in dB of each
   * of the EQ_BANDS peaking filters on the output, broadband noise riding on
   * oscillators 4 and 5 (as a fraction of their volume), and an overall gain
   * in dB */
  double eq[10];
  double wide;
  double eq_gain;
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
  for (int i = 0; i < 10; i++)
    r.eq[i] = a->eq[i] + (b->eq[i] - a->eq[i]) * t;
  r.wide = a->wide + (b->wide - a->wide) * t;
  r.eq_gain = a->eq_gain + (b->eq_gain - a->eq_gain) * t;
  return r;
}

/* synth is the 5-channel synthesizer */
/* the chip's output. it makes its samples at CHIP_RATE (measured from the
 * mirror images of steady tones; the units' clocks differ by 0.3%), holds each
 * until the next, and an analogue low pass shapes the result, images and all.
 * here the samples are held the same way, each output sample is the average of
 * what is held over its time, and output_sos is that low pass (measured from
 * the voicing pulses below 4kHz and from the images above: -5.4dB at 2kHz, -14
 * at 3.5kHz, -18 at 5.5kHz, -38 at 23.5kHz), whatever the AtariVox and
 * recorder add included */
#define CHIP_RATE 8028.0

#define OUTPUT_SECTIONS 3
static const double output_sos[OUTPUT_SECTIONS][6] = {
    {0.0636279260299, 0, 0, 1, 0.0618381585253, 0},
    {1, -0.0974446347576, 0, 1, -1.63378468591, 0.680472611685},
    {1, 0.985616684932, 0, 1, 1.0014122889, 0.0588508646104}
};

typedef struct synth {
  /* phase of oscillators 1 to 5 and of the envelope oscillator, in cycles */
  double phase[5];
  double env_phase;

  /* samples since the last voicing pulse */
  long env_age;

  /* samples since oscillators 1 to 3 last came up from silence */
  long onset_age;

  /* noise source for distortion (xorshift) */
  uint32_t noise;

  /* the held random values of oscillators 4 and 5, and the samples left
   * until the next (see noise_hold_ms) */
  double held[2];
  long hold_left;

  /* the consonants' peaking filters (see sound_eq): the gains their
   * coefficients were made for, the coefficients (b0, b1, b2, a1, a2) and
   * the last two inputs and outputs of each */
  double eq_db[10];
  double eq_co[10][5];
  double eq_st[10][4];
  bool eq_ready;

  /* the noise source of the consonants' broadband noise */
  uint32_t wide_noise;

  /* the level the noise floor follows, and its own noise source (see
   * SKIRT_DB) */
  double skirt_level;
  uint32_t skirt_noise;

  /* dc blocker state */
  double dc_in, dc_out;
} synth;


static void synth_init(synth *s) {
  memset(s, 0, sizeof(*s));
  s->noise = 0x2545f491u;
  s->skirt_noise = 0x9e3779b9u;
  s->wide_noise = 0x85ebca6bu;
  s->env_age = 1L << 30;
  s->onset_age = 0;
}

static double synth_random(synth *s) {
  s->noise ^= s->noise << 13;
  s->noise ^= s->noise >> 17;
  s->noise ^= s->noise << 5;
  return (double)(int32_t)s->noise / SJ_MAX_INT32;
}

/* the chip's output has a floor of noise under whatever it plays, 40 to 55dB
 * under a pure tone. here white noise follows the output's level (over about
 * 25ms) SKIRT_DB under it */
#define SKIRT_DB (-24.0)
#define SKIRT_FOLLOW 0.995

/* the sounds' shaping (see sound_eq): EQ_BANDS peaking filters half an octave
 * apart, then an overall gain. a filter is made again whenever its gain
 * changes at all, not when it has moved some step, so that compilers that do
 * and don't fuse a multiply and an add give the same samples */
#define EQ_BANDS 10
#define EQ_Q 1.0
static const double eq_hz[EQ_BANDS] = {180, 250, 355, 500, 710, 1000, 1410, 2000, 2830, 3600};

static double synth_eq(synth *s, const frame *f, double x) {
  for (int k = 0; k < EQ_BANDS; k++) {
    double *c = s->eq_co[k];
    if (!s->eq_ready || f->eq[k] != s->eq_db[k]) {
      s->eq_db[k] = f->eq[k];
      const double a = pow(10, s->eq_db[k] / 40);
      const double w = 2 * SJ_PI * eq_hz[k] / CHIP_RATE;
      const double al = sin(w) / (2 * EQ_Q);
      const double a0 = 1 + al / a;
      c[0] = (1 + al * a) / a0;
      c[1] = -2 * cos(w) / a0;
      c[2] = (1 - al * a) / a0;
      c[3] = -2 * cos(w) / a0;
      c[4] = (1 - al / a) / a0;
    }
    double *z = s->eq_st[k];
    const double y = c[0] * x + c[1] * z[0] + c[2] * z[1] - c[3] * z[2] - c[4] * z[3];
    z[1] = z[0];
    z[0] = x;
    z[3] = z[2];
    z[2] = y;
    x = y;
  }
  s->eq_ready = true;
  return x * pow(10, f->eq_gain / 20);
}

/* sample produces the next output sample, in the range -1 to 1. master is
 * the master volume (0 to 127) */
static double synth_sample(synth *s, const frame *f, double master) {
  /* envelope oscillator: a voicing pulse at the end of each period */
  double env = 1.0;
  bool pulse = false;
  if (f->pitch > 0) {
    s->env_phase += f->pitch / CHIP_RATE;
    if (s->env_phase >= 1) {
      s->env_phase -= floor(s->env_phase);
      pulse = true;
    }
  }
  if (pulse) {
    s->env_age = 0;
    /* restart the formant oscillators with each pulse */
    if (f->voiced)
      for (int i = 0; i < 3; i++)
        if (!(f->free_osc >> i & 1))
          s->phase[i] = f->phase0[i];
  }
  if (f->wave == ENV_VOICE) {
    /* the chip's voicing: each pulse restarts the formant oscillators at full
     * level and they fade to nothing along voice_fade in under 10ms, whatever
     * the pitch or volume. at low pitch the rest of the period is silent; at
     * high pitch the next pulse cuts the fade short. a pulsed effect's fade
     * can be longer (see fx_pulse) */
    const double k = (double)s->env_age * 1000 / CHIP_RATE / VOICE_FADE_STEP_MS /
                     (f->fade_scale > 0 ? f->fade_scale : 1);
    const int i = (int)k;
    env = i >= VOICE_FADE_N - 1 ? 0 : voice_fade[i] + (voice_fade[i + 1] - voice_fade[i]) * (k - i);
  } else if (f->pitch > 0) {
    const double p = s->env_phase;
    switch (f->wave) {
    case ENV_SAW:
      env = 1 - p;
      break;
    case ENV_VOICE:
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
    /* a pitch of zero gives no periodic envelope */
    env = 0;
  }
  if (s->env_age < (1L << 30))
    s->env_age++;

  /* mixer 1: oscillators 1 to 3 */
  double mix1 = 0, free1 = 0, total = 0;
  for (int i = 0; i < 3; i++) {
    const double fr = fmin2(on_grid(f->freq[i]), MAX_FREQUENCY);
    s->phase[i] += fr / CHIP_RATE;
    s->phase[i] -= floor(s->phase[i]);
    const double v = fmin2(f->vol[i], MAX_OSC_VOLUME);
    total += v;
    if (f->free_osc >> i & 1)
      free1 += v * sin(2 * SJ_PI * s->phase[i]);
    else
      mix1 += v * sin(2 * SJ_PI * s->phase[i]);
  }
  if (f->voiced)
    mix1 *= env;
  mix1 += free1;
  if (total > MAX_MIXER1)
    mix1 *= MAX_MIXER1 / total;
  /* the voicing coming up from silence (see ONSET_MS) */
  if (total == 0) {
    s->onset_age = 0;
  } else if (f->onset_ms > 0 && s->onset_age < (long)(f->onset_ms * CHIP_RATE / 1000)) {
    mix1 *= (double)s->onset_age / (f->onset_ms * CHIP_RATE / 1000);
    s->onset_age++;
  }

  /* mixer 2: oscillators 4 and 5, with distortion spreading noise around
   * the oscillator frequency, a new random value every sample or held for
   * noise_hold_ms, and then moved by noise_shift */
  double mix2 = 0;
  const double spread = f->distortion / 255 * 1.5;
  if (f->noise_hold_ms > 0 && --s->hold_left <= 0) {
    s->held[0] = synth_random(s);
    s->held[1] = synth_random(s);
    s->hold_left = (long)(f->noise_hold_ms * CHIP_RATE / 1000 + 0.5);
  }
  for (int i = 3; i < 5; i++) {
    const double fr = fmin2(on_grid(f->freq[i]), MAX_FREQUENCY);
    const double r = f->noise_hold_ms > 0 ? s->held[i - 3] : synth_random(s);
    double hz = fr * (1 + spread * r) + f->noise_shift;
    if (spread == 0)
      hz = fmax2(fmin2(hz, MAX_FREQUENCY), 0);
    s->phase[i] += hz / CHIP_RATE;
    s->phase[i] -= floor(s->phase[i]);
    mix2 += fmin2(f->vol[i], MAX_OSC_VOLUME) * sin(2 * SJ_PI * s->phase[i]);
  }
  /* the consonants' broadband noise (see sound_eq) */
  if (f->wide > 0) {
    s->wide_noise ^= s->wide_noise << 13;
    s->wide_noise ^= s->wide_noise >> 17;
    s->wide_noise ^= s->wide_noise << 5;
    mix2 += f->wide * (fmin2(f->vol[3], MAX_OSC_VOLUME) + fmin2(f->vol[4], MAX_OSC_VOLUME)) *
            (double)(int32_t)s->wide_noise / SJ_MAX_INT32;
  }
  if (f->half_env)
    mix2 *= 0.5 + 0.5 * env;
  else if (f->noise_depth > 0)
    mix2 *= 1 - f->noise_depth * (1 - env);

  /* mixer 5: summation and master volume */
  double x = (mix1 + mix2) / (MAX_MIXER1 + 2 * MAX_OSC_VOLUME) *
             fmin2(master, MAX_MASTER) / MAX_MASTER;

  x = synth_eq(s, f, x);

  /* the noise floor (see SKIRT_DB) */
  s->skirt_level = SKIRT_FOLLOW * s->skirt_level + (1 - SKIRT_FOLLOW) * fabs(x);
  s->skirt_noise ^= s->skirt_noise << 13;
  s->skirt_noise ^= s->skirt_noise >> 17;
  s->skirt_noise ^= s->skirt_noise << 5;
  x += pow(10, SKIRT_DB / 20) * s->skirt_level * (double)(int32_t)s->skirt_noise / SJ_MAX_INT32;

  /* dc blocker */
  const double y = x - s->dc_in + 0.995 * s->dc_out;
  s->dc_in = x;
  s->dc_out = y;

  return y;
}

/* ======================================================================== */
/* tables.go                                                                */
/* ======================================================================== */

/* the most parts any sound has (a traced effect: see fx_knots) */
#define MAX_PARTS 256

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

  /* the sound doesn't follow the Bend control (see msa_expand()) */
  bool no_bend;
  /* the copies' length when repeated, against the manual's (see copy_length) */
  double copy_length;

  /* Slow doesn't lengthen the sound (see msa_expand()) */
  bool no_slow;

  /* a voiced effect whose pitch is its own, not moved by the Pitch control
   * (see c1()) */
  bool own_pitch;

  /* the sound is played from the chip's own frames (see vf_points) */
  bool framed;

  /* the sound's gate keeps time with the Speed control (see c_noise()) */
  bool speed_gate;

  /* the sound is a stop, its first part the closure (see plosive() and
   * msa_closure()) */
  bool stop;
} sound;

/* the lengths the stops and affricates are built with, against the manual's:
 * steady sounds about 1.5 times, diphthongs 1.2 times, stops as given (from a
 * recording of a real SpeakJet). the other allophones have their own (see
 * allophone_length) */
#define STEADY_LENGTH 1.5
#define DIPHTHONG_LENGTH 1.2
#define STOP_LENGTH 1.0

/* formant targets in Hz. the vowels, nasals and liquids are the chip's,
 * measured from single voicing pulses on two units with the chip's fade and
 * output filter (see voice_fade): their F1 and F2 are whole multiples of about
 * 88.4Hz less 3.5Hz. the rest are published values for an adult male speaker,
 * or the mean of two independent measurements of the chip where they agree */
typedef struct formants {
  double f[3];
} formants;

#define FM(a, b, c) ((formants){{(a), (b), (c)}})

static const formants fIY = {{256, 2208, 2551}};
static const formants fIH = {{350, 1762, 2479}};
static const formants fEY = {{531, 1764, 2383}};
static const formants fEH = {{617, 1676, 2384}};
static const formants fAY = {{703, 1679, 2216}};
static const formants fAX = {{530, 1503, 2383}};
static const formants fUX = {{617, 1324, 2379}};
static const formants fOH = {{704, 1147, 2379}};
static const formants fAW = {{706, 970, 2464}};
static const formants fOW = {{526, 881, 2353}};
static const formants fUH = {{435, 1055, 2275}};
static const formants fUW = {{349, 1055, 2094}};
static const formants fAH = {{700, 1220, 2600}};
static const formants fER = {{480, 1350, 1650}};
static const formants fRR = {{442, 1325, 1680}};
static const formants fLE = {{438, 968, 2635}};
static const formants fLO = {{400, 900, 2500}};
static const formants fWW = {{438, 790, 2058}};

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

/* the chip doesn't restart all three formant sines at the same phase: each
 * sound's second and third formants start at their own phase against the first
 * (as fractions of a cycle). only the sounds that match the chip better with
 * them at speaking pitch have them */
static frame formant_phases(frame f, double p2, double p3) {
  f.phase0[1] = p2;
  f.phase0[2] = p3;
  return f;
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

/* how much quieter the chip's noise is than levels first chosen here: hiss
 * sits 16 to 23dB under the vowels beside it. the fricatives have their levels
 * in their frames (see frSE()); these are for the bursts and aspiration */
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
  s->stop = true;
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

/* the noise frames of the voiceless fricatives, fitted as whole sounds to both
 * units: the chip's hiss is narrow bands (SE and SO at 3 to 4kHz, SH at 2.5 to
 * 3.2kHz, HE and HO around 1 to 1.5kHz, WH at 0.6 to 0.9kHz). their levels are
 * set by allophone_gain_db */
static frame frSE(void) { return noise_frame(lAlveolar, 3629.7, 2609.1, 25.87, 5.78, 33.4); }
static frame frSO(void) { return noise_frame(lAlveolarBak, 2953.5, 3580, 17.5, 30.41, 27.3); }
static frame frSH(void) { return noise_frame(lPalatal, 3605.7, 2502.8, 17.72, 22.59, 44.4); }
static frame frZH(void) {
  return hush(voiced_noise_frame(FM(260, 1800, 2500), 14, 2400, 3200, 14, 10, 200),
              FRICATIVE_DB);
}

/* stop descriptions: duration, closure, burst, voiced, locus, burst
 * frequencies, burst volumes, aspiration frequencies (STOPN() also the noise
 * widths, aspiration volumes and voicing). fitted as whole sounds to both
 * units, each keeping the length it has (see allophone_length): a closure that
 * is silent can't be measured from a sound alone */
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

/* LENGTH-TABLE-BEGIN */
/* how much longer the chip plays each allophone than the manual's length (what
 * its parts add up to), at the default speed, measured and fitted on both
 * units, alone and in connected groups. the framed sounds (see vf_points) are
 * the measured length already, so theirs is near 1 */
static const double allophone_length[] = {
    1.4, 1, 1.96, 1.47, 2.0825, 1.96, 1.4, 1.7325, 1.4,
    1.68, 1.68, 1.4, 1.4, 1.33, 1.0375, 1.2425, 1.3475, 1.3825,
    1.0625, 1.54, 1.5925, 0.9375, 1.177734, 1, 1.025, 1.0492, 1.169697,
    1.254, 0.875, 0.921081, 1.05, 1.0625, 1, 1.05, 1, 1.0125,
    1.025, 0.890315, 0.8295, 0.8906, 1.175, 1, 1.63, 0.975, 2.87025,
    3.27, 1.3475, 1, 0.8375, 0.8875, 1.0875, 1, 1, 0.8328,
    2.800463, 1.17, 1.12125, 1.11, 1.49, 2.45, 1.862719, 1.17, 1.28325,
    2.07, 2.67575, 0.922059, 2.5315, 1.085, 2.49, 2.706, 1.4175, 1.37984,
};
/* LENGTH-TABLE-END */

/* REFIT-BEGIN */
/* final_length multiplies an allophone's length when it is the last before a
 * pause or the end and isn't repeated: on the chip a vowel played last is
 * longer than inside a phrase (IY and AW 1.25 times) and TT and FF shorter
 * (0.64, 0.79) */
static const double final_length[] = {
    1.25, 1.175, 1, 1.175, 1, 1, 1, 1, 1.25,
    1, 1, 1.15, 1, 1, 0.9, 1.025, 0.9375, 1,
    1, 1.05, 0.9875, 1.1375, 1, 1, 1, 0.9125, 1,
    1, 1.1931, 1.2375, 0.925, 0.975, 1.0375, 1, 1.025, 1.0625,
    1, 1, 1.2656, 1, 1, 1, 1, 1, 1,
    1, 0.975, 0.8875, 1.2625, 1.2, 1, 1.075, 1.2125, 1,
    1, 1, 1, 1, 0.7875, 0.9625, 1, 1, 1,
    0.6375, 1.05, 1, 1, 1.3, 1, 0.8, 1, 0.925,
};
/* the copies after the first, when a sound is repeated, as a multiple of the
 * manual's length (see msa_expand()): on the chip many are shorter than the
 * manual says (DO 0.7 times, ZZ 0.69, EB 0.51) */
static const double copy_length[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 0.9875, 0.925, 1,
    0.75, 1, 1, 1.0625, 1, 0.9625, 1, 0.9125, 1,
    0.8625, 1.075, 1, 0.8875, 0.9625, 1.0875, 0.9125, 1.05, 0.925,
    0.9375, 0.9375, 1, 0.6875, 0.975, 0.7625, 0.7625, 0.825, 0.5125,
    0.8, 1, 0.7, 1, 1.1, 0.925, 0.9875, 1, 0.8125,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
};
/* REFIT-END */

/* how much louder (in dB) the chip plays each allophone than its oscillator
 * volumes give: against AW on its own, and refitted where sounds sit next to
 * each other */
static const double allophone_gain_db[] = {
    3.54, 0, 2, 1.2, 2, 1.7, 1.3, 3.99, 0,
    -1.8, -1.9, -1.3, -5.96, -10.97, 0, -4.5, -4.7, -4.2,
    0, -4.2, 0.4, 0, 0.8, 0, 0, 0, 1.2,
    0.5, 0, 0.9, 0, 0, 0, 0, 0, 0,
    0, -11.3, -2.1, 4.31, 0, 0, 5.9, 0, -2.6,
    2.7, -0.1, 0, 0, 0, 0, 0, 0, 0,
    -15.5, -26.49, -31.1, -29.8, -26.3, -15.9, -17.6, -16.6, -26.8,
    -2.1, -3.3, -26.4, 1.5, -3, -2.38, -3.8, 6.4, 5.9,
};

/* VF-BEGIN */
/* voiced sounds played as the chip plays them, frame by frame: each point is a
 * voicing pulse fitted as three formant sines with the chip's fade and output
 * filter (see voice_fade), the two units averaged: its time after the one
 * before, the glide to it, the three frequencies and their volumes (AW's first
 * formant is 24). for the diphthongs (which move in steps), the voiced
 * fricatives (mostly voicing) and voiced stops such as BO and GE (a voice bar,
 * then a short vowel-like release) */
typedef struct vf_point {
  float ms, glide;
  float freq[3];
  float vol[3];
} vf_point;

static const vf_point vf_points[] = {
    {1.00, 1.00, {352, 1750, 2524}, {10.64, 7.00, 4.52}}, /* IH */
    {79.95, 79.95, {346, 1770, 2471}, {22.77, 17.49, 10.08}}, /* IH */
    {24.25, 24.25, {348, 1751, 2497}, {5.84, 5.68, 2.93}}, /* IH */
    {11.50, 11.50, {348, 1751, 2497}, {5.84, 5.68, 2.93}}, /* IH */
    {1.00, 1.00, {251, 1149, 2596}, {0.81, 0.23, 0.25}}, /* NO */
    {12.39, 12.39, {254, 1116, 2604}, {7.53, 1.69, 3.23}}, /* NO */
    {57.19, 57.19, {253, 1154, 2599}, {9.20, 2.04, 4.05}}, /* NO */
    {11.52, 11.52, {260, 1157, 2555}, {6.99, 1.31, 2.92}}, /* NO */
    {11.50, 11.50, {260, 1157, 2555}, {6.99, 1.31, 2.92}}, /* NO */
    {1.00, 1.00, {442, 875, 2394}, {4.79, 5.02, 0.42}}, /* LO */
    {58.31, 58.31, {447, 876, 2394}, {9.30, 10.39, 1.70}}, /* LO */
    {22.85, 22.85, {443, 869, 2450}, {5.54, 6.05, 0.56}}, /* LO */
    {11.50, 11.50, {443, 869, 2450}, {5.54, 6.05, 0.56}}, /* LO */
    {1.00, 1.00, {358, 1336, 2487}, {3.97, 2.40, 2.83}}, /* IYRR */
    {12.15, 12.15, {234, 2208, 2552}, {12.00, 8.04, 6.25}}, /* IYRR */
    {148.59, 148.59, {396, 1510, 2441}, {16.18, 12.84, 1.04}}, /* IYRR */
    {67.89, 67.89, {429, 1322, 2441}, {10.24, 9.55, 1.29}}, /* IYRR */
    {1.00, 1.00, {532, 1502, 2368}, {12.05, 7.97, 5.86}}, /* AXRR */
    {58.04, 58.04, {507, 1468, 2426}, {25.00, 16.61, 6.10}}, /* AXRR */
    {68.86, 68.86, {476, 1408, 1933}, {21.41, 15.09, 6.00}}, /* AXRR */
    {11.46, 11.46, {468, 1384, 1943}, {21.36, 14.98, 5.50}}, /* AXRR */
    {11.50, 11.50, {463, 1377, 2347}, {19.81, 15.16, 2.10}}, /* AXRR */
    {45.23, 45.23, {435, 1340, 2347}, {19.49, 16.07, 0.90}}, /* AXRR */
    {35.04, 35.04, {436, 1324, 2347}, {7.40, 6.72, 0.73}}, /* AXRR */
    {1.00, 1.00, {726, 930, 2530}, {7.32, 6.79, 2.55}}, /* AWRR */
    {23.76, 23.76, {694, 981, 2460}, {22.75, 21.26, 6.22}}, /* AWRR */
    {115.65, 115.65, {547, 1195, 1971}, {17.72, 15.93, 4.21}}, /* AWRR */
    {11.41, 11.41, {529, 1233, 2229}, {18.15, 15.80, 4.08}}, /* AWRR */
    {80.02, 80.02, {446, 1318, 2397}, {9.51, 9.62, 0.97}}, /* AWRR */
    {1.00, 1.00, {519, 910, 2400}, {1.06, 0.89, 0.85}}, /* OWRR */
    {12.89, 12.89, {562, 865, 2400}, {6.87, 7.72, 0.86}}, /* OWRR */
    {9.68, 9.68, {529, 883, 2427}, {16.52, 20.16, 4.58}}, /* OWRR */
    {36.45, 36.45, {524, 950, 2400}, {11.75, 13.56, 7.44}}, /* OWRR */
    {10.74, 10.74, {520, 993, 2313}, {14.28, 16.08, 2.91}}, /* OWRR */
    {11.34, 11.34, {517, 1024, 2400}, {14.71, 16.57, 1.08}}, /* OWRR */
    {22.87, 22.87, {497, 1083, 2555}, {14.91, 15.93, 3.20}}, /* OWRR */
    {34.23, 34.23, {479, 1170, 2583}, {17.39, 16.17, 2.47}}, /* OWRR */
    {22.81, 22.81, {464, 1236, 2379}, {17.32, 15.97, 2.06}}, /* OWRR */
    {22.74, 22.74, {451, 1305, 2583}, {17.41, 15.90, 1.92}}, /* OWRR */
    {12.62, 12.62, {450, 1322, 2482}, {12.43, 11.20, 4.01}}, /* OWRR */
    {9.59, 9.59, {449, 1316, 2400}, {11.05, 9.98, 1.70}}, /* OWRR */
    {13.29, 13.29, {450, 910, 2400}, {5.79, 5.13, 0.38}}, /* OWRR */
    {11.50, 11.50, {450, 910, 2400}, {5.79, 5.13, 0.38}}, /* OWRR */
    {1.00, 1.00, {367, 1006, 2461}, {0.43, 0.48, 0.48}}, /* OWIY */
    {13.15, 13.15, {551, 871, 2461}, {5.79, 6.16, 1.14}}, /* OWIY */
    {12.23, 12.23, {552, 904, 2543}, {10.17, 12.25, 4.25}}, /* OWIY */
    {11.38, 11.38, {534, 917, 2503}, {11.71, 13.00, 7.05}}, /* OWIY */
    {33.37, 33.37, {478, 1156, 2393}, {15.07, 15.69, 5.67}}, /* OWIY */
    {11.38, 11.38, {457, 1231, 2424}, {16.43, 15.58, 3.73}}, /* OWIY */
    {10.81, 10.81, {436, 1308, 2429}, {14.99, 13.52, 7.32}}, /* OWIY */
    {46.62, 46.62, {380, 1597, 2595}, {16.72, 12.66, 4.93}}, /* OWIY */
    {34.80, 34.80, {336, 1809, 2522}, {16.10, 13.08, 7.15}}, /* OWIY */
    {45.82, 45.82, {243, 2092, 2561}, {16.16, 8.68, 8.00}}, /* OWIY */
    {22.91, 22.91, {232, 2231, 2561}, {14.13, 8.52, 7.00}}, /* OWIY */
    {12.68, 12.68, {249, 2232, 2545}, {7.17, 5.18, 3.47}}, /* OWIY */
    {11.50, 11.50, {249, 2232, 2545}, {7.17, 5.18, 3.47}}, /* OWIY */
    {1.00, 1.00, {595, 1732, 2592}, {1.13, 0.41, 0.39}}, /* IYEH */
    {13.69, 13.69, {595, 2137, 2553}, {5.36, 3.89, 3.71}}, /* IYEH */
    {10.56, 10.56, {263, 2190, 2496}, {13.73, 10.61, 7.87}}, /* IYEH */
    {12.67, 12.67, {255, 2169, 2526}, {16.77, 10.04, 8.61}}, /* IYEH */
    {11.47, 11.47, {262, 2146, 2592}, {17.51, 10.52, 8.76}}, /* IYEH */
    {22.50, 22.50, {347, 2055, 2518}, {19.26, 10.71, 10.32}}, /* IYEH */
    {68.29, 68.29, {524, 1793, 2512}, {22.79, 12.85, 7.46}}, /* IYEH */
    {45.30, 45.30, {613, 1687, 2356}, {25.88, 15.32, 10.01}}, /* IYEH */
    {25.19, 25.19, {610, 1692, 2342}, {9.04, 5.22, 3.41}}, /* IYEH */
    {11.50, 11.50, {610, 1692, 2342}, {9.04, 5.22, 3.41}}, /* IYEH */
    {1.00, 1.00, {617, 1681, 2416}, {0.57, 0.38, 0.19}}, /* EHLL */
    {10.47, 10.47, {617, 1681, 2416}, {4.82, 3.19, 2.38}}, /* EHLL */
    {24.20, 24.20, {617, 1681, 2353}, {25.21, 14.61, 10.75}}, /* EHLL */
    {91.72, 91.72, {498, 1158, 2630}, {14.83, 11.29, 3.62}}, /* EHLL */
    {34.82, 34.82, {457, 971, 2416}, {9.68, 10.34, 1.19}}, /* EHLL */
    {11.47, 11.47, {617, 965, 2416}, {5.21, 6.13, 0.49}}, /* EHLL */
    {11.50, 11.50, {617, 965, 2416}, {5.21, 6.13, 0.49}}, /* EHLL */
    {1.00, 1.00, {214, 1118, 2504}, {4.23, 2.83, 2.45}}, /* IYUW */
    {12.44, 12.44, {208, 2176, 2548}, {14.05, 7.71, 5.76}}, /* IYUW */
    {11.08, 11.08, {214, 2208, 2507}, {18.49, 9.82, 8.01}}, /* IYUW */
    {22.98, 22.98, {260, 2045, 2552}, {14.67, 8.20, 8.25}}, /* IYUW */
    {22.51, 22.51, {265, 1837, 2504}, {15.25, 11.07, 6.06}}, /* IYUW */
    {10.23, 10.23, {305, 1767, 2368}, {15.78, 14.49, 7.55}}, /* IYUW */
    {24.55, 24.55, {289, 1598, 2419}, {13.28, 9.14, 3.37}}, /* IYUW */
    {55.79, 55.79, {333, 1205, 2198}, {13.66, 12.58, 3.89}}, /* IYUW */
    {23.00, 23.00, {336, 1064, 2342}, {14.98, 14.53, 3.10}}, /* IYUW */
    {12.12, 12.12, {326, 1062, 2552}, {8.86, 8.49, 0.65}}, /* IYUW */
    {11.50, 11.50, {326, 1062, 2552}, {8.86, 8.49, 0.65}}, /* IYUW */
    {1.00, 1.00, {675, 1207, 2378}, {2.26, 2.07, 1.53}}, /* AXUW */
    {10.99, 10.99, {531, 1504, 2358}, {13.51, 8.71, 6.50}}, /* AXUW */
    {11.64, 11.64, {530, 1500, 2376}, {24.66, 15.78, 11.67}}, /* AXUW */
    {115.41, 115.41, {683, 1137, 2432}, {23.12, 17.14, 8.71}}, /* AXUW */
    {22.46, 22.46, {515, 1090, 2341}, {18.61, 15.28, 3.74}}, /* AXUW */
    {11.56, 11.56, {407, 1080, 2511}, {16.19, 13.59, 3.08}}, /* AXUW */
    {11.54, 11.54, {343, 1061, 2378}, {14.28, 13.74, 1.20}}, /* AXUW */
    {11.58, 11.58, {317, 1059, 2378}, {8.03, 7.35, 0.48}}, /* AXUW */
    {11.50, 11.50, {317, 1059, 2378}, {8.03, 7.35, 0.48}}, /* AXUW */
    {1.00, 1.00, {351, 1664, 2435}, {4.35, 2.15, 1.70}}, /* IHWW */
    {34.95, 34.95, {351, 1713, 2435}, {22.44, 17.04, 9.58}}, /* IHWW */
    {69.75, 69.75, {381, 1354, 2439}, {15.07, 12.86, 2.93}}, /* IHWW */
    {57.00, 57.00, {398, 1064, 2228}, {13.16, 13.37, 3.76}}, /* IHWW */
    {11.47, 11.47, {408, 1013, 2435}, {12.30, 13.21, 1.68}}, /* IHWW */
    {46.93, 46.93, {430, 811, 2435}, {6.70, 9.13, 2.27}}, /* IHWW */
    {12.16, 12.16, {351, 802, 2435}, {3.25, 4.83, 1.14}}, /* IHWW */
    {11.50, 11.50, {351, 802, 2435}, {3.25, 4.83, 1.14}}, /* IHWW */
    {1.00, 1.00, {679, 1567, 2175}, {1.02, 0.71, 0.70}}, /* AYWW */
    {24.29, 24.29, {712, 1671, 2607}, {19.40, 9.73, 2.08}}, /* AYWW */
    {68.24, 68.24, {633, 1397, 2110}, {24.43, 14.07, 4.74}}, /* AYWW */
    {57.52, 57.52, {560, 1116, 2228}, {17.16, 13.35, 4.35}}, /* AYWW */
    {11.47, 11.47, {539, 1067, 2705}, {16.03, 13.33, 2.33}}, /* AYWW */
    {58.51, 58.51, {459, 814, 2459}, {7.25, 9.89, 2.54}}, /* AYWW */
    {11.77, 11.77, {679, 791, 2175}, {4.58, 6.03, 1.26}}, /* AYWW */
    {11.50, 11.50, {679, 791, 2175}, {4.58, 6.03, 1.26}}, /* AYWW */
    {1.00, 1.00, {564, 862, 2501}, {6.51, 7.02, 0.82}}, /* OWWW */
    {11.98, 11.98, {549, 905, 2529}, {10.07, 11.94, 4.16}}, /* OWWW */
    {11.48, 11.48, {561, 891, 2482}, {10.26, 13.15, 2.52}}, /* OWWW */
    {11.46, 11.46, {542, 892, 2501}, {10.05, 12.72, 4.18}}, /* OWWW */
    {11.47, 11.47, {548, 866, 2387}, {9.95, 12.89, 2.26}}, /* OWWW */
    {11.43, 11.43, {519, 869, 2602}, {8.82, 12.02, 2.70}}, /* OWWW */
    {22.92, 22.92, {520, 832, 2506}, {9.18, 11.78, 3.43}}, /* OWWW */
    {11.46, 11.46, {485, 827, 2543}, {8.68, 11.40, 2.06}}, /* OWWW */
    {11.48, 11.48, {501, 837, 2529}, {8.22, 11.09, 1.07}}, /* OWWW */
    {11.46, 11.46, {487, 824, 2595}, {8.10, 11.03, 3.34}}, /* OWWW */
    {22.94, 22.94, {468, 818, 2535}, {7.15, 10.09, 2.54}}, /* OWWW */
    {11.47, 11.47, {466, 826, 2584}, {7.05, 9.64, 3.75}}, /* OWWW */
    {22.87, 22.87, {452, 793, 2494}, {6.41, 8.73, 2.24}}, /* OWWW */
    {12.41, 12.41, {561, 789, 2501}, {2.56, 3.98, 1.02}}, /* OWWW */
    {11.50, 11.50, {561, 789, 2501}, {2.56, 3.98, 1.02}}, /* OWWW */
    {1.00, 1.00, {271, 1499, 2383}, {0.40, 0.24, 0.21}}, /* ZZ */
    {11.45, 11.45, {298, 1491, 2394}, {4.81, 3.49, 3.70}}, /* ZZ */
    {11.39, 11.39, {274, 1499, 2392}, {10.03, 6.86, 7.25}}, /* ZZ */
    {78.93, 78.93, {276, 1508, 2395}, {8.23, 5.19, 5.43}}, /* ZZ */
    {1.00, 1.00, {434, 1676, 2563}, {6.69, 4.59, 1.88}}, /* ZH */
    {67.10, 67.10, {439, 1677, 2500}, {6.58, 4.44, 2.37}}, /* ZH */
    {1.00, 1.00, {282, 1589, 2352}, {3.90, 3.82, 1.28}}, /* DH */
    {90.33, 90.33, {246, 1598, 2331}, {2.44, 2.02, 0.76}}, /* DH */
    {1.00, 1.00, {624, 1406, 2392}, {1.19, 0.41, 0.17}}, /* BO */
    {11.12, 11.12, {274, 1406, 2392}, {6.89, 0.33, 0.61}}, /* BO */
    {11.37, 11.37, {271, 1406, 2392}, {7.40, 0.29, 0.72}}, /* BO */
    {11.30, 11.30, {410, 1406, 2392}, {8.53, 1.49, 1.65}}, /* BO */
    {11.87, 11.87, {624, 1406, 2392}, {20.14, 14.98, 7.74}}, /* BO */
    {35.03, 35.03, {622, 1400, 2395}, {8.18, 6.33, 3.09}}, /* BO */
    {1.00, 1.00, {440, 1486, 2485}, {0.35, 0.29, 0.43}}, /* DO */
    {11.33, 11.33, {263, 1486, 2485}, {4.58, 0.28, 0.59}}, /* DO */
    {21.63, 21.63, {274, 1486, 2485}, {7.52, 0.38, 0.72}}, /* DO */
    {12.34, 12.34, {440, 1486, 2485}, {10.49, 7.15, 4.09}}, /* DO */
    {12.41, 12.41, {439, 1486, 2514}, {7.92, 5.46, 2.77}}, /* DO */
    {11.77, 11.77, {440, 1486, 2485}, {1.67, 1.00, 0.26}}, /* DO */
    {11.50, 11.50, {440, 1486, 2485}, {1.67, 1.00, 0.26}}, /* DO */
    {1.00, 1.00, {254, 1972, 2404}, {2.56, 1.69, 1.06}}, /* ED */
    {11.53, 11.53, {239, 1951, 2635}, {3.99, 1.83, 1.51}}, /* ED */
    {12.84, 12.84, {205, 1951, 2635}, {1.99, 0.41, 0.18}}, /* ED */
    {11.50, 11.50, {205, 1951, 2635}, {1.99, 0.41, 0.18}}, /* ED */
    {1.00, 1.00, {265, 1504, 2470}, {3.32, 3.27, 1.48}}, /* OD */
    {10.81, 10.81, {260, 1522, 2447}, {3.12, 2.00, 1.06}}, /* OD */
    {12.92, 12.92, {273, 1504, 2470}, {1.57, 0.48, 0.19}}, /* OD */
    {11.50, 11.50, {273, 1504, 2470}, {1.57, 0.48, 0.19}}, /* OD */
    {1.00, 1.00, {254, 1306, 2523}, {3.01, 0.37, 0.49}}, /* GE */
    {10.53, 10.53, {265, 1063, 2551}, {6.37, 0.29, 0.69}}, /* GE */
    {11.46, 11.46, {265, 1307, 2508}, {6.36, 0.35, 0.61}}, /* GE */
    {11.41, 11.41, {267, 1960, 2582}, {5.72, 0.35, 0.91}}, /* GE */
    {11.50, 11.50, {267, 1960, 2582}, {5.72, 0.35, 0.91}}, /* GE */
    {1.00, 1.00, {250, 2012, 2386}, {5.66, 0.19, 0.54}}, /* GO */
    {11.45, 11.45, {258, 2012, 2480}, {6.87, 1.03, 0.53}}, /* GO */
    {11.43, 11.43, {264, 1436, 2570}, {6.73, 0.55, 0.82}}, /* GO */
    {22.97, 22.97, {282, 1718, 2551}, {2.94, 0.62, 0.37}}, /* GO */
    {1.00, 1.00, {264, 2117, 2480}, {8.05, 5.10, 1.17}}, /* EG */
    {11.45, 11.45, {258, 2124, 2467}, {5.51, 3.38, 0.63}}, /* EG */
    {11.50, 11.50, {258, 2124, 2467}, {5.51, 3.38, 0.63}}, /* EG */
    {1.00, 1.00, {279, 1314, 2503}, {1.06, 0.51, 0.38}}, /* OG */
    {10.62, 10.62, {279, 1314, 2423}, {7.49, 6.23, 0.95}}, /* OG */
    {22.94, 22.94, {262, 1319, 2480}, {5.45, 3.96, 0.82}}, /* OG */
};

static const struct {
  short first, count;
} vf_index[] = {
    {0, 0}, {0, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {4, 5}, {0, 0}, {0, 0}, {0, 0},
    {9, 4}, {0, 0}, {0, 0}, {13, 4}, {0, 0}, {17, 7},
    {24, 5}, {29, 14}, {0, 0}, {0, 0}, {43, 13}, {0, 0},
    {56, 10}, {66, 7}, {73, 11}, {84, 9}, {93, 8}, {101, 8},
    {109, 15}, {0, 0}, {0, 0}, {124, 4}, {128, 2}, {130, 2},
    {0, 0}, {132, 6}, {0, 0}, {0, 0}, {0, 0}, {138, 7},
    {145, 4}, {149, 4}, {153, 5}, {158, 4}, {162, 3}, {165, 3},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
};
/* VF-END */

/* the hiss of ZZ and ZH: oscillators 4 and 5 at 3.5 to 4.5kHz, about 20dB
 * under the voicing's loudest band, half enveloped by the voicing and
 * following each frame's level */
static const struct {
  short code;
  double n1, n2, db, dist;
} framed_hiss[] = {
    {167, 3600, 4100, -22, 30}, /* ZZ */
    {168, 3300, 4100, -24, 60}, /* ZH */
};

static void framed_noise(sound *s, int code) {
  for (int k = 0; k < (int)(sizeof(framed_hiss) / sizeof(framed_hiss[0])); k++) {
    if (framed_hiss[k].code != code)
      continue;
    double loudest = 0;
    for (int i = 0; i < s->nparts; i++)
      loudest = fmax2(loudest, s->parts[i].target.vol[0]);
    for (int i = 0; i < s->nparts; i++) {
      frame *f = &s->parts[i].target;
      const double v = 24 * pow(10, framed_hiss[k].db / 20) * f->vol[0] / loudest;
      f->freq[3] = framed_hiss[k].n1;
      f->freq[4] = framed_hiss[k].n2;
      f->vol[3] = v;
      f->vol[4] = v * 0.7;
      f->distortion = framed_hiss[k].dist;
      f->half_env = true;
    }
  }
}

static void allophone(sound *s, int code) {
  stop st;
  if (vf_index[code - 128].count > 0) {
    const vf_point *p = &vf_points[vf_index[code - 128].first];
    s->speech = true;
    s->framed = true;
    s->length = allophone_length[code - 128];
    for (int i = 0; i < vf_index[code - 128].count; i++)
      add_part(s, p[i].ms, p[i].glide,
               voiced_frame(FM(p[i].freq[0], p[i].freq[1], p[i].freq[2]), p[i].vol[0], p[i].vol[1], p[i].vol[2]));
    for (int i = 0; i < s->nparts; i++)
      s->parts[i].target.onset_ms = onset_ms(code);
    framed_noise(s, code);
    const double g = pow(10, allophone_gain_db[code - 128] / 20);
    for (int i = 0; i < s->nparts; i++)
      for (int j = 0; j < 5; j++)
        s->parts[i].target.vol[j] *= g;
    return;
  }
  switch (code) {
  /* the vowels' and liquids' three volumes are the chip's, from single voicing
   * pulses (see voice_fade). some sounds are played from the chip's own frames
   * instead (see vf_points) */
  case 128: steady(s, 70, voiced_frame(fIY, 24.0, 15.6, 9.8)); break; /* IY */
  case 129: steady(s, 70, formant_phases(voiced_frame(fIH, 24.0, 16.7, 10.0), 0.132, 0.175)); break; /* IH */
  case 130: steady(s, 70, voiced_frame(fEY, 24.0, 14.0, 7.7)); break; /* EY */
  case 131: steady(s, 70, voiced_frame(fEH, 24.0, 13.4, 9.3)); break; /* EH */
  case 132: steady(s, 70, voiced_frame(fAY, 24.0, 11.3, 6.4)); break; /* AY */
  case 133: steady(s, 70, voiced_frame(fAX, 24.0, 14.8, 8.8)); break; /* AX */
  case 134: steady(s, 70, voiced_frame(fUX, 24.0, 15.4, 9.6)); break; /* UX */
  case 135: steady(s, 70, voiced_frame(fOH, 24.0, 17.9, 9.1)); break; /* OH */
  case 136: steady(s, 70, voiced_frame(fAW, 24.0, 22.3, 6.6)); break; /* AW */
  case 137: steady(s, 70, formant_phases(voiced_frame(fOW, 20.1, 24.0, 4.9), 0.954, 0.982)); break; /* OW */
  case 138: steady(s, 70, formant_phases(voiced_frame(fUH, 22.5, 24.0, 6.0), 0.924, 0.928)); break; /* UH */
  case 139: steady(s, 70, formant_phases(voiced_frame(fUW, 24.0, 22.5, 3.9), 0.955, 0.032)); break; /* UW */
  /* the nasals' resonances and volumes are fitted to the VoxRecord recording's
   * third take (the middle of each, in sixth octaves from 200Hz to 3.9kHz):
   * besides the low murmur the chip's nasals have strong resonances near 1.4
   * to 1.6kHz and 2.2 to 2.6kHz that the published values here lacked */
  case 140: steady(s, 70, voiced_frame(FM(266, 1410, 2252), 24.0, 6.3, 1.9)); break; /* MM */
  case 141: steady(s, 70, formant_phases(voiced_frame(FM(262, 1572, 2582), 24.0, 3.6, 7.6), 0.032, 0.965)); break; /* NE */
  case 142: steady(s, 70, formant_phases(voiced_frame(FM(269, 1169, 2590), 24.0, 4.6, 7.3), 0.962, 0.031)); break; /* NO */
  /* NGE and NGO are fitted to their harmonics on both units: on the chip NGE's
   * second and third formants are one peak near 2.3kHz */
  case 143: steady(s, 70, formant_phases(voiced_frame(FM(368.334, 2266.3, 2386.423), 22.0, 6.845, 2.093), 0.225, 0.909)); break; /* NGE */
  case 144: steady(s, 70, formant_phases(voiced_frame(FM(374.718, 1491.004, 1938.775), 22.0, 4.018, 9.051), 0.868, 0.994)); break; /* NGO */
  case 145: steady(s, 70, formant_phases(voiced_frame(fLE, 23.3, 24.0, 4.3), 0.945, 0.930)); break; /* LE */
  case 146: steady(s, 70, liquid_frame(fLO)); break;
  case 147: steady(s, 70, formant_phases(voiced_frame(fWW, 17.7, 24.0, 1.8), 0.952, 0.056)); break; /* WW */
  case 148: steady(s, 70, formant_phases(voiced_frame(fRR, 24.0, 19.5, 4.5), 0.993, 0.005)); break; /* RR */
  /* the diphthongs' vowel ends take their vowel's volumes (above) */
  case 149: diphthong(s, 200, voiced_frame(fIY, 24.0, 15.6, 9.8), liquid_frame(fER)); break; /* IYRR */
  case 150: diphthong(s, 200, voiced_frame(fEH, 24.0, 13.4, 9.3), liquid_frame(fER)); break; /* EYRR */
  case 151: diphthong(s, 190, vowel_frame(FM(500, 1400, 1750)), liquid_frame(fER)); break; /* AXRR */
  case 152: diphthong(s, 200, vowel_frame(fOHr), liquid_frame(fER)); break; /* AWRR */
  case 153: diphthong(s, 185, vowel_frame(fAWr), liquid_frame(fER)); break; /* OWRR */
  case 154: diphthong(s, 165, voiced_frame(fEY, 24.0, 14.0, 7.7), voiced_frame(fIY, 24.0, 15.6, 9.8)); break; /* EYIY */
  case 155: diphthong(s, 200, voiced_frame(fOH, 24.0, 17.9, 9.1), voiced_frame(fIY, 24.0, 15.6, 9.8)); break; /* OHIY */
  case 156: diphthong(s, 225, vowel_frame(fAWr), voiced_frame(fIY, 24.0, 15.6, 9.8)); break; /* OWIY */
  case 157: diphthong(s, 185, voiced_frame(fOH, 24.0, 17.9, 9.1), voiced_frame(fIH, 24.0, 16.7, 10.0)); break; /* OHIH */
  case 158: diphthong(s, 170, vowel_frame(fYY), voiced_frame(fEH, 24.0, 13.4, 9.3)); break; /* IYEH */
  case 159: diphthong(s, 140, voiced_frame(fAX, 24.0, 14.8, 8.8), liquid_frame(fLO)); break; /* EHLL */
  case 160: diphthong(s, 180, vowel_frame(fYY), voiced_frame(fUW, 24.0, 22.5, 3.9)); break; /* IYUW */
  case 161: diphthong(s, 170, vowel_frame(fAH), voiced_frame(fUW, 24.0, 22.5, 3.9)); break; /* AXUW */
  case 162: diphthong(s, 170, vowel_frame(FM(330, 1300, 2300)), voiced_frame(fUW, 24.0, 22.5, 3.9)); break; /* IHWW */
  case 163: diphthong(s, 200, voiced_frame(fAY, 24.0, 11.3, 6.4), voiced_frame(fUH, 22.5, 24.0, 6.0)); break; /* AYWW */
  case 164: diphthong(s, 131, voiced_frame(fOW, 20.1, 24.0, 4.9), vowel_frame(FM(330, 700, 2250))); break; /* OWWW */
  case 165: /* JH */
    st = STOP(23.6008, 14.6703, 8.8361, true, lPalatal, 4086.8, 1843.5, 20.47, 25.65, 0, 0);
    affricate(s, &st, voiced_noise_frame(FM(158, 1974, 1842), 21.23, 1984.3, 3081.6, 26.87, 18.82, 47.7), 46.3992);
    break;
  case 166: steady(s, 70, voiced_noise_frame(FM(492, 2083, 3156), 3.6, 2253, 1368, 1.4, 5.62, 78)); break; /* VV */
  case 167: steady(s, 70, hush(voiced_noise_frame(FM(240, 1500, 2500), 14, 3700, 3950, 14, 10, 160), FRICATIVE_DB)); break; /* ZZ */
  case 168: steady(s, 70, frZH()); break; /* ZH */
  case 169: steady(s, 70, hush(voiced_noise_frame(FM(250, 1300, 2400), 16, 3000, 3800, 7, 5, 255), FRICATIVE_DB)); break; /* DH */
  case 170: st = STOPN(45, 31.6, 10.8, true, FM(363, 1741, 2562), 2149, 3662, 22, 16.4, 0, 0, 124, 0, 0, 0, 10, 0.84); plosive(s, &st); break; /* BE */
  case 171: st = STOP(45, 25, 8, true, lLabialBack, 600, 1000, 10, 6, 0, 0); plosive(s, &st); break; /* BO */
  case 172: st = STOPN(10, 2.85, 7.15, true, FM(345, 918, 2893), 1733.4, 1870.5, 15.96, 21.89, 0, 0, 99.2, 0, 0, 0, 25.63, 0.444); plosive(s, &st); break; /* EB */
  /* OB and PO keep the values they had before the refit through the new
   * filter, which matched both units worse once the recordings' background
   * was taken out (OB 66 to 75%, PO 48 to 64% and 4.9 to 3.0dB on unit B) */
  case 173: st = STOPN(10, 1, 0.2, true, FM(416, 1414, 2909), 835, 3504, 29.1, 22.8, 0, 0, 201, 0, 0, 0, 2.2, 0.25); plosive(s, &st); break; /* OB */
  case 174: st = STOPN(45, 21.26, 23.74, true, lAlveolar, 2535.3, 1885.7, 29.96, 28.29, 0, 0, 57.5, 0, 0, 0, 12.27, 0.941); plosive(s, &st); break; /* DE */
  case 175: st = STOP(45, 25, 8, true, lAlveolarBak, 1800, 2220, 12, 8, 0, 0); plosive(s, &st); break; /* DO */
  case 176: st = STOPN(10, 1.87, 0.77, true, FM(281, 2404, 2711), 3411, 3749, 16.6, 20.2, 0, 0, 189, 0, 0, 0, 0.3, 0.44); plosive(s, &st); break; /* ED */
  case 177: st = STOPN(10, 2.21, 0.46, true, FM(278, 1462, 2581), 2076, 3335, 15.2, 24.3, 0, 0, 208, 0, 0, 0, 0.6, 0.93); plosive(s, &st); break; /* OD */
  case 178: st = STOP(55, 30, 10, true, lVelar, 1610, 2030, 12, 8, 0, 0); plosive(s, &st); break; /* GE */
  case 179: st = STOP(55, 30, 10, true, lVelarBack, 650, 900, 12, 8, 0, 0); plosive(s, &st); break; /* GO */
  case 180: st = STOP(55, 35, 10, true, lVelar, 2300, 2900, 10, 6, 0, 0); plosive(s, &st); break; /* EG */
  case 181: st = STOP(55, 35, 10, true, lVelarBack, 910, 1260, 10, 6, 0, 0); plosive(s, &st); break; /* OG */
  case 182: /* CH */
    st = STOP(49.4544, 23.4763, 3.4515, false, lPalatal, 2623.8, 2225.2, 1.57, 2.79, 0, 0);
    affricate(s, &st, noise_frame(lPalatal, 2260.1, 3154.6, 10.14, 24.25, 42.6), 20.5456);
    break;
  case 183: steady(s, 70, noise_frame(FM(400, 1900, 2600), 1546.1, 1464.8, 27.07, 7.24, 192.3)); break; /* HE */
  case 184: steady(s, 70, noise_frame(FM(600, 1000, 2400), 1368.4, 1277.8, 23.85, 10.37, 165.2)); break; /* HO */
  case 185: steady(s, 70, noise_frame(fWW, 821.4, 731.8, 16.48, 19.02, 209.7)); break; /* WH */
  case 186: steady(s, 70, noise_frame(lLabial, 2823.1, 3165.5, 15.64, 16.67, 107.3)); break; /* FF */
  case 187: thumped(s, 4.36, 414.1, 15.84, 35.64, frSE()); break; /* SE */
  case 188: thumped(s, 4.53, 436.9, 15.26, 35.47, frSO()); break; /* SO */
  case 189: thumped(s, 9.13, 363.2, 11.07, 40.87, frSH()); break; /* SH */
  case 190: steady(s, 40, noise_frame(lDental, 2170.3, 2087.4, 26.95, 26.05, 143.4)); break; /* TH */
  case 191: st = STOPN(50, 31.8, 7.11, false, lAlveolar, 2689, 3257, 15.9, 22.1, 3188, 3038, 73, 19, 3.7, 17.4, 0, 0); plosive(s, &st); break; /* TT */
  case 192: st = STOPN(70, 43.56, 26.34, false, lAlveolar, 3361.7, 2605.1, 24.92, 19.05, 2420.8, 2928, 45.6, 56.5, 7.51, 5.36, 0, 0); plosive(s, &st); break; /* TU */
  case 193: { /* TS */
    /* TS is hiss that starts at its loudest, peaked at 3.5kHz, and dies away
     * (6dB down at 40ms, 14dB by 120ms), spreading out to a flat spectrum */
    const frame hiss = noise_frame(lAlveolar, 3282.5, 3250.4, 8.04, 19.69, 48.6);
    s->speech = true;
    /* 50, 10, 40 and 90ms (with the glides) at the manual's 170ms: see
     * allophone_length */
    add_part(s, 44.7368, 5.3684, silent_frame(lAlveolar));
    add_part(s, 8.9474, 1.7895, hiss);
    add_part(s, 35.7895, 35.7895, hush(hiss, 6));
    add_part(s, 80.5263, 80.5263, hush(noise_frame(lAlveolar, 3900, 3800, 11.95, 25.78, 100), 20));
    break;
  }
  case 194: st = STOPN(55, 15.33, 6.09, false, lVelar, 1641, 2125.3, 21.74, 25.19, 3713.1, 4001.3, 62.9, 159, 0.39, 0.78, 0, 0); plosive(s, &st); break; /* KE */
  case 195: st = STOPN(55, 43.36, 4.21, false, lVelarBack, 1064, 1788.5, 18.12, 16.08, 1591.2, 1262, 185.3, 79.5, 5.49, 7.86, 0, 0); plosive(s, &st); break; /* KO */
  case 196: st = STOPN(55, 43.95, 8.79, false, lVelar, 2933.4, 3338, 0.86, 0.06, 3538.4, 1860.3, 56.9, 108.4, 4.54, 25.1, 0, 0); plosive(s, &st); break; /* EK */
  case 197: st = STOPN(45, 11.86, 31.21, false, lVelarBack, 3171.3, 812.5, 1.72, 0.23, 1975, 1050.1, 58.1, 112.5, 26.71, 7.48, 0, 0); plosive(s, &st); break; /* OK */
  case 198: st = STOPN(99, 59.57, 31.25, false, lLabial, 410.4, 848.4, 19.62, 14.17, 1315.3, 2497.7, 227.1, 88.6, 9.82, 30.48, 0, 0); plosive(s, &st); break; /* PE */
  case 199: st = STOP(99, 50, 9, false, lLabialBack, 300, 720, 12, 9, 600, 1380); plosive(s, &st); break; /* PO */
  default: break;
  }
  s->length = allophone_length[code - 128];
  const double g = pow(10, allophone_gain_db[code - 128] / 20);
  for (int i = 0; i < s->nparts; i++) {
    for (int j = 0; j < 5; j++)
      s->parts[i].target.vol[j] *= g;
    s->parts[i].target.onset_ms = onset_ms(code);
  }
}

/* effect constructors */

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
/* the effects traced from recordings that are not pure tones (the tone effects
 * are in fx_knots). each point is where it is reached (ms from the start of
 * the sound), its kind, a buzz's pitch, its level in dB (-60 is silence), up
 * to three frequencies (0 for none) and the second and third's levels against
 * the first. a tone is a pure sine; noise is noise around its frequency; a
 * buzz is voiced at its pitch with oscillators 1 to 3 on its formants. the
 * levels have the output filter's loss added back (see CHIP_RATE). A1 is three
 * formants (steady at 175Hz, rising 720 to 1645Hz, and rising faster from 120
 * to 2870Hz) */
enum { FX_TONE, FX_NOISE, FX_BUZZ };

typedef struct fx_point {
  short ms, kind, pitch;
  double db;
  short freq[3];
  double rel[3];
} fx_point;

static const fx_point fx_points[] = {
    /* A1 */
    {15, 2, 87, -60, {175, 720, 120}, {0, 15, -3}},
    {35, 2, 87, -16.1, {175, 720, 120}, {0, 15, -3}},
    {60, 2, 87, 4.7, {175, 750, 172}, {0, -5.3, -3}},
    {85, 2, 87, 18.2, {175, 780, 224}, {0, -16.7, -3}},
    {160, 2, 87, 7.9, {175, 868, 428}, {0, -4.3, 3.6}},
    {185, 2, 87, 5.7, {175, 898, 495}, {0, -2.9, -1}},
    {210, 2, 87, 7.6, {175, 929, 567}, {0, -10.8, -6.4}},
    {235, 2, 87, 6.7, {175, 960, 640}, {0, -1.2, -6.9}},
    {260, 2, 87, 8.4, {175, 992, 709}, {0, -11.9, -3.9}},
    {285, 2, 87, 8.5, {175, 1024, 778}, {0, -3, -3}},
    {485, 2, 87, 2.9, {175, 1215, 1445}, {0, -3, -3}},
    {510, 2, 87, 2.3, {175, 1239, 1525}, {0, -3.4, -7.7}},
    {535, 2, 87, 1.5, {175, 1263, 1605}, {0, -2.6, -1.4}},
    {560, 2, 87, 2.7, {175, 1295, 1689}, {0, -7.5, -6.1}},
    {585, 2, 87, 3.9, {175, 1327, 1773}, {0, -3.5, -7.6}},
    {610, 2, 87, 2, {175, 1357, 1858}, {0, -3.6, -0.3}},
    {635, 2, 87, 4.8, {175, 1388, 1943}, {0, -9.4, -11.3}},
    {660, 2, 87, 2.6, {175, 1418, 2027}, {0, -1.1, -1.2}},
    {710, 2, 87, 3.3, {175, 1473, 2192}, {0, -5.5, -8.1}},
    {735, 2, 87, 2.6, {175, 1498, 2272}, {0, -5.5, -1.1}},
    {760, 2, 87, 5.4, {175, 1516, 2347}, {0, -7.2, -12.6}},
    {785, 2, 87, 2.8, {175, 1535, 2422}, {0, -3.9, 2.8}},
    {810, 2, 87, 3.5, {175, 1554, 2497}, {0, -6.5, -9}},
    {860, 2, 87, 2.1, {175, 1591, 2648}, {0, -5.8, -2.7}},
    {885, 2, 87, 5.2, {175, 1610, 2723}, {0, -6.8, -12.4}},
    {910, 2, 87, 7.9, {175, 1627, 2797}, {0, -10.4, -7.8}},
    {975, 2, 87, 6, {175, 1668, 2984}, {0, -3, -3}},
    /* A3. its buzz was traced at 470 to 490Hz, but on both units of the
     * fourth VoxProbe recording it repeats at the voicing frequency (60.2,
     * 87.1 and 150Hz at pitch 60, 88 and 150), so its buzz is at the default
     * voicing pitch here and follows Pitch: its items match both units
     * better (79 to 80%, 80 to 82%) */
    {5, 2, 88, -60, {576, 977, 1996}, {0, 27.2, 36.5}},
    {20, 2, 88, -60, {576, 990, 2010}, {0, 26.3, 38}},
    {25, 2, 88, -6.5, {577, 990, 2386}, {0, -35, -12.1}},
    {30, 2, 88, -5.5, {577, 978, 2386}, {0, -1.6, -13.1}},
    {35, 2, 88, -3.9, {577, 978, 2386}, {0, -1, -13.9}},
    {80, 2, 88, -4.6, {578, 1065, 2476}, {0, 1, -14}},
    {120, 2, 88, -3.5, {626, 1164, 2491}, {0, 0.2, -15.2}},
    {125, 2, 88, -5.5, {632, 1177, 1852}, {0, 0.1, 3.6}},
    {135, 2, 88, -4.9, {645, 1199, 1852}, {0, -0.1, 2.7}},
    {520, 2, 88, -8.7, {1452, 2357, 3417}, {0, 4.1, 6}},
    {545, 2, 88, -14.2, {1465, 2382, 3447}, {0, 5.2, 6.6}},
    {550, 2, 88, -26, {1459, 2382, 3431}, {0, 4, 6.5}},
    {555, 2, 88, -60, {1304, 2379, 3423}, {0, -21.7, 6.5}},
    {570, 2, 88, -60, {1011, 1986, 2975}, {0, 9, 16.6}},
    /* B9: traced again from unit B's VoxRecord recording every 2ms (8ms
     * windows), its three strongest tones followed by continuity, with its own
     * peaking filters (see sound_eq): all 14 recordings of both units 1.83 to
     * 1.43, the other pitches and bends 1.83 to 1.45 */
    {2, 0, 0, -60, {0, 0, 0}, {0, -60, -60}},
    {18, 0, 0, -60, {0, 0, 0}, {0, -60, -60}},
    {20, 0, 0, -24.7, {1943, 266, 0}, {0, -25.8, -60}},
    {22, 0, 0, -10.1, {2690, 1822, 883}, {0, -0.6, -12.3}},
    {26, 0, 0, -0.8, {2707, 1805, 893}, {0, 0.2, -12.1}},
    {78, 0, 0, -5.1, {2669, 1795, 840}, {0, 0.5, -12.3}},
    {80, 0, 0, -60, {2669, 761, 1033}, {0, -60, -60}},
    {98, 0, 0, -60, {2669, 753, 242}, {0, -60, -60}},
    {100, 0, 0, -39.5, {628, 995, 809}, {0, 5.8, 4.9}},
    {104, 0, 0, -11.9, {666, 995, 809}, {0, -60, -60}},
    {110, 0, 0, -27.8, {624, 995, 134}, {0, -60, -26.4}},
    {112, 0, 0, -32.9, {551, 995, 134}, {0, -60, -60}},
    {116, 0, 0, -10.8, {569, 995, 134}, {0, -60, -60}},
    {120, 0, 0, -19.4, {548, 995, 134}, {0, -60, -60}},
    {122, 0, 0, -13.9, {3756, 2499, 454}, {0, 2.4, -17.5}},
    {124, 0, 0, -5.4, {3781, 2502, 1220}, {0, 2.7, -18}},
    {126, 0, 0, -60, {3781, 2510, 507}, {0, -60, -60}},
    {130, 0, 0, -60, {3781, 2514, 527}, {0, -60, -60}},
    {132, 0, 0, -2.8, {3813, 2512, 517}, {0, 2.6, -19}},
    {134, 0, 0, -2.8, {3815, 2512, 406}, {0, 2.6, -35.6}},
    {136, 0, 0, -2.8, {3813, 2514, 344}, {0, 2.6, -24.4}},
    {138, 0, 0, -60, {3813, 2511, 505}, {0, -60, -60}},
    {140, 0, 0, -60, {3813, 2510, 521}, {0, -60, -60}},
    {142, 0, 0, -2.6, {3810, 2511, 525}, {0, 2.3, -12.8}},
    {144, 0, 0, -2.6, {3810, 2512, 521}, {0, 2.4, -22.8}},
    {146, 0, 0, -2.7, {3810, 2512, 713}, {0, 2.5, -29.6}},
    {148, 0, 0, -2.7, {3812, 2514, 478}, {0, 2.5, -16.7}},
    {150, 0, 0, -60, {3812, 2513, 511}, {0, -60, -60}},
    {152, 0, 0, -60, {3812, 2513, 523}, {0, -60, -60}},
    {154, 0, 0, -2.8, {3814, 2513, 523}, {0, 2.6, -14.9}},
    {156, 0, 0, -2.7, {3811, 2513, 506}, {0, 2.5, -26.6}},
    {158, 0, 0, -2.8, {3812, 2513, 176}, {0, 2.7, -36.5}},
    {160, 0, 0, -3.4, {3816, 2502, 506}, {0, 2.1, -12.8}},
    {162, 0, 0, -60, {3816, 2502, 516}, {0, -60, -60}},
    {190, 0, 0, -60, {3816, 272, 718}, {0, -60, -60}},
    {192, 0, 0, -36.4, {511, 695, 872}, {0, 4.9, 6.7}},
    {194, 0, 0, -60, {511, 750, 872}, {0, -60, -60}},
    {206, 0, 0, -60, {511, 823, 877}, {0, -60, -60}},
    {208, 0, 0, -11.4, {892, 3494, 1794}, {0, 7.6, 10.1}},
    {264, 0, 0, -13, {900, 3503, 1806}, {0, 10.7, 12.9}},
    {268, 0, 0, -21.2, {896, 3506, 1807}, {0, 18.9, 21}},
    {270, 0, 0, -60, {896, 3504, 1807}, {0, -60, -60}},
    {274, 0, 0, -60, {896, 3506, 1807}, {0, -60, -60}},
    {276, 0, 0, -20, {886, 3504, 1808}, {0, 16.2, 18.2}},
    {280, 0, 0, -26.8, {868, 3505, 1807}, {0, 21.7, 23.8}},
    {282, 0, 0, -60, {868, 3507, 1808}, {0, -60, -60}},
    {286, 0, 0, -60, {868, 3510, 1808}, {0, -60, -60}},
    {288, 0, 0, -26.7, {897, 3506, 1807}, {0, 18, 20.7}},
    {292, 0, 0, -32.3, {936, 3503, 1808}, {0, 21.4, 24.2}},
    {294, 0, 0, -60, {936, 3504, 1807}, {0, -60, -60}},
    {298, 0, 0, -60, {936, 3510, 1807}, {0, -60, -60}},
    {300, 0, 0, -42.2, {754, 3505, 1809}, {0, 24.2, 26.5}},
    {302, 0, 0, -46.2, {655, 3498, 1806}, {0, 25, 27.1}},
    {304, 0, 0, -42, {815, 454, 1815}, {0, -8.3, 17.6}},
    {306, 0, 0, -60, {815, 359, 1815}, {0, -60, -60}},
    {319, 0, 0, -60, {815, 204, 1815}, {0, -60, -60}},
    /* C0 */
    {5, 0, 0, -60, {239, 991, 1545}, {0, -14.9, -60}},
    {20, 0, 0, -60, {239, 991, 1545}, {0, -0.1, -60}},
    {25, 0, 0, -4.2, {2315, 991, 1545}, {0, -5.4, -60}},
    {30, 0, 0, -3.9, {2315, 2058, 1545}, {0, -14.2, -60}},
    {35, 0, 0, -0.8, {2315, 2058, 1545}, {0, -16.1, -60}},
    {40, 0, 0, -2.1, {2058, 2058, 1545}, {0, -2.2, -60}},
    {45, 0, 0, -2.7, {2053, 2058, 1545}, {0, 0, -2.6}},
    {55, 0, 0, -4, {1790, 1767, 2327}, {0, -13.5, -10.6}},
    {70, 0, 0, -1.1, {1528, 1767, 2327}, {0, -11.8, -9}},
    {75, 0, 0, -1, {1265, 1767, 2327}, {0, -3.5, -4.9}},
    {80, 0, 0, -1.4, {1265, 2298, 1277}, {0, -6.9, -14.6}},
    {85, 0, 0, 0.5, {1260, 1516, 2327}, {0, -0.6, -2.9}},
    {90, 0, 0, -1.4, {1009, 1015, 2239}, {0, -11.7, -9.3}},
    {95, 0, 0, 1.4, {991, 1003, 2239}, {0, -8.1, -5.9}},
    {100, 0, 0, 0.7, {781, 764, 2239}, {0, -10.2, -8}},
    {105, 0, 0, 1.7, {764, 577, 2239}, {0, -13.7, 4.8}},
    {110, 0, 0, 2.3, {723, 402, 245}, {0, -12.3, -13.3}},
    {115, 0, 0, 0.2, {647, 309, 245}, {0, 0.1, -60}},
    {130, 0, 0, 2.5, {501, 309, 245}, {0, -60, -60}},
    {135, 0, 0, 3.1, {362, 309, 245}, {0, -60, -60}},
    {145, 0, 0, 3.6, {338, 455, 245}, {0, -60, -60}},
    {150, 0, 0, 2, {198, 455, 583}, {0, -60, -60}},
    {165, 0, 0, -5.1, {198, 455, 583}, {0, -60, -60}},
    {170, 0, 0, -6.2, {297, 455, 583}, {0, -6.3, -60}},
    {175, 0, 0, -5.8, {327, 595, 583}, {0, -0.1, -60}},
    {180, 0, 0, -7, {297, 455, 583}, {0, -6, -14.7}},
    {185, 0, 0, -9.9, {327, 595, 787}, {0, -2.7, -6.1}},
    {190, 0, 0, -9, {327, 595, 898}, {0, -6.4, -9.9}},
    {200, 0, 0, -13.4, {251, 484, 787}, {0, -8.2, -13.8}},
    {205, 0, 0, -12.7, {251, 566, 881}, {0, -9.3, -12}},
    {210, 0, 0, -14.5, {251, 484, 881}, {0, -0, -0.1}},
    {215, 0, 0, -17.3, {216, 490, 881}, {0, -8.8, -60}},
    {220, 0, 0, -17.1, {216, 490, 881}, {0, -0, -60}},
    {225, 0, 0, -60, {268, 490, 881}, {0, -10.7, -60}},
    {245, 0, 0, -60, {239, 461, 881}, {0, -60, -60}},
    /* C2 */
    {5, 0, 0, -60, {233, 216, 974}, {0, -60, -60}},
    {30, 0, 0, -60, {478, 910, 974}, {0, -5.1, -0.1}},
    {35, 0, 0, -6.8, {499, 904, 974}, {0, -0.1, -0.1}},
    {45, 0, 0, -6.4, {507, 904, 169}, {0, -2.8, -11}},
    {50, 0, 0, -0.8, {507, 910, 2210}, {0, -0.1, 4.5}},
    {55, 0, 0, -1.4, {507, 916, 2198}, {0, -2.7, -2.8}},
    {60, 0, 0, -1.5, {507, 921, 175}, {0, -2.6, -10.3}},
    {65, 0, 0, 0.9, {519, 933, 2012}, {0, -2.9, -7.5}},
    {75, 0, 0, -1.6, {525, 986, 2006}, {0, -3.2, -6.4}},
    {80, 0, 0, 0.7, {531, 1009, 239}, {0, -4.3, -9.4}},
    {85, 0, 0, -1, {543, 1020, 1814}, {0, -0, 1.9}},
    {90, 0, 0, -1.9, {548, 986, 239}, {0, -8.3, -10.9}},
    {100, 0, 0, -1.6, {566, 1114, 239}, {0, -0, 0.1}},
    {135, 0, 0, -9.7, {560, 1172, 245}, {0, -3.5, -9.9}},
    {140, 0, 0, -9.5, {554, 1172, 344}, {0, -7, -12.4}},
    {165, 0, 0, -25.7, {449, 1155, 251}, {0, -14.3, 0}},
    {170, 0, 0, -25.2, {449, 1155, 251}, {0, -9.1, -60}},
    {175, 0, 0, -22.7, {449, 175, 251}, {0, 0, -60}},
    {180, 0, 0, -9.3, {3312, 175, 251}, {0, -60, -60}},
    {190, 0, 0, -3.9, {3336, 175, 251}, {0, -13.1, -60}},
    {195, 0, 0, -4.4, {3336, 175, 618}, {0, -26.6, -60}},
    {205, 0, 0, 0.3, {3341, 192, 618}, {0, -13.2, -60}},
    {215, 0, 0, -1, {3341, 216, 618}, {0, -26.5, -60}},
    {220, 0, 0, -0.8, {3341, 297, 618}, {0, -17.5, -22.8}},
    {225, 0, 0, -20.7, {1674, 315, 618}, {0, -6.6, -10.5}},
    {230, 0, 0, -21.3, {1668, 315, 618}, {0, -7.2, -10.7}},
    {235, 0, 0, -22.4, {262, 472, 618}, {0, -2.9, -9.2}},
    {240, 0, 0, -60, {245, 472, 484}, {0, -14.6, -0}},
    {250, 0, 0, -60, {233, 472, 484}, {0, -60, -60}},
    /* C3 */
    {5, 0, 0, -60, {222, 490, 414}, {0, -60, -60}},
    {20, 0, 0, -60, {268, 490, 414}, {0, -7.4, -60}},
    {25, 0, 0, -9.4, {3143, 216, 414}, {0, -11.7, -60}},
    {30, 0, 0, -8.3, {3161, 175, 414}, {0, -19.1, -60}},
    {40, 0, 0, -3, {3166, 187, 414}, {0, -11.8, -60}},
    {45, 0, 0, -3.2, {3172, 175, 414}, {0, -25.7, -60}},
    {50, 0, 0, -0.9, {3166, 175, 414}, {0, -60, -60}},
    {115, 0, 0, 3.8, {3376, 175, 414}, {0, -60, -60}},
    {120, 0, 0, 3.9, {3470, 204, 414}, {0, -60, -60}},
    {180, 0, 0, 4.3, {3785, 204, 414}, {0, -60, -60}},
    {185, 0, 0, 4.4, {3785, 204, 414}, {0, -16.8, -60}},
    {190, 0, 0, 4.4, {3785, 204, 414}, {0, -16.8, -60}},
    {195, 0, 0, 4.4, {3785, 204, 414}, {0, -26.4, -60}},
    {200, 0, 0, 0.5, {3785, 338, 414}, {0, -18.9, -60}},
    {205, 0, 0, 0.7, {3773, 338, 414}, {0, -30.2, -60}},
    {210, 0, 0, 0.6, {3773, 356, 414}, {0, -31.1, -16.8}},
    {215, 0, 0, 0.6, {3773, 356, 414}, {0, -27.2, -31.4}},
    {220, 0, 0, -18.9, {233, 3779, 571}, {0, 10.3, -8.6}},
    {225, 0, 0, -20.3, {233, 3779, 741}, {0, 16.5, -10.4}},
    {230, 0, 0, -20.5, {233, 3779, 571}, {0, 16.7, -8.9}},
    {235, 0, 0, -20.3, {239, 3755, 571}, {0, 10.1, -8}},
    {240, 0, 0, -60, {239, 997, 571}, {0, -13.4, -60}},
    {250, 0, 0, -60, {239, 991, 571}, {0, -13.5, -60}},
    /* C4: traced again from unit B's VoxRecord recording every 2ms (8ms
     * windows), the one tone it is, with its own peaking filters (see sound_eq);
     * its pulsing with the voicing and its noise stay as they were (see
     * fx_pulse, fx_noise): all 13 recordings of both units 1.30 to 1.05, the
     * other pitches and bends 1.28 to 1.06 */
    {2, 0, 0, -60, {0, 0, 0}, {0, -60, -60}},
    {16, 0, 0, -57.4, {210, 0, 0}, {0, -60, -60}},
    {18, 0, 0, -36.1, {959, 0, 0}, {0, -60, -60}},
    {24, 0, 0, -60, {941, 0, 0}, {0, -60, -60}},
    {26, 0, 0, -24.6, {910, 0, 0}, {0, -60, -60}},
    {28, 0, 0, -14.7, {953, 0, 0}, {0, -60, -60}},
    {34, 0, 0, -27.7, {956, 0, 0}, {0, -60, -60}},
    {36, 0, 0, -54.5, {195, 0, 0}, {0, -60, -60}},
    {38, 0, 0, -49.4, {182, 0, 0}, {0, -60, -60}},
    {40, 0, 0, -14, {932, 0, 0}, {0, -60, -60}},
    {42, 0, 0, -7.3, {957, 0, 0}, {0, -60, -60}},
    {48, 0, 0, -25.5, {969, 0, 0}, {0, -60, -60}},
    {50, 0, 0, -36.1, {1295, 0, 0}, {0, -60, -60}},
    {52, 0, 0, -13.8, {916, 0, 0}, {0, -60, -60}},
    {54, 0, 0, -4.7, {954, 0, 0}, {0, -60, -60}},
    {58, 0, 0, -9.2, {965, 0, 0}, {0, -60, -60}},
    {62, 0, 0, -27.9, {1093, 0, 0}, {0, -60, -60}},
    {66, 0, 0, -2.7, {956, 0, 0}, {0, -60, -60}},
    {72, 0, 0, -20.8, {956, 0, 0}, {0, -60, -60}},
    {74, 0, 0, -25.3, {776, 0, 0}, {0, -60, -60}},
    {76, 0, 0, -7.3, {935, 0, 0}, {0, -60, -60}},
    {78, 0, 0, -2.2, {958, 0, 0}, {0, -60, -60}},
    {84, 0, 0, -25.1, {946, 0, 0}, {0, -60, -60}},
    {86, 0, 0, -19.8, {791, 0, 0}, {0, -60, -60}},
    {90, 0, 0, -2, {960, 0, 0}, {0, -60, -60}},
    {96, 0, 0, -27.8, {971, 0, 0}, {0, -60, -60}},
    {98, 0, 0, -12.8, {1965, 0, 0}, {0, -60, -60}},
    {100, 0, 0, -3.6, {943, 0, 0}, {0, -60, -60}},
    {102, 0, 0, -2.5, {1937, 0, 0}, {0, -60, -60}},
    {108, 0, 0, -24, {2078, 0, 0}, {0, -60, -60}},
    {112, 0, 0, -0.7, {1936, 0, 0}, {0, -60, -60}},
    {118, 0, 0, -19.9, {1940, 0, 0}, {0, -60, -60}},
    {124, 0, 0, -0.3, {1930, 0, 0}, {0, -60, -60}},
    {130, 0, 0, -24.1, {1933, 0, 0}, {0, -60, -60}},
    {136, 0, 0, 0, {1915, 0, 0}, {0, -60, -60}},
    {142, 0, 0, -27.1, {1927, 0, 0}, {0, -60, -60}},
    {146, 0, 0, -1.1, {1908, 0, 0}, {0, -60, -60}},
    {152, 0, 0, -17.2, {1928, 0, 0}, {0, -60, -60}},
    {154, 0, 0, -23.9, {3103, 0, 0}, {0, -60, -60}},
    {158, 0, 0, -1.5, {3001, 0, 0}, {0, -60, -60}},
    {164, 0, 0, -21.6, {2974, 0, 0}, {0, -60, -60}},
    {170, 0, 0, -1.3, {2994, 0, 0}, {0, -60, -60}},
    {176, 0, 0, -26.8, {3080, 0, 0}, {0, -60, -60}},
    {180, 0, 0, -3.3, {2986, 0, 0}, {0, -60, -60}},
    {182, 0, 0, -1.5, {2988, 0, 0}, {0, -60, -60}},
    {186, 0, 0, -15.3, {2987, 0, 0}, {0, -60, -60}},
    {188, 0, 0, -60, {2987, 0, 0}, {0, -60, -60}},
    {190, 0, 0, -10.2, {2972, 0, 0}, {0, -60, -60}},
    {192, 0, 0, -2.2, {2988, 0, 0}, {0, -60, -60}},
    {200, 0, 0, -23.7, {3106, 0, 0}, {0, -60, -60}},
    {204, 0, 0, -2, {2990, 0, 0}, {0, -60, -60}},
    {210, 0, 0, -24.7, {2962, 0, 0}, {0, -60, -60}},
    {212, 0, 0, -54.1, {255, 0, 0}, {0, -60, -60}},
    {214, 0, 0, -11, {2982, 0, 0}, {0, -60, -60}},
    {216, 0, 0, -5.2, {2988, 0, 0}, {0, -60, -60}},
    {222, 0, 0, -25.5, {2995, 0, 0}, {0, -60, -60}},
    {224, 0, 0, -55.5, {237, 0, 0}, {0, -60, -60}},
    {226, 0, 0, -56.1, {229, 0, 0}, {0, -60, -60}},
    {228, 0, 0, -16.6, {2982, 0, 0}, {0, -60, -60}},
    {230, 0, 0, -13.3, {2989, 0, 0}, {0, -60, -60}},
    {234, 0, 0, -26.1, {2998, 0, 0}, {0, -60, -60}},
    {236, 0, 0, -60, {2998, 0, 0}, {0, -60, -60}},
    {238, 0, 0, -59, {226, 0, 0}, {0, -60, -60}},
    {249, 0, 0, -60, {226, 0, 0}, {0, -60, -60}},
    /* C6 */
    {5, 0, 0, -60, {233, 478, 583}, {0, -60, -60}},
    {25, 0, 0, -60, {2012, 991, 595}, {0, -14.7, -15.1}},
    {30, 0, 0, -22.1, {2029, 980, 997}, {0, -10.5, -15.7}},
    {35, 0, 0, -13.4, {2041, 227, 595}, {0, -7.5, -9.8}},
    {40, 0, 0, -13, {2029, 227, 1569}, {0, -14.8, -17.3}},
    {55, 0, 0, -8.2, {2023, 227, 1569}, {0, -3.1, -60}},
    {75, 0, 0, -5.2, {2023, 1475, 1125}, {0, -2.6, -60}},
    {90, 0, 0, -1.9, {1738, 1475, 1125}, {0, -0.9, -1.5}},
    {100, 0, 0, -0.6, {1376, 1143, 1125}, {0, -14.2, -14.8}},
    {110, 0, 0, 1.1, {1067, 1143, 2799}, {0, -9.8, -2.4}},
    {130, 0, 0, -1.4, {904, 647, 641}, {0, 0, 0}},
    {135, 0, 0, -2.3, {904, 1942, 641}, {0, 2.7, 0}},
    {140, 0, 0, -2.8, {904, 1942, 910}, {0, 1.8, -7}},
    {145, 0, 0, -1.9, {1930, 1942, 910}, {0, -1.8, -6.2}},
    {150, 0, 0, -1.8, {1936, 1942, 1679}, {0, -9.1, -13.1}},
    {160, 0, 0, -2, {1930, 898, 910}, {0, -5.1, -8.5}},
    {165, 0, 0, -3, {906, 898, 647}, {0, -1.4, -10.3}},
    {185, 0, 0, 1.2, {904, 641, 2782}, {0, -13.7, -5}},
    {200, 0, 0, -0, {1225, 1096, 2898}, {0, -0.5, 7.4}},
    {205, 0, 0, -0.4, {1330, 1073, 1382}, {0, -10.7, -12.7}},
    {240, 0, 0, -6, {2029, 1096, 1195}, {0, -60, -60}},
    {280, 0, 0, -16.1, {2035, 571, 531}, {0, -7.5, -11}},
    {285, 0, 0, -60, {239, 571, 531}, {0, -8, -10.3}},
    {305, 0, 0, -60, {239, 478, 566}, {0, -13, -60}},
    /* C7 is one tone that sweeps up from 500Hz to 2.7kHz and drops back five
     * times in 300ms, faster each time: traced every 2ms */
    {2, 0, 0, -60, {0, 0, 0}, {0, -60, -60}},
    {20, 0, 0, -55.4, {263, 0, 0}, {0, -60, -60}},
    {22, 0, 0, -32.2, {501, 0, 0}, {0, -60, -60}},
    {26, 0, 0, -26, {524, 0, 0}, {0, -60, -60}},
    {30, 0, 0, -42.8, {519, 0, 0}, {0, -60, -60}},
    {32, 0, 0, -57, {182, 0, 0}, {0, -60, -60}},
    {34, 0, 0, -46.2, {202, 0, 0}, {0, -60, -60}},
    {36, 0, 0, -23.9, {488, 0, 0}, {0, -60, -60}},
    {38, 0, 0, -17.8, {514, 0, 0}, {0, -60, -60}},
    {44, 0, 0, -37.8, {520, 0, 0}, {0, -60, -60}},
    {46, 0, 0, -49.7, {160, 0, 0}, {0, -60, -60}},
    {48, 0, 0, -27, {461, 0, 0}, {0, -60, -60}},
    {50, 0, 0, -15.8, {507, 0, 0}, {0, -60, -60}},
    {56, 0, 0, -27.5, {525, 0, 0}, {0, -60, -60}},
    {58, 0, 0, -49.4, {124, 0, 0}, {0, -60, -60}},
    {60, 0, 0, -26.2, {459, 0, 0}, {0, -60, -60}},
    {64, 0, 0, -11.2, {526, 0, 0}, {0, -60, -60}},
    {68, 0, 0, -22.8, {609, 0, 0}, {0, -60, -60}},
    {70, 0, 0, -33.7, {850, 0, 0}, {0, -60, -60}},
    {74, 0, 0, -4.5, {1029, 0, 0}, {0, -60, -60}},
    {82, 0, 0, -24.6, {1368, 0, 0}, {0, -60, -60}},
    {84, 0, 0, -6.3, {1578, 0, 0}, {0, -60, -60}},
    {86, 0, 0, -0.6, {1639, 0, 0}, {0, -60, -60}},
    {92, 0, 0, -21.4, {1841, 0, 0}, {0, -60, -60}},
    {98, 0, 0, -0, {2242, 0, 0}, {0, -60, -60}},
    {104, 0, 0, -25.5, {2433, 0, 0}, {0, -60, -60}},
    {108, 0, 0, -2.8, {2733, 0, 0}, {0, -60, -60}},
    {110, 0, 0, -3.9, {2643, 0, 0}, {0, -60, -60}},
    {116, 0, 0, -32.7, {1529, 0, 0}, {0, -60, -60}},
    {118, 0, 0, -19.8, {542, 0, 0}, {0, -60, -60}},
    {122, 0, 0, -8.8, {678, 0, 0}, {0, -60, -60}},
    {126, 0, 0, -22.9, {825, 0, 0}, {0, -60, -60}},
    {132, 0, 0, -2.5, {1191, 0, 0}, {0, -60, -60}},
    {136, 0, 0, -14.1, {1345, 0, 0}, {0, -60, -60}},
    {140, 0, 0, -1.3, {1630, 0, 0}, {0, -60, -60}},
    {154, 0, 0, -7.7, {2207, 0, 0}, {0, -60, -60}},
    {158, 0, 0, -3.4, {1974, 0, 0}, {0, -60, -60}},
    {162, 0, 0, -15.1, {1571, 0, 0}, {0, -60, -60}},
    {164, 0, 0, -11.2, {588, 0, 0}, {0, -60, -60}},
    {166, 0, 0, -10.1, {576, 0, 0}, {0, -60, -60}},
    {202, 0, 0, -3.4, {2081, 0, 0}, {0, -60, -60}},
    {206, 0, 0, -4.7, {1594, 0, 0}, {0, -60, -60}},
    {208, 0, 0, -12.9, {763, 0, 0}, {0, -60, -60}},
    {212, 0, 0, -10.6, {627, 0, 0}, {0, -60, -60}},
    {220, 0, 0, -6.3, {823, 0, 0}, {0, -60, -60}},
    {224, 0, 0, -8.7, {771, 0, 0}, {0, -60, -60}},
    {234, 0, 0, -6.4, {1030, 0, 0}, {0, -60, -60}},
    {236, 0, 0, -2.6, {1296, 0, 0}, {0, -60, -60}},
    {244, 0, 0, -4.5, {1286, 0, 0}, {0, -60, -60}},
    {246, 0, 0, -1.4, {1557, 0, 0}, {0, -60, -60}},
    {252, 0, 0, -3.6, {1209, 0, 0}, {0, -60, -60}},
    {254, 0, 0, -9.5, {764, 0, 0}, {0, -60, -60}},
    {258, 0, 0, -10.6, {542, 0, 0}, {0, -60, -60}},
    {264, 0, 0, -12.7, {466, 0, 0}, {0, -60, -60}},
    {268, 0, 0, -7.4, {731, 0, 0}, {0, -60, -60}},
    {276, 0, 0, -8, {706, 0, 0}, {0, -60, -60}},
    {280, 0, 0, -4.3, {939, 0, 0}, {0, -60, -60}},
    {288, 0, 0, -4.2, {979, 0, 0}, {0, -60, -60}},
    {290, 0, 0, -4.7, {1246, 0, 0}, {0, -60, -60}},
    {294, 0, 0, -1.2, {1226, 0, 0}, {0, -60, -60}},
    {302, 0, 0, -7.3, {856, 0, 0}, {0, -60, -60}},
    {304, 0, 0, -11.6, {485, 0, 0}, {0, -60, -60}},
    {306, 0, 0, -12.9, {428, 0, 0}, {0, -60, -60}},
    {322, 0, 0, -13.9, {446, 0, 0}, {0, -60, -60}},
    {330, 0, 0, -18.8, {370, 0, 0}, {0, -60, -60}},
    {332, 0, 0, -15.8, {453, 0, 0}, {0, -60, -60}},
    {334, 0, 0, -22.1, {360, 0, 0}, {0, -60, -60}},
    {336, 0, 0, -20.4, {379, 0, 0}, {0, -60, -60}},
    {340, 0, 0, -16.4, {561, 0, 0}, {0, -60, -60}},
    {354, 0, 0, -21.1, {437, 0, 0}, {0, -60, -60}},
    {358, 0, 0, -25, {469, 0, 0}, {0, -60, -60}},
    {360, 0, 0, -24.1, {614, 0, 0}, {0, -60, -60}},
    {362, 0, 0, -22.6, {459, 0, 0}, {0, -60, -60}},
    {372, 0, 0, -25.6, {476, 0, 0}, {0, -60, -60}},
    {376, 0, 0, -39.6, {556, 0, 0}, {0, -60, -60}},
    {382, 0, 0, -32.7, {483, 0, 0}, {0, -60, -60}},
    {388, 0, 0, -60, {558, 0, 0}, {0, -60, -60}},
    {405, 0, 0, -60, {558, 0, 0}, {0, -60, -60}},
    /* M2 */
    {5, 2, 128, -60.0, {540, 1687, 2856}, {0, 37.6, 43.5}},
    {10, 2, 177, -60.0, {540, 1707, 2858}, {0, 38.1, 44.6}},
    {15, 2, 498, -60.0, {540, 1707, 2866}, {0, 4.2, 8.3}},
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
    {550, 2, 492, -60.0, {491, 1710, 2901}, {0, 5.4, 9.9}},
    {555, 2, 492, -60.0, {491, 1707, 2899}, {0, 38.2, 43.8}},
};

/* where each effect's points start in fx_points, and how many there are,
 * for codes 200 to 254. effects without points are built in effect() */
static const struct {
  short first, count;
} fx_index[] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 27}, {0, 0}, {27, 14},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {41, 56}, {97, 35}, {0, 0}, {132, 29}, {161, 23}, {184, 64},
    {0, 0}, {248, 24}, {272, 78}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {350, 16},
};

/* the effects the chip gates: their tones pulse on and off at a steady rate
 * (the envelope oscillator). 0 is not gated. for codes 200 to 254. C8 and C9
 * are gated noise (see c_noise()), and C0, C2, C3, C4 and C6 pulse with the
 * voicing instead (see fx_pulse) */
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
    {0, ENV_SAW}, {0, ENV_SAW}, {66, ENV_SINE}, {0, ENV_SAW},
    {87, ENV_SINE}, {0, ENV_SAW}, {125, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW}, {0, ENV_SAW},
    {0, ENV_SAW}, {0, ENV_SAW},
};
/* FX-GATE-END */

/* the effects that pulse with the voicing: each pulse restarts the tones at
 * full level and fades along voice_fade stretched by fade, the pulses a steady
 * distance (offset_hz) from the voicing frequency, so they follow the Pitch
 * control: C0 4Hz under it, C2 20.5Hz under, C4 on it, C6 42.7Hz over. C3's
 * distance grows along the sound (drift, in Hz a millisecond), so its pulses
 * slow as it plays. R8 and R9 restart at half the frequency of their loudest
 * tone */
static const struct {
  short code;
  double offset_hz, fade, drift;
} fx_pulse[] = {
    {232, -20.5, 1.0, 0},
    {234, 0.2, 1.0, 0},
    {236, 42.7, 1.6, 0},
    {208, 0, 0.7, 0},
    {209, 0, 0.7, 0},
    {233, -13.0, 1.075, 0.14},
    {230, -4.0, 0.6625, 0},
};

/* the entry in fx_pulse for an effect, or -1 */
static int fx_pulse_index(int code) {
  for (int i = 0; i < (int)(sizeof(fx_pulse) / sizeof(fx_pulse[0])); i++)
    if (fx_pulse[i].code == code)
      return i;
  return -1;
}


/* the noise under some of the traced effects, on oscillators 4 and 5: their
 * frequencies, width (distortion), how deep it is gated and its level against
 * the tones. the C series have a broad noise under their tones, and some tone
 * effects a skirt of noise 15 to 30dB under them */
static const struct {
  short code;
  double n1, n2, dist, depth, db;
} fx_noise[] = {
    {230, 2361.6, 2282.7, 27.5, 0.371, -18.86},
    {231, 1810.5, 2003.4, 147.2, 0.324, -1.82},
    {232, 1935.8, 1748.1, 54.8, 0.4, -15.15},
    {234, 1704.5, 3553.6, 228.3, 1.0, -12.64},
    {236, 2357.3, 2947.3, 10.8, 0.0, -9.44},
    {237, 442.9, 1765.9, 199.9, 0.832, -7.05},
    {200, 600.9, 527.1, 135.1, 0, -17.96},
    {201, 1143.9, 3430.0, 31.9, 0, -10.65},
    {202, 1162.4, 1103.3, 73.7, 0, -18.28},
    {204, 1437.6, 1456.3, 28.0, 0, -18.56},
    {205, 2417.5, 2751.8, 44.1, 0, -15.10},
    {206, 1735.5, 2187.7, 10.6, 0, -3.58},
    {210, 419.0, 533.5, 158.7, 0, -8.41},
    {216, 2026.9, 2048.9, 0.2, 0, -13.53},
    {217, 2308.5, 1467.2, 205.5, 0, -23.03},
    {218, 478.3, 2534.5, 114.5, 0, -26.93},
    {219, 2499.3, 2824.7, 50.3, 0, -18.69},
    {220, 686.8, 3002.8, 237.0, 0, -35.74},
    {221, 1303.1, 2945.7, 163.1, 0, -27.86},
    {222, 625.0, 537.8, 121.2, 0, -38.33},
    {225, 388.1, 2892.3, 75.7, 0, -32.91},
    {226, 2473.9, 2831.0, 65.7, 0, -6.71},
    {228, 1613.6, 3166.5, 19.9, 0, -38.21},
    {235, 774.1, 3426.3, 192.9, 0, -22.45},
    {252, 1312.8, 2050.9, 46.6, 0, -9.72},
};

/* the entry in fx_noise for an effect, or -1 */
static int fx_noise_index(int code) {
  for (int i = 0; i < (int)(sizeof(fx_noise) / sizeof(fx_noise[0])); i++)
    if (fx_noise[i].code == code)
      return i;
  return -1;
}

static double voice_hz(double pitch);

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
    } else if (fx_pulse_index(code) >= 0) {
      const int k = fx_pulse_index(code);
      f.pulse_offset = fx_pulse[k].offset_hz - fx_pulse[k].drift * p->ms;
      f.pitch = voice_hz(SJ_DEFAULT_PITCH) + f.pulse_offset;
      f.wave = ENV_VOICE;
      f.voiced = true;
      f.pulsed = true;
      f.fade_scale = fx_pulse[k].fade;
    } else if (fx_gate[code - 200].hz > 0) {
      f.pitch = fx_gate[code - 200].hz;
      f.wave = fx_gate[code - 200].wave;
      f.voiced = true;
    }
  }
  const int i = fx_noise_index(code);
  if (i >= 0) {
    double v = f.vol[0] + f.vol[1] + f.vol[2];
    if (p->kind == FX_NOISE)
      v = f.vol[3] + f.vol[4];
    f.freq[3] = fx_noise[i].n1;
    f.freq[4] = fx_noise[i].n2;
    f.distortion = fx_noise[i].dist;
    f.vol[3] = f.vol[4] = 0.5 * v * pow(10, fx_noise[i].db / 20);
    f.noise_depth = f.pitch > 0 ? fx_noise[i].depth : 0;
  }
  if (p->db <= -60)
    return quiet(f);
  return f;
}

/* contour plays a traced effect: a quick start on the first point, a glide to
 * each of the others in turn, then 3ms to silence. a tone doesn't move while
 * it is silent: it fades out where it was and fades in where it is going,
 * rather than sweeping in from wherever the trace's silent points were */
static void contour(sound *s, const fx_point *p, int n, int code) {
  frame fr[MAX_PARTS];
  for (int i = 0; i < n; i++)
    fr[i] = fx_frame(&p[i], code);
  for (int j = 0; j < 3; j++) {
    bool on[MAX_PARTS];
    for (int i = 0; i < n; i++)
      on[i] = p[i].db > -60 && p[i].freq[j] > 0 && (j == 0 || p[i].rel[j] > -59);
    for (int i = 0; i < n; i++) {
      if (on[i])
        continue;
      if (i > 0 && on[i - 1]) {
        fr[i].freq[j] = fr[i - 1].freq[j];
        continue;
      }
      int k = i + 1;
      while (k < n && !on[k])
        k++;
      if (k < n)
        fr[i].freq[j] = fr[k].freq[j];
      else if (i > 0)
        fr[i].freq[j] = fr[i - 1].freq[j];
    }
  }
  hold(s, p[0].ms, 2, fr[0]);
  for (int i = 1; i < n; i++) {
    const double ms = p[i].ms - p[i - 1].ms;
    hold(s, ms, ms, fr[i]);
  }
  hold(s, 3, 3, quiet(fr[n - 1]));
}

/* C8 and C9 are noise from about 400Hz to 2.8kHz, gated by a saw (see
 * noise_depth) about 15dB deep every 19ms: oscillators 4 and 5 spread wide,
 * each random value held 0.75ms (a band with edges, as on the chip), at these
 * levels in dB every 5ms. the gate keeps time with Speed, not Pitch */
static const double c8_db[] = {
    -16.2, -11.8, -9.5, -5.5, -1.1, -0.5, -0.4, -0.3, -0.1, -0.3,
    -0.2, -0.1, -0.3, -0.5, -0.5, -0.2, -0.9, -1.3, -1.8, -5.2,
};
static const double c9_db[] = {
    -18.9, -13.6, -11.4, -7.8, -3.1, -1.8, -1.6, -1.1, -0.8,
    -0.7, -0.5, -0.0, -0.5, -1.2, -1.8, -4.7, -16.4,
};

static void c_noise(sound *s, const double *db, int n) {
  s->speed_gate = true;
  frame f = noise_frame(lGlottal, 1416.3, 1905.6, 0, 0, 100);
  f.noise_hold_ms = 0.75;
  f.pitch = 52;
  f.wave = ENV_SAW;
  f.noise_depth = 0.826;
  for (int i = 0; i < n; i++) {
    f.vol[3] = f.vol[4] = 20 * pow(10, db[i] / 20);
    hold(s, 5, i == 0 ? 2 : 5, f);
  }
  f.vol[3] = f.vol[4] = 0;
  hold(s, 3, 3, f);
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


/* FX2-BEGIN */
/* the effects that are pure tones, traced from both units every millisecond,
 * the levels with the output filter's loss added back, each effect scaled so
 * its loudest point is at the oscillators' limit. each tone has an oscillator
 * of its own and is kept as its knots: its time from the start of the sound in
 * tenths of a millisecond, its frequency in Hz and its volume in hundredths,
 * glided between in straight lines (only the knots a straight line can't pass
 * close enough to are kept). before its first knot a tone sits silent at its
 * first frequency */
static const short fx_knots[] = {
    10, 794, 0, 50, 794, 0, 60, 794, 252, 170, 803, 3100, 650, 802, 3011, 780, 809, 641,
    790, 809, 0, 820, 809, 0, 10, 409, 0, 170, 409, 0, 180, 409, 408, 290, 450, 700,
    530, 539, 844, 580, 570, 1056, 590, 570, 0, 820, 570, 0, 10, 635, 0, 30, 635, 0,
    40, 635, 37, 50, 681, 79, 80, 681, 169, 120, 686, 174, 160, 660, 247, 200, 699, 324,
    280, 698, 347, 350, 733, 341, 500, 735, 335, 580, 776, 342, 670, 767, 327, 690, 715, 266,
    720, 701, 233, 760, 744, 257, 780, 768, 145, 790, 768, 0, 860, 768, 0, 10, 1105, 0,
    60, 1105, 0, 70, 1105, 209, 80, 1105, 331, 110, 1105, 1319, 160, 1105, 2886, 650, 1105, 3100,
    780, 1107, 362, 790, 1107, 254, 820, 1107, 37, 860, 1107, 0, 10, 1300, 0, 40, 1300, 18,
    60, 1300, 101, 70, 1300, 244, 80, 1300, 418, 90, 1300, 715, 120, 1301, 1865, 160, 1304, 3078,
    670, 1304, 3100, 720, 1302, 2124, 750, 1304, 1180, 760, 1305, 970, 790, 1306, 539, 800, 1305, 711,
    810, 1305, 0, 950, 1305, 0, 10, 733, 0, 60, 733, 0, 70, 733, 103, 80, 653, 75,
    120, 659, 241, 160, 685, 308, 700, 375, 355, 730, 377, 324, 750, 415, 259, 760, 418, 239,
    800, 376, 257, 830, 412, 223, 900, 421, 24, 910, 420, 60, 920, 420, 0, 950, 420, 0,
    10, 1697, 0, 150, 1697, 0, 160, 1697, 367, 200, 1667, 518, 240, 1606, 690, 250, 1606, 0,
    950, 1606, 0, 10, 1605, 0, 30, 1605, 0, 40, 1605, 95, 70, 1605, 451, 130, 1605, 2438,
    660, 1606, 2828, 800, 1607, 109, 810, 1606, 135, 820, 1606, 0, 960, 1606, 0, 10, 636, 0,
    30, 636, 0, 200, 694, 736, 570, 771, 320, 960, 706, 0, 10, 345, 0, 170, 345, 0,
    180, 345, 121, 650, 367, 436, 730, 296, 177, 820, 299, 62, 830, 299, 0, 960, 299, 0,
    10, 1901, 13, 30, 1901, 75, 40, 1901, 183, 50, 1901, 334, 140, 1906, 3100, 650, 1907, 2920,
    790, 1905, 130, 800, 1905, 0, 830, 1905, 0, 10, 629, 0, 30, 629, 0, 40, 629, 59,
    60, 603, 85, 90, 588, 312, 110, 555, 390, 140, 549, 425, 170, 629, 475, 390, 863, 217,
    540, 863, 106, 590, 857, 0, 830, 857, 0, 10, 462, 0, 210, 462, 0, 220, 462, 521,
    230, 463, 402, 590, 719, 358, 640, 737, 394, 680, 702, 307, 720, 728, 292, 730, 728, 0,
    830, 728, 0, 10, 282, 0, 330, 282, 0, 340, 282, 197, 370, 279, 222, 470, 314, 503,
    630, 367, 452, 680, 365, 408, 690, 373, 355, 700, 441, 373, 710, 441, 0, 830, 441, 0,
    10, 1476, 0, 370, 1476, 0, 380, 1476, 177, 500, 1634, 298, 510, 1634, 0, 830, 1634, 0,
    10, 347, 12, 50, 347, 75, 70, 347, 185, 100, 307, 258, 120, 302, 291, 130, 310, 306,
    150, 359, 372, 170, 379, 426, 640, 382, 419, 690, 349, 399, 760, 431, 419, 800, 492, 206,
    830, 492, 108, 850, 496, 103, 860, 496, 0, 890, 496, 0, 10, 2206, 0, 30, 2206, 43,
    50, 2206, 236, 70, 2206, 625, 120, 2207, 2483, 670, 2208, 2506, 690, 2208, 1790, 770, 2207, 271,
    790, 2208, 97, 830, 2209, 0, 890, 2209, 0, 10, 810, 0, 90, 810, 0, 100, 810, 346,
    170, 769, 414, 640, 757, 416, 670, 740, 404, 680, 812, 0, 690, 812, 171, 720, 831, 212,
    760, 787, 218, 790, 736, 175, 800, 736, 0, 890, 736, 0, 10, 1668, 0, 40, 1668, 58,
    80, 1646, 291, 130, 1489, 217, 140, 1707, 0, 250, 1707, 0, 260, 1707, 200, 340, 1643, 426,
    460, 1482, 312, 470, 542, 0, 680, 542, 0, 690, 542, 579, 750, 518, 484, 780, 469, 279,
    810, 442, 416, 850, 504, 335, 860, 504, 0, 930, 504, 0, 10, 477, 0, 30, 477, 0,
    40, 477, 139, 70, 471, 414, 110, 462, 526, 140, 377, 578, 160, 358, 695, 220, 374, 720,
    400, 307, 669, 520, 287, 199, 530, 1622, 0, 690, 1622, 0, 700, 1622, 346, 760, 1670, 263,
    830, 1688, 287, 860, 1681, 119, 900, 1678, 0, 930, 1678, 0, 10, 2511, 0, 40, 2511, 0,
    50, 2511, 75, 70, 2511, 326, 80, 2511, 529, 130, 2510, 3084, 700, 2509, 3100, 780, 2508, 677,
    810, 2508, 172, 830, 2506, 50, 840, 2506, 0, 930, 2506, 0, 10, 760, 0, 130, 760, 0,
    140, 760, 320, 150, 713, 466, 220, 735, 605, 610, 466, 794, 620, 466, 0, 930, 466, 0,
    10, 878, 0, 250, 878, 0, 260, 878, 106, 440, 869, 332, 640, 643, 682, 680, 625, 746,
    690, 625, 0, 930, 625, 0, 10, 2805, 0, 20, 2805, 0, 30, 2805, 73, 40, 2805, 160,
    110, 2809, 2416, 640, 2809, 2959, 750, 2810, 485, 790, 2811, 0, 970, 2811, 0, 10, 593, 0,
    30, 593, 0, 40, 593, 112, 70, 565, 345, 90, 492, 334, 120, 475, 367, 130, 493, 486,
    140, 1002, 0, 150, 1002, 0, 160, 1002, 333, 280, 1133, 143, 290, 260, 0, 380, 260, 0,
    390, 260, 188, 670, 287, 444, 750, 403, 472, 820, 409, 97, 970, 410, 0, 10, 639, 0,
    100, 639, 0, 110, 639, 299, 180, 596, 566, 340, 697, 334, 350, 761, 0, 400, 761, 0,
    410, 761, 183, 500, 780, 128, 510, 1077, 0, 670, 1077, 0, 680, 1077, 255, 770, 1138, 282,
    820, 1141, 71, 880, 1141, 78, 910, 1144, 0, 970, 1144, 0, 10, 424, 0, 200, 424, 0,
    210, 424, 419, 610, 591, 366, 710, 572, 346, 720, 572, 0, 970, 572, 0, 10, 901, 0,
    270, 901, 0, 280, 901, 283, 640, 1196, 383, 700, 1273, 139, 710, 1273, 0, 970, 1273, 0,
    10, 728, 0, 20, 728, 0, 30, 728, 89, 60, 706, 540, 120, 756, 1200, 130, 2783, 0,
    140, 2783, 411, 170, 2770, 583, 230, 2655, 627, 310, 2660, 270, 350, 2663, 177, 360, 2670, 222,
    370, 1910, 0, 420, 1910, 0, 430, 1910, 85, 480, 1905, 225, 510, 1903, 402, 660, 1828, 599,
    720, 1792, 456, 730, 1730, 385, 740, 689, 0, 747, 689, 0, 757, 689, 773, 784, 682, 768,
    801, 624, 798, 811, 614, 780, 836, 616, 365, 883, 619, 0, 913, 619, 0, 10, 575, 0,
    110, 575, 0, 120, 575, 903, 140, 621, 1143, 200, 679, 1670, 670, 614, 1670, 720, 540, 1353,
    762, 541, 1112, 772, 574, 1749, 779, 574, 0, 913, 574, 0, 10, 3111, 0, 50, 3111, 0,
    60, 3111, 175, 110, 3111, 1417, 160, 3110, 2868, 690, 3112, 2571, 757, 3111, 925, 801, 3111, 215,
    811, 3111, 129, 826, 3112, 126, 836, 3112, 0, 913, 3112, 0, 10, 2311, 0, 190, 2311, 0,
    200, 2311, 107, 270, 2317, 180, 340, 2316, 399, 410, 2228, 386, 540, 2220, 193, 550, 2211, 242,
    560, 2211, 0, 913, 2211, 0, 10, 680, 0, 20, 680, 0, 40, 678, 202, 70, 682, 622,
    100, 689, 906, 150, 651, 1385, 190, 678, 1498, 670, 612, 1436, 710, 534, 1169, 730, 531, 1075,
    764, 526, 946, 774, 545, 1372, 781, 545, 0, 917, 545, 0, 10, 1693, 0, 40, 1693, 0,
    50, 1693, 130, 70, 1710, 276, 120, 1800, 400, 130, 1677, 0, 160, 1677, 0, 170, 1677, 326,
    230, 1667, 526, 340, 1668, 518, 400, 1605, 516, 520, 1602, 497, 600, 1538, 547, 730, 1548, 412,
    752, 1512, 366, 798, 1587, 323, 805, 1587, 0, 917, 1587, 0, 10, 3412, 0, 40, 3412, 0,
    50, 3412, 150, 70, 3412, 524, 90, 3412, 1187, 170, 3413, 4266, 660, 3413, 4074, 759, 3411, 1025,
    781, 3410, 620, 798, 3410, 451, 805, 3410, 0, 917, 3410, 0, 10, 334, 0, 180, 334, 0,
    190, 334, 260, 210, 334, 149, 220, 350, 143, 250, 348, 129, 260, 328, 119, 280, 347, 107,
    290, 324, 101, 310, 344, 97, 320, 327, 98, 340, 360, 108, 350, 331, 100, 380, 300, 110,
    410, 312, 149, 430, 340, 184, 440, 323, 195, 450, 688, 0, 752, 688, 0, 759, 688, 621,
    786, 684, 636, 805, 630, 660, 817, 618, 658, 887, 628, 0, 917, 628, 0, 10, 1615, 0,
    120, 1615, 0, 130, 1615, 571, 2040, 2653, 443, 2050, 2574, 512, 2060, 415, 0, 3170, 415, 0,
    3180, 415, 1070, 3220, 437, 1205, 3350, 385, 616, 3370, 327, 631, 3380, 327, 0, 3410, 327, 0,
    10, 305, 0, 320, 305, 0, 330, 305, 192, 340, 301, 132, 400, 294, 535, 470, 215, 289,
    520, 281, 605, 590, 242, 504, 650, 283, 802, 700, 262, 660, 1120, 340, 840, 1170, 314, 927,
    1210, 360, 816, 1270, 269, 845, 1280, 274, 749, 1290, 361, 921, 1300, 224, 0, 1360, 224, 0,
    1370, 224, 784, 1380, 266, 866, 1410, 291, 1242, 1460, 236, 1142, 1500, 280, 1538, 1540, 247, 1421,
    1580, 279, 1769, 1690, 263, 1830, 2930, 384, 3100, 3160, 305, 1694, 3170, 305, 0, 3410, 305, 0,
    10, 247, 37, 110, 247, 1050, 140, 247, 2036, 210, 247, 1965, 320, 261, 3100, 930, 496, 3100,
    970, 534, 3100, 1451, 1122, 2799, 1611, 1712, 2041, 1811, 2464, 1874, 1851, 2689, 1439, 1971, 3578, 1041,
    2011, 3878, 1859, 2051, 3911, 2295, 2091, 3913, 2153, 2491, 3913, 417, 2501, 3913, 0, 2531, 3913, 0,
    10, 644, 0, 30, 644, 0, 40, 644, 167, 50, 641, 167, 90, 641, 1267, 130, 636, 1894,
    1890, 405, 1913, 1920, 417, 1890, 1950, 389, 1980, 1990, 413, 1827, 2030, 358, 2181, 2060, 409, 1766,
    2070, 409, 1747, 2100, 345, 2417, 2110, 338, 2584, 2120, 341, 2461, 2160, 409, 1578, 2190, 341, 2388,
    2240, 336, 2487, 2265, 377, 1581, 2273, 410, 1524, 2278, 410, 0, 5590, 410, 0, 10, 333, 0,
    2273, 333, 0, 2278, 333, 1062, 2298, 333, 913, 2398, 331, 3100, 2568, 333, 631, 2578, 393, 461,
    2588, 409, 421, 2608, 390, 541, 2618, 333, 796, 2678, 331, 2645, 2758, 334, 1697, 2768, 384, 1230,
    2788, 411, 1039, 2798, 401, 1137, 2818, 333, 1958, 2858, 333, 2472, 2868, 335, 2098, 2888, 405, 1364,
    2898, 413, 1307, 2918, 384, 1535, 2928, 338, 1951, 2948, 335, 2271, 2958, 342, 2017, 2968, 386, 1649,
    2988, 414, 1460, 3008, 388, 1625, 3018, 350, 1884, 3028, 348, 1963, 3058, 414, 1563, 3078, 408, 1585,
    3098, 376, 1751, 3128, 418, 1615, 3158, 403, 1628, 3198, 423, 1621, 5002, 628, 1444, 5510, 628, 64,
    5590, 628, 0, 10, 308, 22, 70, 308, 171, 110, 308, 1200, 150, 457, 2815, 570, 1649, 1859,
    1270, 1649, 72, 1280, 1649, 0, 1970, 1649, 0, 10, 262, 0, 160, 262, 0, 170, 262, 2141,
    240, 304, 3057, 710, 799, 3100, 1650, 804, 1509, 1900, 804, 135, 1940, 804, 0, 1970, 804, 0,
    10, 1145, 0, 20, 1145, 0, 30, 1145, 67, 120, 1156, 2309, 1540, 1462, 1032, 2400, 1463, 226,
    3000, 1463, 111, 3460, 1463, 73, 4120, 1456, 85, 4130, 1456, 0, 5900, 1456, 0, 10, 418, 0,
    110, 418, 0, 120, 418, 48, 150, 418, 50, 940, 418, 1177, 2390, 515, 3093, 4770, 867, 2850,
    5650, 928, 203, 5750, 928, 0, 5900, 928, 0, 10, 876, 0, 140, 876, 0, 150, 876, 48,
    850, 874, 901, 2450, 1229, 2465, 4770, 1748, 2190, 5870, 1815, 0, 5900, 1815, 0, 10, 348, 16,
    70, 348, 98, 200, 348, 1080, 640, 999, 3100, 1300, 1003, 33, 1310, 284, 0, 1620, 284, 0,
    1730, 280, 400, 1800, 280, 1172, 2100, 498, 2966, 2690, 502, 39, 2760, 502, 0, 10, 289, 0,
    290, 289, 0, 300, 289, 1106, 350, 290, 1380, 650, 498, 2922, 1230, 501, 119, 1240, 544, 0,
    1460, 544, 11, 1610, 544, 553, 2070, 1966, 2370, 2680, 2008, 52, 2760, 2008, 0, 10, 1303, 0,
    20, 1303, 0, 40, 1302, 179, 50, 1302, 362, 100, 1301, 1890, 560, 1295, 2068, 2130, 304, 3100,
    2250, 252, 1216, 2400, 251, 0, 2560, 251, 0, 10, 2431, 0, 30, 2431, 0, 40, 2431, 129,
    110, 2449, 1676, 810, 2431, 1913, 2420, 1334, 2296, 2490, 1308, 393, 2520, 1308, 46, 2530, 1308, 0,
    2560, 1308, 0, 10, 2603, 0, 20, 2603, 0, 30, 2603, 102, 110, 2606, 3076, 1150, 2607, 3100,
    1740, 2605, 31, 1780, 2605, 0, 10, 1421, 0, 30, 1421, 0, 40, 1421, 169, 80, 1416, 1565,
    130, 1473, 2909, 1490, 3837, 891, 1530, 3879, 996, 1570, 3822, 875, 2630, 1546, 2752, 2700, 1418, 3074,
    3870, 1407, 51, 3940, 1407, 0, 10, 1406, 0, 30, 1406, 0, 40, 1406, 117, 50, 1406, 334,
    120, 1412, 3032, 1220, 1414, 3018, 1270, 1562, 1652, 1280, 1869, 0, 1310, 1869, 0, 1320, 1869, 2459,
    1390, 2266, 2525, 1400, 2395, 0, 1410, 2395, 2678, 1460, 2507, 3023, 1590, 2508, 3007, 1630, 2461, 1731,
    1640, 1767, 0, 1650, 1767, 1698, 1710, 1707, 2906, 2820, 1707, 2887, 3920, 1708, 118, 3990, 1709, 10,
    4030, 1709, 0, 10, 260, 0, 2040, 260, 0, 2070, 257, 32, 2160, 259, 251, 2230, 298, 509,
    2610, 601, 510, 3920, 600, 0, 4030, 600, 0, 10, 3103, 0, 20, 3103, 0, 30, 3103, 130,
    120, 3114, 2135, 1100, 3112, 2072, 1150, 3076, 788, 1170, 3075, 154, 1180, 3075, 0, 3100, 3075, 0,
    10, 615, 0, 30, 615, 0, 40, 615, 130, 120, 596, 815, 2280, 602, 815, 3020, 602, 0,
    3100, 602, 0, 10, 1416, 0, 1100, 1416, 0, 1110, 1416, 67, 1140, 1422, 619, 1200, 1404, 2994,
    2310, 1404, 3082, 3070, 1404, 0, 3100, 1404, 0, 10, 1828, 0, 20, 1828, 0, 30, 1828, 216,
    70, 1828, 1632, 140, 1855, 3020, 230, 1835, 1678, 310, 1805, 3100, 360, 1841, 2495, 2730, 1805, 2951,
    2780, 1853, 1682, 2860, 1807, 1498, 2920, 1858, 3074, 3690, 1834, 166, 3740, 1834, 29, 3790, 1839, 0,
    10, 1620, 0, 400, 1620, 0, 410, 1620, 1524, 1470, 357, 2167, 1520, 325, 2115, 1560, 353, 2163,
    2600, 1574, 1487, 2640, 1617, 1919, 2650, 1617, 0, 3790, 1617, 0, 10, 982, 0, 30, 982, 0,
    50, 981, 277, 120, 965, 2565, 780, 320, 3100, 870, 260, 1375, 1030, 258, 33, 1040, 258, 0,
    1660, 258, 0, 10, 3508, 0, 30, 3508, 0, 40, 3508, 56, 50, 3502, 212, 130, 3511, 1656,
    1100, 3514, 1599, 1620, 3517, 132, 1630, 3517, 0, 1660, 3517, 0, 10, 3503, 0, 20, 3503, 0,
    30, 3503, 141, 90, 3507, 2297, 270, 3452, 1305, 280, 3493, 0, 480, 3493, 0, 490, 3493, 1154,
    610, 3513, 2647, 680, 3491, 949, 690, 3469, 0, 820, 3469, 0, 830, 3469, 1274, 890, 3509, 2586,
    940, 3463, 1313, 950, 3463, 0, 1710, 3463, 0, 10, 1821, 0, 230, 1821, 0, 240, 1821, 101,
    260, 1821, 447, 310, 1805, 2904, 460, 1804, 3086, 510, 1803, 1026, 520, 1803, 0, 610, 1803, 0,
    620, 1803, 47, 630, 1802, 162, 640, 1802, 361, 700, 1803, 3096, 800, 1809, 2578, 850, 1823, 298,
    870, 1822, 0, 900, 1822, 0, 910, 1822, 117, 930, 1822, 496, 970, 1808, 2565, 1120, 1804, 3100,
    1620, 1805, 203, 1670, 1805, 33, 1710, 1805, 0, 10, 2908, 0, 20, 2908, 0, 30, 2908, 174,
    60, 2910, 1164, 100, 2914, 2900, 410, 2912, 3100, 440, 2910, 2556, 490, 2907, 434, 510, 2907, 70,
    520, 3884, 0, 610, 3884, 0, 620, 3884, 115, 630, 3903, 460, 660, 3897, 1353, 730, 3918, 2813,
    1060, 3916, 2742, 1350, 3918, 1385, 1590, 3917, 202, 1600, 3917, 0, 1630, 3917, 0, 10, 2085, 0,
    20, 2085, 0, 30, 2085, 84, 40, 2086, 240, 110, 2082, 2804, 660, 1752, 2853, 1250, 1501, 3015,
    2010, 1330, 3100, 2740, 1305, 2417, 3150, 1305, 124, 3170, 1305, 49, 3220, 1306, 0, 10, 3618, 0,
    350, 3618, 0, 360, 3618, 455, 410, 3621, 1401, 580, 3619, 2237, 1240, 3005, 2894, 2300, 2611, 2451,
    2690, 2609, 2454, 2840, 2611, 1771, 3150, 2612, 97, 3170, 2613, 82, 3180, 2613, 0, 3220, 2613, 0,
    10, 716, 0, 85, 716, 0, 95, 716, 128, 150, 716, 268, 508, 716, 266, 658, 716, 948,
    939, 597, 2179, 1302, 586, 2165, 1597, 472, 2136, 1627, 421, 2188, 1647, 453, 1985, 1672, 441, 1909,
    1682, 339, 0, 1767, 339, 0, 1777, 339, 2131, 1832, 332, 3100, 1882, 324, 1153, 1888, 406, 0,
    1893, 406, 0, 1899, 406, 1757, 1909, 357, 1011, 1929, 318, 1300, 2009, 302, 2532, 2049, 323, 2383,
    2109, 289, 2322, 2259, 299, 2174, 2329, 305, 852, 2429, 305, 457, 2489, 305, 687, 2549, 305, 471,
    2609, 305, 680, 2659, 305, 467, 2719, 305, 679, 2779, 305, 466, 2829, 305, 679, 2889, 305, 473,
    2949, 305, 688, 3009, 305, 473, 3069, 305, 677, 3119, 305, 463, 3179, 305, 670, 3239, 305, 399,
    3299, 305, 405, 3379, 305, 68, 3389, 316, 123, 3399, 316, 0, 3429, 316, 0, 10, 2705, 0,
    95, 2705, 0, 100, 2705, 805, 110, 2708, 389, 180, 2710, 1278, 540, 2668, 1062, 550, 2708, 1595,
    560, 901, 0, 807, 901, 0, 812, 901, 1306, 912, 903, 2083, 1052, 902, 1799, 1288, 898, 307,
    1302, 898, 268, 1312, 899, 361, 1322, 281, 0, 1652, 281, 0, 1662, 281, 1755, 1692, 361, 2622,
    1712, 360, 2545, 1732, 417, 1680, 1752, 424, 1500, 1767, 406, 1628, 1777, 406, 0, 3429, 406, 0,
    10, 302, 0, 862, 302, 0, 872, 302, 254, 939, 301, 493, 1112, 299, 1822, 1288, 300, 2004,
    1482, 300, 2228, 1492, 300, 0, 3429, 300, 0, 10, 965, 0, 30, 965, 0, 40, 965, 318,
    50, 968, 764, 70, 963, 1775, 110, 950, 3100, 1010, 950, 3065, 1190, 950, 1788, 1420, 949, 899,
    1480, 949, 535, 1540, 949, 236, 1570, 949, 98, 1590, 950, 79, 1600, 950, 0, 1640, 950, 0,
    10, 1334, 0, 70, 1334, 0, 80, 1334, 1754, 140, 1346, 2730, 980, 1341, 2711, 1190, 1342, 1853,
    1420, 1342, 641, 1480, 1342, 486, 1510, 1342, 302, 1550, 1342, 136, 1590, 1342, 26, 1640, 1344, 0,
    10, 690, 0, 40, 690, 0, 50, 690, 377, 70, 697, 878, 90, 705, 2040, 130, 707, 3100,
    1030, 701, 3046, 1160, 701, 2016, 1360, 701, 1069, 1440, 701, 829, 1550, 701, 209, 1610, 700, 0,
    1640, 700, 0, 10, 1231, 0, 70, 1231, 0, 80, 1231, 1247, 90, 1213, 1727, 130, 1214, 2589,
    1030, 1215, 2047, 1160, 1215, 1978, 1530, 1215, 250, 1600, 1215, 0, 1640, 1215, 0, 10, 714, 0,
    40, 714, 0, 50, 714, 328, 60, 693, 411, 130, 704, 3098, 1040, 701, 3100, 1590, 701, 57,
    1620, 701, 0, 1660, 701, 0, 10, 1350, 0, 60, 1350, 0, 90, 1343, 1405, 140, 1347, 2533,
    1110, 1341, 2163, 1500, 1341, 426, 1590, 1341, 48, 1660, 1342, 0, 10, 699, 0, 40, 699, 0,
    60, 699, 424, 70, 699, 783, 80, 699, 1447, 130, 706, 3100, 1020, 701, 3055, 1550, 701, 203,
    1610, 701, 0, 1640, 701, 0, 10, 1489, 0, 60, 1489, 0, 70, 1489, 1019, 130, 1479, 2366,
    1110, 1480, 2004, 1540, 1480, 185, 1600, 1480, 0, 1640, 1480, 0, 10, 794, 0, 40, 794, 0,
    80, 780, 1568, 130, 773, 3098, 1000, 773, 3100, 1590, 773, 48, 1650, 773, 0, 10, 1202, 0,
    80, 1202, 0, 90, 1202, 1839, 1150, 1215, 2036, 1460, 1215, 603, 1590, 1215, 0, 1650, 1215, 0,
    10, 765, 0, 40, 765, 0, 50, 765, 221, 70, 765, 772, 80, 765, 1441, 130, 772, 3100,
    1030, 773, 3052, 1590, 772, 54, 1610, 772, 0, 1640, 772, 0, 10, 1354, 0, 70, 1354, 0,
    80, 1354, 1460, 140, 1346, 2540, 1150, 1341, 1980, 1280, 1341, 1177, 1540, 1341, 233, 1600, 1342, 0,
    1640, 1342, 0, 10, 780, 0, 40, 780, 0, 50, 780, 449, 60, 758, 503, 70, 758, 782,
    90, 757, 1893, 130, 771, 3100, 1040, 773, 3028, 1320, 773, 1210, 1450, 773, 791, 1550, 773, 227,
    1610, 773, 0, 1640, 773, 0, 10, 1476, 0, 60, 1476, 0, 70, 1476, 424, 80, 1470, 1089,
    130, 1479, 2440, 1050, 1480, 2332, 1540, 1481, 214, 1610, 1480, 0, 1640, 1480, 0, 10, 895, 0,
    30, 895, 0, 40, 895, 577, 50, 862, 445, 130, 861, 3100, 1080, 856, 2759, 1570, 855, 73,
    1600, 856, 0, 1630, 856, 0, 10, 1220, 0, 70, 1220, 0, 90, 1212, 1894, 130, 1214, 2718,
    1050, 1214, 2570, 1580, 1215, 0, 1630, 1215, 0, 10, 878, 0, 30, 878, 0, 50, 853, 307,
    130, 860, 3100, 1030, 856, 3044, 1590, 854, 47, 1640, 854, 0, 10, 1360, 0, 70, 1360, 0,
    80, 1360, 853, 130, 1346, 2607, 1070, 1342, 2380, 1590, 1342, 41, 1600, 1342, 0, 1640, 1342, 0,
    10, 865, 0, 40, 865, 0, 90, 850, 2007, 1050, 856, 2991, 1600, 856, 49, 1650, 856, 0,
    10, 1486, 0, 60, 1486, 0, 70, 1486, 737, 130, 1480, 2501, 1070, 1480, 2337, 1610, 1483, 0,
    1650, 1483, 0, 10, 936, 0, 40, 936, 0, 50, 936, 507, 130, 950, 3100, 1010, 949, 3065,
    1570, 948, 93, 1580, 949, 111, 1590, 949, 0, 1620, 949, 0, 10, 1211, 0, 100, 1211, 0,
    110, 1211, 2174, 140, 1215, 2827, 1010, 1215, 2786, 1500, 1216, 353, 1520, 1216, 301, 1530, 1217, 375,
    1540, 1217, 0, 1620, 1217, 0, 10, 953, 0, 40, 953, 0, 50, 953, 848, 130, 949, 3100,
    1050, 950, 2899, 1560, 949, 124, 1600, 949, 0, 1640, 949, 0, 10, 1470, 0, 70, 1470, 0,
    80, 1470, 1530, 990, 1479, 2579, 1590, 1478, 29, 1640, 1478, 0, 10, 1732, 0, 50, 1732, 0,
    60, 1732, 3100, 70, 1784, 1971, 100, 1810, 3100, 2570, 1782, 3100, 2680, 1844, 3100, 3360, 1782, 3100,
    8590, 1783, 428, 9720, 1783, 357, 9850, 1783, 244, 10920, 1783, 253, 11040, 1783, 133, 11880, 1783, 130,
    11950, 1783, 41, 11960, 1794, 70, 11970, 1794, 0, 12000, 1794, 0,
};

/* each tone: its effect, its oscillator, where its knots start in fx_knots
 * (in knots) and how many */
static const struct {
  short code, osc, first, count;
} fx_tones[] = {
    {200, 0, 0, 8}, {200, 1, 8, 8}, {201, 0, 16, 19}, {201, 1, 35, 11},
    {202, 0, 46, 16}, {202, 1, 62, 16}, {202, 2, 78, 7}, {203, 0, 85, 10},
    {203, 1, 95, 5}, {203, 2, 100, 8}, {204, 0, 108, 9}, {204, 1, 117, 12},
    {204, 2, 129, 10}, {204, 3, 139, 11}, {204, 4, 150, 6}, {205, 0, 156, 16},
    {205, 1, 172, 11}, {205, 2, 183, 13}, {206, 0, 196, 18}, {206, 1, 214, 18},
    {206, 2, 232, 12}, {206, 3, 244, 8}, {206, 4, 252, 8}, {207, 0, 260, 9},
    {207, 1, 269, 18}, {207, 2, 287, 17}, {207, 3, 304, 7}, {207, 4, 311, 7},
    {208, 0, 318, 29}, {208, 2, 347, 11}, {208, 3, 358, 12}, {208, 4, 370, 10},
    {209, 0, 380, 14}, {209, 1, 394, 18}, {209, 3, 412, 12}, {209, 4, 424, 25},
    {210, 0, 449, 13}, {210, 1, 462, 30}, {214, 0, 492, 18}, {215, 0, 510, 23},
    {215, 1, 533, 38}, {216, 0, 571, 8}, {216, 1, 579, 9}, {217, 0, 588, 11},
    {217, 1, 599, 10}, {217, 2, 609, 8}, {218, 0, 617, 12}, {218, 1, 629, 12},
    {219, 0, 641, 10}, {219, 1, 651, 10}, {220, 0, 661, 7}, {221, 0, 668, 12},
    {222, 0, 680, 23}, {222, 1, 703, 8}, {223, 0, 711, 9}, {223, 1, 720, 7},
    {223, 2, 727, 8}, {224, 0, 735, 15}, {224, 1, 750, 10}, {225, 0, 760, 9},
    {225, 1, 769, 9}, {226, 0, 778, 17}, {226, 1, 795, 24}, {227, 0, 819, 20},
    {228, 0, 839, 12}, {228, 1, 851, 13}, {235, 0, 864, 47}, {235, 1, 911, 25},
    {235, 2, 936, 9}, {240, 0, 945, 15}, {240, 1, 960, 12}, {241, 0, 972, 13},
    {241, 1, 985, 10}, {242, 0, 995, 9}, {242, 1, 1004, 8}, {243, 0, 1012, 10},
    {243, 1, 1022, 8}, {244, 0, 1030, 7}, {244, 1, 1037, 7}, {245, 0, 1044, 10},
    {245, 1, 1054, 9}, {246, 0, 1063, 13}, {246, 1, 1076, 9}, {247, 0, 1085, 9},
    {247, 1, 1094, 7}, {248, 0, 1101, 7}, {248, 1, 1108, 8}, {249, 0, 1116, 6},
    {249, 1, 1122, 7}, {250, 0, 1129, 9}, {250, 1, 1138, 10}, {251, 0, 1148, 8},
    {251, 1, 1156, 6}, {252, 0, 1162, 18},
};

typedef struct fx2_point {
  float ms;
  float freq[5];
  float vol[5];
} fx2_point;

/* A4 is not a sine: its tone carries its third harmonic, exactly three times
 * its frequency, 10 to 14dB under it, gone as it nears 4kHz. it plays on
 * oscillator 2 */
static const struct {
  short code;
  double db;
} third_harmonics[] = {
    {214, -14.0}, /* A4 */
};

static void contour2(sound *s, const fx2_point *p, int n, int code) {
  double third = 0;
  for (int k = 0; k < (int)(sizeof(third_harmonics) / sizeof(third_harmonics[0])); k++)
    if (third_harmonics[k].code == code)
      third = pow(10, third_harmonics[k].db / 20);
  const int k = fx_noise_index(code);
  const int q = fx_pulse_index(code);
  for (int i = 0; i < n; i++) {
    frame f;
    memset(&f, 0, sizeof(f));
    for (int j = 0; j < 5; j++) {
      f.freq[j] = p[i].freq[j];
      f.vol[j] = p[i].vol[j];
    }
    /* the third harmonic (see third_harmonics), held still and silent once it
     * would pass 3990Hz */
    if (third > 0) {
      const bool on = 3 * f.freq[0] < 3990;
      f.freq[1] = on ? 3 * f.freq[0] : 3990;
      f.vol[1] = on ? f.vol[0] * third : 0;
    }
    /* the noise under the tones (see fx_noise), on oscillators 4 and 5 when
     * the effect has no tone on them */
    if (k >= 0 && q < 0 && f.vol[3] == 0 && f.vol[4] == 0) {
      f.freq[3] = fx_noise[k].n1;
      f.freq[4] = fx_noise[k].n2;
      f.distortion = fx_noise[k].dist;
      f.vol[3] = f.vol[4] =
          0.5 * (f.vol[0] + f.vol[1] + f.vol[2]) * pow(10, fx_noise[k].db / 20);
    }
    /* a pulsed tone effect restarts its tones at half the frequency of the
     * loudest (see fx_pulse) */
    if (q >= 0) {
      int c = 0;
      for (int j = 1; j < 3; j++)
        if (p[i].vol[j] > p[i].vol[c])
          c = j;
      f.pulse_offset = p[i].freq[c] / 2 - voice_hz(SJ_DEFAULT_PITCH) + fx_pulse[q].offset_hz;
      f.pitch = voice_hz(SJ_DEFAULT_PITCH) + f.pulse_offset;
      f.wave = ENV_VOICE;
      f.voiced = true;
      f.pulsed = true;
      f.fade_scale = fx_pulse[q].fade;
    }
    hold(s, p[i].ms, p[i].ms, f);
  }
}
/* contour_tones plays an effect from its tones' knots (see fx_knots): a part
 * at every knot of any of them, each tone at its straight line's value there.
 * false if the effect has no tones */
static bool contour_tones(sound *s, int code) {
  int times[MAX_PARTS], n = 0;
  for (int k = 0; k < (int)(sizeof(fx_tones) / sizeof(fx_tones[0])); k++) {
    if (fx_tones[k].code != code)
      continue;
    for (int i = 0; i < fx_tones[k].count; i++) {
      const int t = fx_knots[3 * (fx_tones[k].first + i)];
      int j = 0;
      while (j < n && times[j] < t)
        j++;
      if (j < n && times[j] == t)
        continue;
      for (int m = n; m > j; m--)
        times[m] = times[m - 1];
      times[j] = t;
      n++;
    }
  }
  if (n == 0)
    return false;
  static fx2_point p[MAX_PARTS];
  memset(p, 0, sizeof(p));
  for (int i = 0; i < n; i++) {
    p[i].ms = (float)((times[i] - (i > 0 ? times[i - 1] : 0)) / 10.0);
    for (int k = 0; k < (int)(sizeof(fx_tones) / sizeof(fx_tones[0])); k++) {
      if (fx_tones[k].code != code)
        continue;
      const short *q = &fx_knots[3 * fx_tones[k].first];
      const int c = fx_tones[k].count;
      double f, v;
      if (times[i] <= q[0]) {
        f = q[1];
        v = q[2];
      } else if (times[i] >= q[3 * (c - 1)]) {
        f = q[3 * (c - 1) + 1];
        v = q[3 * (c - 1) + 2];
      } else {
        int j = 0;
        while (q[3 * (j + 1)] <= times[i])
          j++;
        const double w = (double)(times[i] - q[3 * j]) / (q[3 * (j + 1)] - q[3 * j]);
        f = q[3 * j + 1] + w * (q[3 * (j + 1) + 1] - q[3 * j + 1]);
        v = q[3 * j + 2] + w * (q[3 * (j + 1) + 2] - q[3 * j + 2]);
      }
      p[i].freq[fx_tones[k].osc] = (float)f;
      p[i].vol[fx_tones[k].osc] = (float)(v / 100);
    }
  }
  contour2(s, p, n, code);
  return true;
}
/* FX2-END */

/* FXV-BEGIN */
/* the buzz effects played as the chip plays them, pulse by pulse (measured as
 * vf_points are): each point is a pulse's time after the one before, the three
 * formants and their volumes */
typedef struct fxv_point {
  float ms;
  float freq[3];
  float vol[3];
} fxv_point;

static const fxv_point fxv_points[] = {
    {1.00, {262, 1651, 1422}, {5.46, 9.84, 3.08}}, /* A2 */
    {125.61, {260, 1549, 2556}, {5.65, 9.24, 9.35}}, /* A2 */
    {22.95, {235, 1533, 2488}, {6.63, 9.81, 8.93}}, /* A2 */
    {24.34, {226, 1523, 2417}, {7.24, 12.13, 10.91}}, /* A2 */
    {23.09, {679, 1509, 2347}, {4.85, 15.62, 14.52}}, /* A2 */
    {11.52, {219, 1500, 2311}, {7.76, 16.22, 14.07}}, /* A2 */
    {11.48, {227, 1494, 2265}, {6.96, 15.86, 15.20}}, /* A2 */
    {11.50, {224, 1486, 2262}, {7.05, 15.82, 15.94}}, /* A2 */
    {11.52, {215, 1468, 2226}, {7.11, 15.58, 15.38}}, /* A2 */
    {11.53, {209, 1458, 2209}, {7.23, 15.80, 13.78}}, /* A2 */
    {11.45, {209, 1444, 2193}, {6.94, 15.75, 12.62}}, /* A2 */
    {11.45, {189, 1426, 2102}, {7.67, 15.86, 13.59}}, /* A2 */
    {11.66, {197, 1434, 2015}, {8.29, 16.90, 13.79}}, /* A2 */
    {11.50, {255, 1420, 2216}, {6.54, 17.48, 8.01}}, /* A2 */
    {11.46, {211, 1405, 1940}, {9.34, 17.45, 15.49}}, /* A2 */
    {11.43, {208, 1402, 1918}, {10.11, 17.49, 15.89}}, /* A2 */
    {11.42, {198, 1395, 1883}, {10.80, 17.58, 15.01}}, /* A2 */
    {11.78, {204, 1395, 1803}, {9.51, 18.15, 16.25}}, /* A2 */
    {11.46, {209, 1375, 1783}, {8.66, 17.79, 16.84}}, /* A2 */
    {11.44, {236, 1352, 1772}, {6.21, 17.09, 16.15}}, /* A2 */
    {11.44, {230, 1340, 1734}, {6.65, 16.83, 16.72}}, /* A2 */
    {11.45, {224, 1328, 1674}, {6.81, 17.20, 16.27}}, /* A2 */
    {11.46, {252, 1332, 1615}, {5.75, 17.88, 16.68}}, /* A2 */
    {11.45, {239, 1305, 1598}, {5.87, 17.50, 16.52}}, /* A2 */
    {11.44, {250, 1285, 1562}, {5.76, 17.22, 16.06}}, /* A2 */
    {11.47, {679, 1265, 1514}, {5.10, 16.54, 15.33}}, /* A2 */
    {11.45, {679, 1279, 1469}, {4.86, 17.50, 17.17}}, /* A2 */
    {11.41, {258, 1257, 1422}, {5.25, 18.15, 18.08}}, /* A2 */
    {11.50, {262, 1315, 1999}, {6.11, 17.10, 9.94}}, /* A2 */
    {11.45, {282, 1227, 1977}, {5.22, 16.80, 9.15}}, /* A2 */
    {11.44, {282, 1264, 2558}, {5.28, 23.19, 5.94}}, /* A2 */
    {11.42, {272, 1245, 2488}, {5.50, 26.77, 6.38}}, /* A2 */
    {11.43, {270, 1220, 2578}, {5.71, 29.62, 7.58}}, /* A2 */
    {11.44, {275, 1193, 2581}, {5.23, 31.00, 5.37}}, /* A2 */
    {11.46, {269, 1174, 2578}, {5.50, 31.00, 6.95}}, /* A2 */
    {11.38, {679, 1143, 1422}, {4.63, 31.00, 3.15}}, /* A2 */
    {11.20, {250, 1118, 2492}, {6.85, 31.00, 5.91}}, /* A2 */
    {11.50, {242, 1092, 1422}, {6.88, 31.00, 4.31}}, /* A2 */
    {11.39, {192, 1064, 1422}, {11.19, 31.00, 1.65}}, /* A2 */
    {11.46, {183, 1033, 1422}, {11.78, 31.00, 3.34}}, /* A2 */
    {11.29, {679, 1018, 1422}, {3.22, 28.65, 2.44}}, /* A2 */
    {11.59, {911, 1078, 1422}, {17.97, 16.82, 2.81}}, /* A2 */
    {11.97, {881, 1054, 1422}, {18.24, 17.03, 2.42}}, /* A2 */
    {10.87, {850, 1040, 1422}, {19.04, 17.42, 2.80}}, /* A2 */
    {12.09, {810, 1031, 1422}, {18.82, 16.88, 1.58}}, /* A2 */
    {11.45, {781, 1013, 1422}, {19.69, 17.52, 2.05}}, /* A2 */
    {10.74, {740, 1001, 1422}, {19.92, 17.81, 3.16}}, /* A2 */
    {11.46, {715, 980, 1422}, {20.36, 18.38, 2.57}}, /* A2 */
    {11.45, {679, 968, 1422}, {20.58, 18.73, 2.70}}, /* A2 */
    {14.17, {672, 915, 2536}, {16.48, 15.21, 4.63}}, /* A2 */
    {10.75, {630, 912, 2600}, {12.57, 11.88, 4.62}}, /* A2 */
    {11.58, {601, 918, 2495}, {14.14, 13.33, 4.72}}, /* A2 */
    {11.54, {549, 911, 2445}, {14.24, 13.27, 6.28}}, /* A2 */
    {11.52, {535, 924, 2445}, {14.22, 13.99, 8.79}}, /* A2 */
    {11.52, {517, 921, 2432}, {13.84, 13.86, 7.64}}, /* A2 */
    {11.42, {396, 877, 1422}, {15.13, 13.10, 2.09}}, /* A2 */
    {10.82, {409, 845, 1422}, {15.81, 14.73, 2.60}}, /* A2 */
    {11.52, {386, 841, 1422}, {14.56, 15.01, 3.18}}, /* A2 */
    {11.57, {366, 831, 2576}, {12.89, 14.69, 4.58}}, /* A2 */
    {11.54, {313, 831, 1422}, {12.18, 14.11, 3.99}}, /* A2 */
    {11.50, {215, 797, 1422}, {19.92, 14.28, 3.13}}, /* A2 */
    {10.75, {200, 803, 1422}, {19.90, 13.82, 2.59}}, /* A2 */
    {11.59, {186, 786, 1422}, {17.65, 12.75, 1.26}}, /* A2 */
    {11.50, {189, 769, 1422}, {12.85, 12.85, 2.06}}, /* A2 */
    {45.73, {206, 711, 1422}, {9.25, 16.00, 2.64}}, /* A2 */
    {11.41, {220, 714, 1422}, {6.82, 14.01, 2.82}}, /* A2 */
    {12.17, {679, 1143, 1422}, {3.07, 7.34, 0.90}}, /* A2 */
    {11.50, {679, 1143, 1422}, {3.07, 7.34, 0.90}}, /* A2 */
};

static const struct {
  short first, count;
} fxv_index[] = {
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 68},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0}
};

static double voice_hz(double pitch);

static void contour_voiced(sound *s, const fxv_point *p, int n) {
  frame f = voiced_frame(FM(0, 0, 0), 0, 0, 0);
  for (int i = 0; i < n; i++) {
    f = voiced_frame(FM(p[i].freq[0], p[i].freq[1], p[i].freq[2]), p[i].vol[0], p[i].vol[1], p[i].vol[2]);
    f.pitch = voice_hz(SJ_DEFAULT_PITCH);
    hold(s, p[i].ms, p[i].ms, f);
  }
  hold(s, 3, 3, quiet(f));
}
/* FXV-END */

/* C1 is voiced: a harmonic series whose own pitch sweeps, in whole voicing
 * ticks (see voice_hz()), from 333Hz up to 570Hz by 80ms and down to 200Hz,
 * with three formants moving through it; the Pitch control doesn't move it
 * (see sound.own_pitch). over its last 200ms one line rises from 2.0 to 2.9kHz
 * through the harmonics: oscillator 3 there runs free of the voicing (see
 * frame.free_osc). each point: its time after the one before, the pitch, the
 * formants and their volumes */
typedef struct c1_point {
  float ms, pitch;
  float freq[3];
  float vol[3];
} c1_point;

static const c1_point c1_points[] = {
    {1.00, 332.8, {877, 1443, 1997}, {10.15, 1.21, 0.00}},
    {1.90, 332.8, {765, 1439, 2089}, {6.63, 0.64, 0.00}},
    {1.90, 332.8, {848, 1354, 2096}, {11.68, 3.93, 0.00}},
    {3.80, 332.8, {1112, 1172, 2064}, {25.95, 12.43, 0.00}},
    {3.80, 347.2, {896, 1604, 2203}, {17.19, 11.29, 0.00}},
    {1.90, 347.2, {823, 1626, 2249}, {16.38, 14.57, 0.00}},
    {3.80, 347.2, {745, 1282, 2297}, {21.90, 28.86, 0.00}},
    {3.80, 347.2, {569, 1574, 2179}, {22.61, 16.36, 0.00}},
    {1.90, 347.2, {557, 1662, 2137}, {23.54, 12.43, 0.19}},
    {3.80, 347.2, {1014, 1357, 2190}, {29.82, 23.86, 1.71}},
    {1.90, 347.2, {1076, 1440, 2217}, {25.70, 23.50, 1.52}},
    {3.80, 347.2, {1000, 1890, 2270}, {8.75, 15.50, 0.00}},
    {3.80, 399.3, {1670, 2073, 2140}, {25.70, 25.79, 5.33}},
    {1.90, 399.3, {1791, 2064, 2103}, {28.79, 28.14, 7.57}},
    {3.80, 420.3, {1319, 1712, 2118}, {16.98, 23.57, 10.62}},
    {3.80, 420.3, {1147, 1526, 1933}, {20.17, 21.00, 7.67}},
    {1.90, 443.7, {1076, 1474, 1832}, {22.58, 19.74, 5.90}},
    {3.80, 443.7, {855, 1777, 1710}, {23.92, 15.55, 4.38}},
    {1.90, 469.8, {747, 1667, 1730}, {23.49, 15.17, 4.76}},
    {3.80, 469.8, {533, 926, 1928}, {20.44, 17.83, 7.81}},
    {1.90, 499.1, {433, 1078, 2032}, {17.58, 19.17, 12.00}},
    {1.90, 499.1, {334, 1303, 2136}, {14.54, 20.50, 16.57}},
    {1.90, 499.1, {264, 1418, 2218}, {11.13, 20.60, 20.00}},
    {1.90, 532.4, {279, 1198, 2232}, {6.65, 16.98, 20.00}},
    {1.90, 532.4, {294, 977, 2245}, {2.18, 13.36, 20.00}},
    {1.90, 532.4, {369, 900, 2258}, {1.51, 11.05, 20.48}},
    {1.90, 532.4, {480, 909, 2272}, {3.13, 9.52, 21.24}},
    {1.90, 570.4, {590, 918, 2285}, {4.75, 8.00, 22.00}},
    {3.80, 570.4, {133, 1255, 2312}, {6.46, 14.86, 24.29}},
    {1.90, 570.4, {59, 1412, 2295}, {6.00, 18.32, 24.00}},
    {1.90, 570.4, {241, 1550, 2228}, {3.33, 21.85, 21.33}},
    {1.90, 532.4, {424, 1688, 2162}, {0.67, 25.37, 18.67}},
    {1.90, 532.4, {376, 1632, 2164}, {3.57, 22.61, 18.29}},
    {1.90, 532.4, {250, 1511, 2188}, {8.33, 17.75, 18.67}},
    {1.90, 532.4, {128, 1447, 2183}, {11.90, 14.08, 18.50}},
    {3.80, 532.4, {-70, 2111, 1749}, {2.38, 23.42, 10.50}},
    {1.90, 532.4, {-125, 2182, 1667}, {0.33, 24.96, 8.79}},
    {5.70, 532.4, {-152, 1612, 1829}, {2.33, 20.25, 10.50}},
    {1.90, 499.1, {-161, 1422, 1882}, {3.00, 18.68, 11.07}},
    {1.90, 499.1, {-158, 1288, 1931}, {3.46, 17.39, 11.64}},
    {13.30, 499.1, {106, 1510, 2157}, {2.35, 14.28, 15.64}},
    {1.90, 499.1, {130, 1545, 2182}, {2.17, 14.33, 15.43}},
    {1.90, 469.8, {131, 1586, 2195}, {1.96, 15.21, 13.90}},
    {15.20, 469.8, {139, 1914, 2298}, {0.24, 22.26, 1.71}},
    {1.90, 443.7, {140, 1955, 2311}, {0.03, 23.14, 0.19}},
    {1.90, 443.7, {202, 1904, 2288}, {0.25, 21.98, 1.33}},
    {17.10, 420.3, {845, 1330, 2027}, {2.82, 8.91, 15.05}},
    {1.90, 420.3, {924, 1324, 2017}, {3.21, 8.00, 15.57}},
    {3.80, 420.3, {1105, 1505, 2062}, {4.36, 8.00, 13.29}},
    {1.90, 399.3, {1195, 1595, 2084}, {4.93, 8.00, 12.14}},
    {13.30, 399.3, {1829, 2229, 2241}, {8.93, 8.00, 4.14}},
    {7.60, 363.0, {1497, 2096, 2298}, {7.89, 7.82, 4.74}},
    {11.40, 347.2, {965, 1873, 2381}, {6.18, 7.53, 5.88}},
    {1.90, 347.2, {915, 1842, 2391}, {5.95, 7.34, 6.04}},
    {19.00, 307.2, {1048, 1633, 2415}, {4.52, 3.05, 6.99}},
    {22.80, 275.4, {854, 1615, 2633}, {1.84, 1.96, 3.87}},
    {17.10, 242.0, {751, 1580, 2706}, {0.76, 0.03, 3.01}},
    {20.90, 210.2, {530, 1720, 2802}, {0.00, 0.04, 2.14}},
    {1.90, 199.7, {530, 1720, 2825}, {0.00, 0.13, 1.86}},
    {11.40, 199.7, {530, 1720, 2965}, {0.00, 0.70, 0.14}},
    {7.60, 199.7, {530, 1720, 2926}, {0.00, 0.25, 0.00}},
};

static void c1(sound *s) {
  const int n = (int)(sizeof(c1_points) / sizeof(c1_points[0]));
  frame f = voiced_frame(FM(0, 0, 0), 0, 0, 0);
  for (int i = 0; i < n; i++) {
    const c1_point *p = &c1_points[i];
    f = voiced_frame(FM(p->freq[0], p->freq[1], p->freq[2]), p->vol[0], p->vol[1], p->vol[2]);
    f.pitch = p->pitch;
    f.free_osc = 4;
    hold(s, p->ms, i == 0 ? 1 : p->ms, f);
  }
  hold(s, 3, 3, quiet(f));
  s->own_pitch = true;
}

static void effect(sound *s, int code) {
  if (code == 231) {
    c1(s);
    return;
  }
  if (fxv_index[code - 200].count > 0) {
    contour_voiced(s, &fxv_points[fxv_index[code - 200].first], fxv_index[code - 200].count);
    return;
  }
  if (contour_tones(s, code))
    return;
  if (fx_index[code - 200].count > 0) {
    contour(s, &fx_points[fx_index[code - 200].first], fx_index[code - 200].count,
            code);
    return;
  }
  switch (code) {
  case 238: c_noise(s, c8_db, (int)(sizeof(c8_db) / sizeof(c8_db[0]))); break; /* C8 */
  case 239: c_noise(s, c9_db, (int)(sizeof(c9_db) / sizeof(c9_db[0]))); break; /* C9 */

  case 253: { /* M1 */
    /* a bang of noise, then hiss around 1 to 2kHz that dies away by 27dB and
     * stops at 300ms */
    const frame bang = noise_frame(lGlottal, 500, 1400, 31, 26, 255);
    const frame hiss = noise_frame(lGlottal, 1000, 1700, 31, 26, 160);
    hold(s, 30, 2, bang);
    hold(s, 20, 20, fade(hiss, 1.5));
    hold(s, 30, 30, fade(hiss, 2));
    hold(s, 50, 50, fade(hiss, 7));
    hold(s, 70, 70, fade(hiss, 12));
    hold(s, 50, 50, fade(hiss, 16));
    hold(s, 40, 40, fade(hiss, 26));
    hold(s, 10, 10, quiet(hiss));
    break;
  }
  default: break;
  }
}

/* lookup builds the sound for an MSA code. the Go version builds every sound
 * once into a table; here each is built when it is needed, which costs a few
 * struct copies per command and keeps the library free of global state */
/* the noise sounds refitted at the chip's own rate (the noise is a frequency
 * jittered every sample, so it moves with the rate): a scale on oscillators 4
 * and 5's frequencies, a change to their width (distortion) and their levels */
static const struct {
  short code;
  double freq_scale, distortion, db4, db5;
} noise_rate_fit[] = {
    {165, 0.9836, -0.3, -0.28, 1.63},
    {166, 0.9824, 6.7, 3.06, -0.80},
    {170, 0.9310, -65.4, 3.72, -3.84},
    {172, 1.0617, -43.2, 8.47, -9.02},
    {173, 0.9253, 78.7, -1.43, -1.04},
    {174, 0.9881, -26.6, -1.23, 1.34},
    {182, 0.9778, 4.5, 0.80, -1.31},
    {183, 1.0414, -17.3, 0.07, 2.65},
    {187, 1.0507, -14.3, 3.45, -0.95},
    {191, 1.0258, 9.5, -2.09, 1.07},
    {192, 0.9785, -13.6, 1.15, -0.49},
    {193, 1.0706, -13.3, 10.28, -7.08},
    {194, 1.0126, 29.7, 0.95, -1.50},
    {195, 1.0266, 38.7, 0.68, -0.16},
    {196, 1.0444, -8.5, 5.29, 0.25},
    {197, 0.9356, 13.6, -0.62, 1.86},
    {198, 0.9956, 2.6, 0.55, -0.66},
    {199, 0.9547, 72.6, -0.02, -0.38},
    {200, 0.9870, 54.5, -7.08, 6.43},
    {201, 0.9336, -10.3, 1.43, -1.06},
    {202, 1.0186, 11.8, -4.17, 4.26},
    {204, 0.9880, -20.2, -0.10, 0.58},
    {206, 0.9968, -16.9, -1.95, 1.83},
    {207, 0.9756, 76.1, -0.32, -0.13},
    {210, 1.0497, 15.8, 2.86, -2.60},
    {214, 0.9786, 65.5, -1.43, 1.26},
    {216, 1.0994, -3.7, -0.16, -0.05},
    {218, 0.9956, 14.7, 8.70, -9.23},
    {226, 1.0477, -5.8, 0.02, 0.45},
    {230, 1.0215, 17.9, -0.72, 0.24},
    {231, 1.0252, -41.2, -1.28, 1.67},
    {232, 1.0196, 0.3, 3.26, -4.28},
    {234, 1.0933, 49.7, 2.56, -2.96},
    {236, 0.9841, 31.2, 1.12, -1.36},
    {237, 0.9611, 36.2, -0.42, -0.06},
    {238, 1.0423, 12.9, -0.26, 0.61},
    {239, 1.0203, 39.1, 2.42, -6.94}
};

/* the sounds shaped further than the oscillators can: each is played through
 * peaking filters half an octave apart (see synth_eq()), the voiceless ones
 * whose hiss is broader on the chip with broadband noise as well, and an
 * overall gain that keeps each at its level. the gains glide with the frames
 * from one sound into the next. the code, the filters' gains in dB, the
 * broadband noise and the overall gain */
static const struct {
  short code;
  double db[EQ_BANDS];
  double wide, gain_db;
} sound_eq[] = {
    {128, {0.59, 0.92, 0.8, 0.56, -0.11, -4.27, 0.73, 1.78, 0.69, -0.66}, 0.00, 0.00}, /* IY */
    {129, {0.81, 0.3, 0.53, 2.09, 0.61, -2.84, 1.16, 1.54, -0.81, -0.67}, 0.00, 0.00}, /* IH */
    {137, {0.04, 0.79, 0.38, -0.44, 0.11, 0.29, -0.74, -4.16, 2.52, -1.08}, 0.00, 0.00}, /* OW */
    {138, {-0.34, -0.23, -0.25, -0.4, -0.56, -0.33, -0.5, -0.63, 0.31, 0.34}, 0.00, 0.00}, /* UH */
    {141, {0.52, 0.71, -1.77, -3.12, 5.48, 3.0, -2.16, 0.66, -0.4, -0.04}, 0.00, 0.00}, /* NE */
    {142, {0.21, 0.79, 0.9, 0.91, 4.69, -0.18, -3.12, 4.81, -1.82, -0.32}, 0.00, 0.00}, /* NO */
    {143, {1.5, 1.7, 0.15, -1.93, -1.67, 3.09, 0.07, -1.45, -0.89, 0.18}, 0.00, 0.00}, /* NGE */
    {144, {1.45, 1.99, -0.97, -2.64, 3.27, -0.0, -1.75, 0.49, -1.86, -1.18}, 0.00, 0.00}, /* NGO */
    {145, {-0.59, -0.05, -0.28, -0.76, -0.61, 0.04, -0.92, -3.34, -0.26, 1.11}, 0.00, 0.00}, /* LE */
    {146, {1.83, 1.91, 1.51, 0.27, 0.22, 2.77, 0.36, -4.49, 3.8, -2.56}, 0.00, 0.00}, /* LO */
    {147, {0.14, 0.38, 0.07, -0.12, 0.13, -0.45, -1.43, -1.13, 0.17, 1.21}, 0.00, 0.00}, /* WW */
    {148, {0.94, 1.07, 0.61, -0.13, 0.39, 1.28, 0.59, 0.32, -1.79, -0.49}, 0.00, 0.00}, /* RR */
    {149, {-0.45, 0.73, 1.15, 0.42, -0.13, -0.32, -0.37, 2.8, -0.82, -0.32}, 0.00, 0.00}, /* IYRR */
    {150, {0.59, 2.02, 5.4, 0.93, -8.79, 1.4, 4.8, 0.5, -2.03, -0.57}, 0.00, 0.00}, /* EYRR */
    {152, {1.23, 1.52, 1.51, 0.73, -0.09, 0.22, 1.24, 2.67, -2.02, -1.5}, 0.00, 0.00}, /* AWRR */
    {153, {1.42, 1.71, 1.45, 0.41, 0.01, 0.32, -0.48, 7.66, -5.81, -0.96}, 0.00, 0.00}, /* OWRR */
    {155, {-1.09, -0.11, 2.06, 2.7, -1.59, -3.47, 2.26, 2.17, -0.05, -0.28}, 0.00, 0.00}, /* OHIY */
    {157, {0.5, 0.01, 1.2, 2.19, -1.63, -2.45, 2.01, 0.61, -0.21, -0.57}, 0.00, 0.00}, /* OHIH */
    {160, {-0.13, 0.39, 1.03, 1.7, 0.03, -1.4, -0.15, 2.37, -0.35, -0.58}, 0.00, 0.00}, /* IYUW */
    {162, {0.73, 0.35, 0.3, 1.06, 1.27, 0.19, 0.06, 1.95, -1.32, -0.88}, 0.00, 0.00}, /* IHWW */
    {163, {1.06, 1.35, 1.5, 0.64, -0.0, 0.8, 0.65, 0.79, -2.09, -0.57}, 0.00, 0.00}, /* AYWW */
    {164, {2.24, 2.59, 2.55, 0.43, 0.44, 2.74, -0.61, 2.59, -4.1, -1.81}, 0.00, 0.00}, /* OWWW */
    {165, {-6.4, -0.3, 6.2, -5.1, 3.7, -1.6, -1.5, -0.1, 1.3, 3.8}, 0.00, 1.19}, /* JH */
    {166, {5.09, 0.01, -1.01, 2.03, -2.75, -1.79, 0.32, 0.11, 1.7, -2.45}, 0.00, 0.23}, /* VV */
    {167, {1.9, -0.3, -0.6, 2.1, 0.4, -3.7, 0.4, -1.8, -1.1, 2.8}, 0.00, -0.45}, /* ZZ */
    {168, {0.45, 0.35, -1.52, -1.06, 3.8, -2.04, -2.27, 4.69, 0.18, -1.18}, 0.00, -0.14}, /* ZH */
    {169, {2.7, -0.9, -0.9, -1.2, 8.4, -5.4, -1.1, 1.4, -2.3, -0.5}, 0.00, -0.45}, /* DH */
    {170, {-8.2, 3.3, 3.2, 2.2, -4.5, -3.2, 5.5, 0.1, 2.1, -0.5}, 0.00, 0.56}, /* BE */
    {171, {2.0, 2.4, 0.3, -0.8, -1.7, 3.6, -0.3, -0.1, -2.2, -3.3}, 0.00, -0.97}, /* BO */
    {173, {-0.5, 4.5, -3.5, 3.5, 3.9, -11.5, 15.0, -7.0, 3.1, -7.8}, 0.00, -2.81}, /* OB */
    {174, {-5.91, -0.32, 8.46, -0.96, -4.0, 0.66, -0.66, -3.21, 0.68, 4.1}, 0.00, 1.38}, /* DE */
    {175, {1.4, 0.6, -1.0, -1.1, 4.0, 0.8, -0.9, 1.0, -1.5, -3.4}, 0.00, -0.86}, /* DO */
    {176, {-4.94, 0.68, 1.7, -4.78, 14.76, -3.74, -0.04, 3.1, -4.58, -1.06}, 0.00, 1.11}, /* ED */
    {177, {-1.5, 0.8, -2.8, -1.4, 10.0, -0.8, -1.7, 1.2, -3.2, -0.5}, 0.00, 0.78}, /* OD */
    {178, {-1.02, 0.92, -1.23, -3.58, 6.81, 7.37, -14.04, 5.31, -3.39, 2.85}, 0.00, 0.67}, /* GE */
    {179, {-1.2, 1.0, -1.1, -2.1, 3.5, 14.4, -15.0, 7.0, -10.8, 4.4}, 0.00, 0.12}, /* GO */
    {180, {-0.42, 0.97, -0.53, -1.12, 10.61, -4.92, -1.04, 3.87, -6.69, -1.2}, 0.00, -1.34}, /* EG */
    {181, {1.2, 1.0, -0.5, 1.9, 4.9, -3.7, 0.3, 1.3, -7.0, 0.5}, 0.00, -1.73}, /* OG */
    {184, {1.26, 0.91, -0.86, -1.22, -0.77, -0.39, -1.21, -0.71, -1.05, 1.16}, 0.00, 0.00}, /* HO */
    {185, {-2.25, -0.18, -2.37, -1.16, 0.61, -2.23, -5.49, -1.02, 1.7, 2.2}, 0.00, 0.00}, /* WH */
    {187, {7.36, 2.8, -7.71, -6.11, 2.58, 0.46, -1.0, -3.48, -0.41, 2.45}, 0.00, 0.00}, /* SE */
    {188, {6.58, 3.83, -7.1, -6.78, 4.02, 1.38, -4.01, -1.59, -0.85, -1.86}, 0.00, 0.00}, /* SO */
    {189, {5.95, -0.24, -7.18, -2.18, 3.08, -2.64, -1.37, 1.32, 1.53, -2.11}, 0.00, 0.00}, /* SH */
    {190, {-2.02, 2.83, 1.28, -0.81, -0.62, -0.05, 2.49, -3.29, 1.66, 1.15}, 0.00, 0.00}, /* TH */
    {191, {3.1, 1.19, -1.25, -1.05, -0.89, -1.47, -2.59, -1.85, -1.0, 0.8}, 0.00, 0.00}, /* TT */
    {192, {-2.71, 0.29, -3.38, -0.61, -1.42, -3.45, -6.33, -0.11, 4.07, 10.45}, 0.60, -6.18}, /* TU */
    {193, {0.63, -0.01, 0.11, 0.32, 0.19, 0.04, -1.1, -2.48, -0.11, -0.53}, 0.00, 0.00}, /* TS */
    {194, {-0.16, -1.78, 0.21, 2.53, -1.67, 0.27, 2.53, -0.37, 3.75, -2.81}, 0.00, 0.00}, /* KE */
    {195, {2.1, 2.8, -6.9, -3.0, 7.0, -2.7, 1.1, 2.7, 3.6, -6.7}, 0.30, -2.29}, /* KO */
    {196, {0.27, 1.59, 2.0, -4.02, -2.63, -0.96, 6.46, -2.63, 5.84, -5.57}, 0.30, -1.01}, /* EK */
    {197, {3.04, 0.01, -0.06, 0.63, -3.1, 0.44, 1.48, 1.91, 0.98, -2.91}, 0.00, 0.00}, /* OK */
    {198, {7.82, 2.52, -2.54, -1.67, 1.63, -1.47, 0.05, -2.11, -6.27, -0.34}, 0.00, 0.00}, /* PE */
    {199, {6.42, 0.76, -2.16, -2.84, -0.61, -0.25, -3.13, -2.09, -1.04, 0.89}, 0.00, 0.00}, /* PO */
    {200, {3.74, 1.61, -0.69, -1.09, -0.23, -1.19, -0.44, -2.49, -0.93, -0.09}, 0.00, 0.00}, /* R0 */
    {201, {0.07, 0.7, 2.55, 1.9, -1.87, -2.68, -1.61, -0.32, -1.36, 2.31}, 0.00, 0.00}, /* R1 */
    {202, {4.66, 7.04, -3.66, -5.05, -0.88, -2.55, -1.45, -2.56, 2.41, 0.24}, 0.00, 0.00}, /* R2 */
    {204, {4.92, -1.88, -4.48, -3.07, -3.88, -0.18, -2.6, -3.04, 2.21, -0.09}, 0.00, 0.00}, /* R4 */
    {205, {13.1, 2.71, -5.99, 0.14, -0.25, -0.73, 3.51, 1.5, -8.89, -2.01}, 0.00, 0.00}, /* R5 */
    {206, {5.65, -2.19, -6.24, -3.53, -4.5, -1.78, 2.62, -15.0, 0.88, 6.59}, 0.00, 0.00}, /* R6 */
    {207, {5.92, -1.73, -4.59, -2.57, -1.37, -3.12, -2.36, 1.65, -5.11, 0.16}, 0.00, 0.00}, /* R7 */
    {208, {3.7, -2.13, -2.51, 2.0, -0.93, -3.17, 0.58, -1.3, -1.96, 0.51}, 0.00, 0.00}, /* R8 */
    {209, {3.52, -0.19, -1.61, 0.37, -0.5, 0.2, 1.53, -3.03, -0.81, -1.44}, 0.00, 0.00}, /* R9 */
    {210, {12.11, -2.22, -7.96, -3.48, -2.47, -3.68, -2.74, -10.51, 9.28, -3.84}, 0.00, 0.00}, /* A0 */
    {211, {1.09, 0.93, 0.82, 0.97, 1.04, 0.45, 0.42, 0.81, -0.31, -1.21}, 0.00, 0.00}, /* A1 */
    {212, {3.04, 1.32, -0.5, -0.7, -0.38, -0.42, -0.82, -1.3, 1.62, -1.14}, 0.00, 0.00}, /* A2 */
    {213, {1.32, 1.67, 1.04, 0.27, 1.07, 1.18, 1.89, -0.25, -1.63, -0.92}, 0.00, 0.00}, /* A3 */
    {214, {8.15, -1.43, -3.03, -0.77, -0.58, -1.6, -0.15, -1.96, -0.2, -0.92}, 0.00, 0.00}, /* A4 */
    {215, {1.63, 0.78, 0.09, -0.06, -0.69, -1.06, -1.94, -0.89, 0.42, -0.12}, 0.00, 0.00}, /* A5 */
    {218, {10.76, 0.42, -5.11, -2.3, -1.69, -2.8, -1.19, -3.63, -4.09, -0.41}, 0.00, 0.00}, /* A8 */
    {219, {15.0, 1.3, -4.51, -1.64, -0.22, -0.69, -0.38, 0.58, -4.75, -5.65}, 0.00, 0.00}, /* A9 */
    {222, {15.0, -0.56, -13.01, -8.09, -2.82, 2.34, -5.52, -7.31, -8.71, 0.68}, 0.00, 0.00}, /* B2 */
    {223, {-0.4, -0.34, -0.44, -1.21, -0.72, 0.05, -1.05, 2.81, 0.93, -0.32}, 0.00, 0.00}, /* B3 */
    {224, {0.43, 0.36, -1.78, -1.83, -1.41, -2.46, -3.21, 3.4, 0.25, 1.32}, 0.00, 0.00}, /* B4 */
    {225, {15.0, -3.34, -9.05, -4.62, -5.61, -1.21, 3.55, -9.98, -5.94, -4.63}, 0.00, 0.00}, /* B5 */
    {226, {0.59, 0.32, -1.45, -2.38, -2.22, -1.1, 1.12, 1.89, -3.64, 1.38}, 0.00, 0.00}, /* B6 */
    {228, {0.16, 1.66, 0.77, -1.57, -1.43, -0.24, -3.08, -1.66, -3.54, -1.43}, 0.00, 0.00}, /* B8 */
    {229, {8.37, 3.98, 4.05, 4.64, 1.85, 1.34, -3.02, -6.62, -7.25, -4.84}, 0.00, 0.00}, /* B9 */
    {230, {1.3, 1.13, 1.19, 1.05, 0.17, 0.17, 0.43, 0.45, -0.4, -1.2}, 0.00, 0.00}, /* C0 */
    {231, {-0.06, 0.8, 0.29, -0.66, -0.65, -0.69, -0.75, -1.03, 0.87, 0.69}, 0.00, 0.00}, /* C1 */
    {232, {-1.92, -0.33, 0.08, -0.79, 0.16, 1.24, -2.94, -0.58, 0.77, 1.03}, 0.00, 0.00}, /* C2 */
    {233, {-0.84, -2.06, 0.02, 1.51, -0.34, -0.26, 0.96, -2.55, 1.29, 0.49}, 0.00, 0.00}, /* C3 */
    {234, {0.46, 1.66, 1.66, -0.05, 1.20, 5.89, -5.14, 0.12, 1.53, -1.19}, 0.00, 0.00}, /* C4 */
    {235, {14.23, -0.52, -5.23, -0.72, -1.0, -3.16, 1.87, 5.76, -5.96, -5.58}, 0.00, 0.00}, /* C5 */
    {236, {-0.09, -0.09, -0.13, -0.9, -1.22, -0.65, -0.38, 0.09, 0.68, 0.45}, 0.00, 0.00}, /* C6 */
    {237, {4.9, 2.98, 2.32, 3.2, 1.91, 0.46, -1.07, -0.65, -1.23, -2.81}, 0.00, 0.00}, /* C7 */
    {238, {-0.15, 0.58, 0.09, 2.65, 3.27, 1.16, -1.61, -0.58, 4.66, -0.71}, 0.00, 0.00}, /* C8 */
    {239, {-1.68, -0.34, 0.61, 1.43, 1.84, 0.32, -1.07, -0.16, 6.3, -1.0}, 0.00, 0.00}, /* C9 */
    {241, {1.15, 1.36, 4.22, 0.33, -9.03, -2.0, -3.76, -3.33, 4.01, 0.59}, 0.00, 0.00}, /* D1 */
    {242, {-0.81, 2.07, 6.08, 0.4, -6.12, 9.4, -2.16, -3.26, -0.95, -0.09}, 0.00, 0.00}, /* D2 */
    {243, {0.91, 2.6, 6.22, 1.53, -8.6, 5.37, -1.34, -3.53, -0.56, -0.06}, 0.00, 0.00}, /* D3 */
    {244, {2.35, 5.68, 6.22, 4.34, -6.09, -4.88, -2.97, -1.15, -0.27, -0.03}, 0.00, 0.00}, /* D4 */
    {245, {1.83, 4.72, 7.91, 6.8, -7.78, -1.88, -2.92, -3.77, -0.71, -0.07}, 0.00, 0.00}, /* D5 */
    {246, {4.84, 5.63, 7.8, 5.49, -9.07, -8.74, -4.01, -2.37, -0.57, -0.06}, 0.00, 0.00}, /* D6 */
    {247, {0.89, 1.74, 2.95, 6.65, -0.17, -10.42, 0.22, 2.41, -1.66, -0.14}, 0.00, 0.00}, /* D7 */
    {249, {1.05, 2.31, 4.07, 5.54, -2.72, -5.38, -2.54, -1.91, 0.07, 0.0}, 0.00, 0.00}, /* D9 */
    {250, {-0.83, -1.71, -1.96, 3.01, 3.96, -8.87, -3.89, 5.1, 1.45, 0.13}, 0.00, 0.00}, /* D10 */
    {251, {-1.81, 1.94, 2.94, 2.69, 1.37, -4.02, -2.06, -1.33, 1.51, 0.13}, 0.00, 0.00}, /* D11 */
    {252, {-0.22, 0.52, 1.21, -1.12, 0.12, -2.37, -0.8, 1.93, -2.42, 2.06}, 0.00, 0.00}, /* M0 */
};

static void sound_shaping(sound *s, int code) {
  for (int k = 0; k < (int)(sizeof(sound_eq) / sizeof(sound_eq[0])); k++) {
    if (sound_eq[k].code != code)
      continue;
    for (int i = 0; i < s->nparts; i++) {
      frame *f = &s->parts[i].target;
      memcpy(f->eq, sound_eq[k].db, sizeof(f->eq));
      f->wide = sound_eq[k].wide;
      f->eq_gain = sound_eq[k].gain_db;
    }
  }
}

/* the chip starts most tones at once, where the traces (5ms frames) ramp up.
 * so these effects start sharply: the parts before the first whose summed
 * volume reaches the given fraction of the sound's loudest are silent, and
 * that part is reached at once, the sound's length unchanged */
static const struct {
  short code;
  double fraction;
} sharp_onsets[] = {
    {201, 0.5} /* R1 */,
    {202, 0.125} /* R2 */,
    {203, 0.5} /* R3 */,
    {204, 0.2} /* R4 */,
    {205, 0.1} /* R5 */,
    {206, 0.3} /* R6 */,
    {207, 0.2} /* R7 */,
    {208, 0.2} /* R8 */,
    {211, 0.05} /* A1 */,
    {212, 0.5} /* A2 */,
    {213, 0.7} /* A3 */,
    {214, 0.7} /* A4 */,
    {216, 0.3} /* A6 */,
    {217, 0.2} /* A7 */,
    {218, 0.05} /* A8 */,
    {219, 0.2} /* A9 */,
    {220, 0.2} /* B0 */,
    {221, 0.7} /* B1 */,
    {222, 0.5} /* B2 */,
    {223, 0.7} /* B3 */,
    {224, 0.5} /* B4 */,
    {225, 0.7} /* B5 */,
    {226, 0.3} /* B6 */,
    {227, 0.5} /* B7 */,
    {228, 0.2} /* B8 */,
    {229, 0.3} /* B9 */,
    {235, 0.05} /* C5 */,
    {236, 0.025} /* C6 */,
    {239, 0.5} /* C9 */,
    {240, 0.2} /* D0 */,
    {242, 0.7} /* D2 */,
    {243, 0.475} /* D3 */,
    {244, 0.05} /* D4 */,
    {245, 0.025} /* D5 */,
    {246, 0.05} /* D6 */,
    {247, 0.2} /* D7 */,
    {248, 0.5} /* D8 */,
    {249, 0.7} /* D9 */,
    {250, 0.1} /* D10 */,
    {251, 0.2} /* D11 */,
    {254, 0.7} /* M2 */,
};

static void sharp_onset(sound *s, int code) {
  for (int k = 0; k < (int)(sizeof(sharp_onsets) / sizeof(sharp_onsets[0])); k++) {
    if (sharp_onsets[k].code != code)
      continue;
    double loudest = 0;
    for (int i = 0; i < s->nparts; i++) {
      double v = 0;
      for (int j = 0; j < 5; j++)
        v += s->parts[i].target.vol[j];
      loudest = fmax2(loudest, v);
    }
    for (int i = 0; i < s->nparts; i++) {
      double v = 0;
      for (int j = 0; j < 5; j++)
        v += s->parts[i].target.vol[j];
      s->parts[i].glide_ms = 0;
      if (v >= sharp_onsets[k].fraction * loudest)
        break;
      for (int j = 0; j < 5; j++)
        s->parts[i].target.vol[j] = 0;
    }
  }
}

static bool lookup(uint8_t code, sound *s) {
  memset(s, 0, sizeof(*s));
  if (code >= 128 && code < 128 + NUM_ALLOPHONES) {
    allophone(s, code);
    sound_shaping(s, code);
    s->copy_length = copy_length[code - 128];
    s->no_slow = code >= 182; /* the voiceless consonants (see msa_expand()) */
  } else if (code >= 200 && code < 200 + NUM_EFFECTS) {
    effect(s, code);
    sharp_onset(s, code);
    sound_shaping(s, code);
    s->copy_length = 1;
    s->no_bend = code == 254; /* M2 (see msa_expand()) */
  } else
    return false;
  for (int k = 0; k < (int)(sizeof(noise_rate_fit) / sizeof(noise_rate_fit[0])); k++) {
    if (noise_rate_fit[k].code != code)
      continue;
    const double g4 = pow(10, noise_rate_fit[k].db4 / 20);
    const double g5 = pow(10, noise_rate_fit[k].db5 / 20);
    for (int i = 0; i < s->nparts; i++) {
      frame *f = &s->parts[i].target;
      f->freq[3] *= noise_rate_fit[k].freq_scale;
      f->freq[4] *= noise_rate_fit[k].freq_scale;
      f->distortion = fmin2(fmax2(f->distortion + noise_rate_fit[k].distortion, 0), 255);
      f->vol[3] *= g4;
      f->vol[4] *= g5;
    }
  }
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

/* pause lengths in milliseconds for codes 0 to 6, measured as the silence the
 * chip leaves between two AWs. pause 0 is not nothing */
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
  return (int)round(ms * CHIP_RATE / 1000);
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

  /* the length of the pauses since the last sound, in ms (see
   * msa_closure()) */
  double paused_ms;

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

/* Bend adds to the frequencies of oscillators 1 to 3 (the formants and tones),
 * not multiplies them: 10 of the chip's frequency steps (FREQ_STEP_HZ) a Bend,
 * 5 from 4 to 5. the hiss on oscillators 4 and 5 doesn't move */
static double msa_bend_hz(const msa *m) {
  if (m->bend >= SJ_DEFAULT_BEND)
    return 10 * FREQ_STEP_HZ * (m->bend - SJ_DEFAULT_BEND);
  return FREQ_STEP_HZ * (10 * m->bend - 45);
}

/* a tone Bend took under nothing (see msa_expand()) */
#define WRAP_HZ 5680.0
#define WRAP_MIN_VOL 8.0

/* a frequency played at the chip's rate, folded under half of it */
static double fold_hz(double hz) {
  hz = fmod(hz, CHIP_RATE);
  if (hz < 0)
    hz += CHIP_RATE;
  return hz > CHIP_RATE / 2 ? CHIP_RATE - hz : hz;
}


/* the voicing frequency for a Pitch setting: the chip counts each voicing
 * period in whole ticks of a VOICE_TICK_HZ clock, 8000 / Pitch ticks (whole
 * numbers) and one more, and at 12 settings one more again. not emulated: at
 * very low pitch the chip starts each sound with a pulse; here pitch 0 is
 * silent */
#define VOICE_TICK_HZ 7986.0
static double voice_hz(double pitch) {
  const int p = (int)pitch;
  if (p <= 0)
    return 0;
  int ticks = 8000 / p + 1;
  switch (p) {
  case 44: case 51: case 53: case 56: case 63: case 77:
  case 87: case 88: case 92: case 104: case 127: case 151:
    ticks++;
    break;
  default:
    break;
  }
  return VOICE_TICK_HZ / ticks;
}

/* the duration multiplier from the Speed control: lengths fall in a straight
 * line with speed, reaching zero at SPEED_ZERO, so against the default it is
 * (146 - speed) / 32 (4.6 at speed 0, 0.59 at 127). pauses are lengthened by
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
/* how far Stress raises F2 and F3 (see msa_expand()): about 85Hz on the chip,
 * which is 19 of its frequency steps */
#define STRESS_HZ (19 * FREQ_STEP_HZ)

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

    /* Fast and Slow (FAST_SCALE, SLOW_SCALE). Slow doesn't lengthen the
     * voiceless consonants */
    if (m->fast)
      scale *= FAST_SCALE;
    if (m->slow && !s->no_slow)
      scale *= SLOW_SCALE;

    /* "STRESS, IH sounds more like (but not quite) the IY sound". on the
     * chip (the VoxRecord recording's third take) stress raises F2 and F3 by
     * 65 to 100Hz in AW, EYIY and NE and leaves the pitch and F1 alone.
     * relax changes nothing that can be measured, and so does nothing here */
    if (m->stress)
      stress_hz = STRESS_HZ;
  }

  const double bend_hz = msa_bend_hz(m);
  bool wrapped[3] = {false, false, false};

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
    } else if (t.pulsed) {
      /* a pulsed effect's gate keeps its distance from the voicing (see
       * fx_pulse) */
      t.pitch = fmax2(voice_hz((double)m->pitch) + t.pulse_offset, 1);
    } else if (t.wave == ENV_VOICE && !s->own_pitch) {
      /* a voiced effect follows the Pitch control: on the chip A1 and M2 are
       * voiced at 60.3, 87.9 and 149.5Hz at pitch 60, 88 and 150. their
       * traced pitch is the chip's at the default pitch */
      t.pitch *= voice_hz((double)m->pitch) / voice_hz(SJ_DEFAULT_PITCH);
    }
    /* M2 doesn't follow Bend: on both units of the fourth VoxProbe recording
     * it is the same at bend 0 and bend 15, its formants within half a hertz
     * of each other over its whole length */
    /* a tone that Bend takes under nothing wraps round: it plays WRAP_HZ
     * higher, folded under half the chip's rate, and stays there. a tone
     * quieter than WRAP_MIN_VOL (one its trace couldn't read) doesn't wrap */
    /* C8's and C9's gate keeps time with Speed (see c_noise()) */
    if (s->speed_gate)
      t.pitch /= scale;
    if (!s->no_bend)
      for (int j = 0; j < 3; j++)
        if (t.freq[j] > 0) {
          if (!s->speech && t.vol[j] > WRAP_MIN_VOL && t.freq[j] + bend_hz < 0)
            wrapped[j] = true;
          t.freq[j] = wrapped[j] ? fold_hz(t.freq[j] + bend_hz + WRAP_HZ)
                                 : fmax2(t.freq[j] + bend_hz, 0);
        }
    /* Bend moves an effect's noise as a block, its width unchanged (the Bend
     * is added after the spread: see synth_sample()). the hiss of speech
     * doesn't move */
    if (!s->speech)
      t.noise_shift = bend_hz;
    segment *sg = &m->segs[m->nsegs++];
    sg->samples = to_samples(ms);
    sg->glide = to_samples(glide);
    sg->target = t;
    sg->jump = false;

    const double copy_ms = p->ms * copy_scale * s->copy_length;
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

/* a pause before a stop takes the place of that much of its closure, which
 * keeps MIN_CLOSURE_MS. after a sound the closure is all there */
#define MIN_CLOSURE_MS 20.0
static void msa_closure(msa *m) {
  if (m->nsegs < 2)
    return;
  const int keep = to_samples(MIN_CLOSURE_MS);
  segment *c = &m->segs[0];
  if (c->samples <= keep)
    return;
  c->samples = imax(c->samples - to_samples(m->paused_ms), keep);
  if (c->glide > c->samples)
    c->glide = c->samples;
}

/* the sounds played from the chip's frames (see vf_points) were measured
 * alone: their first and last frames are the voicing coming up from silence
 * and dying away. between two sounds the chip doesn't do that, so after a
 * voiced sound the frames before the first at FRAMED_JOIN of the sound's
 * loudest take its volumes, and before another sound the frames after the last
 * take that one's */
#define FRAMED_JOIN 0.7
static double frame_volume(const frame *f) { return f->vol[0] + f->vol[1] + f->vol[2]; }
static void msa_join(const msa *m, sound *s, bool more) {
  double loudest = 0;
  for (int i = 0; i < s->nparts; i++)
    loudest = fmax2(loudest, frame_volume(&s->parts[i].target));
  if (m->current.voiced && frame_volume(&m->current) > 0) {
    int j = 0;
    while (j < s->nparts - 1 && frame_volume(&s->parts[j].target) < FRAMED_JOIN * loudest)
      j++;
    for (int i = 0; i < j; i++)
      memcpy(s->parts[i].target.vol, s->parts[j].target.vol, 3 * sizeof(double));
  }
  if (more) {
    int j = s->nparts - 1;
    while (j > 0 && frame_volume(&s->parts[j].target) < FRAMED_JOIN * loudest)
      j--;
    for (int i = j + 1; i < s->nparts; i++)
      memcpy(s->parts[i].target.vol, s->parts[j].target.vol, 3 * sizeof(double));
  }
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

  /* the output (see CHIP_RATE): the sample being held, how far through it
   * the output is (in samples at CHIP_RATE), whether the chip was sounding
   * when it was made, and the low pass's state */
  double held;
  double held_pos;
  bool held_sounding;
  double sos_z[OUTPUT_SECTIONS][2];
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
    m->paused_ms += pause_ms[code] * msa_speed_factor(m);
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
      if (s.framed) {
        uint8_t next;
        msa_join(m, &s, peek(sj, &next) && next >= 128 && next < 200);
      }
      /* the last sound before a pause or the end (see final_length) */
      if (code >= 128 && code < 128 + NUM_ALLOPHONES && m->repeat <= 1) {
        uint8_t next;
        if (!peek(sj, &next) || next <= 6)
          s.length *= final_length[code - 128];
      }
      msa_expand(m, &s);
      if (s.stop)
        msa_closure(m);
      m->paused_ms = 0;
      break;
    }
    }
  }

  msa_start(m);
  return true;
}

/* the chip's next sample, at CHIP_RATE */
static double chip_sample(sj_chip *sj) {
  msa *m = &sj->msa;
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

  const double v = msa_next(m);
  sj->held_sounding = msa_busy(m) || sj->buffered > 0;
  return v;
}

bool sj_render(sj_chip *sj, float *buf, size_t n) {
  const double step = CHIP_RATE / SJ_SAMPLE_RATE;
  bool sounding = sj->held_sounding || sj->buffered > 0 || msa_busy(&sj->msa);

  for (size_t i = 0; i < n; i++) {
    /* the average of the held samples over this output sample's time */
    double rest = step, sum = 0;
    while (sj->held_pos + rest >= 1) {
      sum += sj->held * (1 - sj->held_pos);
      rest -= 1 - sj->held_pos;
      sj->held = chip_sample(sj);
      sj->held_pos = 0;
      sounding = sounding || sj->held_sounding;
    }
    sum += sj->held * rest;
    sj->held_pos += rest;

    /* the low pass after the chip (direct form II transposed) */
    double v = sum / step;
    for (int k = 0; k < OUTPUT_SECTIONS; k++) {
      const double *c = output_sos[k];
      double *z = sj->sos_z[k];
      const double y = c[0] * v + z[0];
      z[0] = c[1] * v - c[4] * y + z[1];
      z[1] = c[2] * v - c[5] * y;
      v = y;
    }
    buf[i] = (float)v;
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
