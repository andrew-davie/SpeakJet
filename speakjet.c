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

/* the chip's voicing clock: a voicing period is a whole number of its ticks (see voice_hz()) */
#define VOICE_TICK_HZ 7986.0

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

  /* oscillators 4 and 5 are C8's and C9's swept band (see SWEEP_LO_HZ) */
  bool sweep;

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

  /* restart the voicing at the next sample (see KICK_HZ) */
  bool kick;
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

/* C8 and C9 are a narrow band of noise swept by the envelope oscillator: on
 * both units its middle climbs from about 650Hz to 3kHz over the first 0.42
 * of each period (19.5ms at the default speed) and falls back over the rest,
 * SWEEP_DB quieter at the top, about a tenth of its frequency wide (measured
 * every millisecond, the period folded). the levels are fitted to the M score */
#define SWEEP_LO_HZ 650.0
#define SWEEP_HI_HZ 3000.0
#define SWEEP_PEAK 0.42
#define SWEEP_DB 8.0
#define SWEEP_SPREAD (35.0 / 255.0 * 1.5)

/* sample produces the next output sample, in the range -1 to 1. master is
 * the master volume (0 to 127) */
static double synth_sample(synth *s, const frame *f, double master) {
  if (s->kick) {
    s->kick = false;
    if (f->voiced) {
      s->env_phase = 0;
      s->env_age = 0;
      for (int i = 0; i < 3; i++)
        if (!(f->free_osc >> i & 1))
          s->phase[i] = f->phase0[i];
      s->onset_age = 1L << 20;
    }
  }
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
  /* where C8's and C9's band is in the envelope's period (see SWEEP_LO_HZ) */
  const double sp = s->env_phase;
  const double sweep_t = sp < SWEEP_PEAK ? sp / SWEEP_PEAK : (1 - sp) / (1 - SWEEP_PEAK);
  for (int i = 3; i < 5; i++) {
    const double fr = fmin2(on_grid(f->freq[i]), MAX_FREQUENCY);
    const double r = f->noise_hold_ms > 0 ? s->held[i - 3] : synth_random(s);
    double hz = fr * (1 + spread * r) + f->noise_shift;
    if (f->sweep)
      hz = SWEEP_LO_HZ * pow(SWEEP_HI_HZ / SWEEP_LO_HZ, sweep_t) * (1 + SWEEP_SPREAD * r) + f->noise_shift;
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
  else if (f->sweep)
    mix2 *= pow(10, -SWEEP_DB * sweep_t / 20);
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
  /* the copies' length when repeated, against the manual's: the second play
   * and the copies after it (see copy1_length) */
  double copy1_length;
  double copy_length;
  /* a multiple of the voicing pitch (see voicing_pitch), 0 for none */
  double pitch_scale;

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
 * units: SO's hiss is a band at 3 to 4kHz, HE and HO's around 1 to 1.5kHz,
 * WH's at 0.6 to 0.9kHz. SE and SH are broad: SE from 2 to 5kHz strongest at
 * the oscillators' top, SH from 1.5 to 4kHz (and its image to 6.5kHz), a wide
 * spread every sample where a narrow one made a line. their levels are set by
 * allophone_gain_db */
static frame frSE(void) { return noise_frame(lAlveolar, MAX_FREQUENCY, 2491.4, 35.96, 3.36, 41.0); }
static frame frSO(void) { return noise_frame(lAlveolarBak, 2953.5, 3580, 17.5, 30.41, 27.3); }
static frame frSH(void) { return noise_frame(lPalatal, 3662.7, 2581.1, 14.17, 22.8, 77.5); }
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
    1.4, 0.95, 1.96, 1.47, 2.0825, 1.96, 1.4, 1.7325, 1.4,
    1.68, 1.68, 1.4, 1.4, 1.33, 1.0375, 1.2425, 1.3475, 1.3825,
    1.0625, 1.54, 1.5925, 0.9375, 1.177734, 1, 1.025, 1.0492, 1.169697,
    1.254, 0.875, 0.921081, 1.05, 1.0625, 1, 1.05, 1, 1.0125,
    1.025, 0.890315, 0.8295, 0.97966, 1.175, 0.8625, 1.63, 0.975, 2.87025,
    2.738625, 1.3475, 1, 0.952656, 0.8875, 1.0875, 1, 1.075, 0.8328,
    2.800463, 1.17, 1.12125, 1.11, 1.49, 2.45, 1.862719, 1.038375, 1.28325,
    2.07, 2.67575, 0.922059, 2.658075, 1.085, 2.49, 2.706, 1.4175, 1.37984,
};
/* LENGTH-TABLE-END */

/* REFIT-BEGIN */
/* final_length multiplies an allophone's length when it is the last before a
 * pause, a command or the end and isn't repeated: on the chip a vowel played
 * last is longer than inside a phrase (IY and AW 1.25 times) and TT and FF
 * shorter (0.64, 0.79). a command counts as on both units of the fifth
 * VoxProbe: AW and IY before a Pitch or Delay are as long as before a pause,
 * and the same length whatever the pause, and whether or not anything is
 * waiting in the buffer */
static const double final_length[] = {
    1.25, 1.175, 1, 1.175, 1, 1, 1, 1, 1.25,
    1, 1, 1.15, 1, 1, 0.9, 1.025, 0.9375, 1,
    1, 1.05, 0.9875, 1.1375, 1, 1, 1, 0.9125, 1,
    1, 1.1931, 1.515937, 0.925, 0.975, 1.0375, 0.8875, 1.025, 1.0625,
    1, 1, 1.2656, 1, 1, 1.159, 1, 1, 1,
    1, 0.975, 0.8875, 1.357187, 1.2, 1, 1.075, 1.2125, 1.0875,
    1, 1, 1, 1, 0.7875, 0.9625, 1, 1, 1,
    0.6375, 1.05, 1, 1, 1.3, 1, 0.8, 1, 0.925,
};
/* the copies after the first, when a sound is repeated, as a multiple of the
 * manual's length (see msa_expand()): the second play of a sound, copy1_length,
 * and the copies after it, copy_length. on both units of the fifth VoxProbe
 * the second play runs longer than the rest (AW 72ms then 70, SE 82 then 70,
 * BE 60 then 70) and most copies are the manual's 70ms; a stop's copies keep
 * their burst and release and put the rest of their time into the closure
 * (EB: a burst every 45ms) */
static const double copy1_length[] = {
    1.1, 0.7, 1, 1, 1, 1, 1.5, 1, 1.25,
    1, 1, 1, 1, 1, 0.75, 0.9875, 0.9065, 1,
    0.75, 1, 1, 1.0625, 0.65, 0.9625, 1, 0.9125, 1.0931,
    0.8625, 1.075, 0.7088, 0.931875, 0.9625, 0.5655, 0.9125, 1.05, 0.925,
    0.9375, 0.9375, 1, 0.6359, 0.975, 0.7625, 1.346, 0.825, 4.5,
    0.8, 1.25, 0.78, 0.95, 0.97625, 0.867188, 0.9875, 0.7, 0.8125,
    1, 1, 1, 1, 1, 1.165, 1, 1, 1,
    0.75, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const double copy_length[] = {
    1, 0.7, 1, 1, 1, 1, 1.125, 1, 1.075,
    1, 1, 1, 1, 1, 0.75, 0.9875, 0.93425, 1,
    0.75, 1, 1, 1.0625, 0.65, 0.9625, 1, 0.9125, 1.1925,
    0.8625, 1.075, 1.215, 0.931875, 0.9625, 1.098375, 0.9125, 1.05, 0.925,
    0.9375, 0.9375, 1, 0.6875, 0.975, 0.7625, 1.346, 0.825, 4,
    0.8, 0.875, 0.675, 0.95, 0.97625, 0.867188, 0.9875, 0.7, 0.8125,
    1, 1, 1, 1, 1, 1.835, 1, 1, 1,
    1.125, 1, 1, 1, 1, 1, 1, 1, 1,
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

/* a few voiced sounds are voiced faster than the vowels: on both units of
 * VoxRecord DH and ZH at 91Hz and ZZ at 89Hz where every vowel is at 87Hz
 * (the default pitch). each is a multiple of the voicing pitch, fitted to the
 * M score */
static const struct {
  short code;
  double scale;
} voicing_pitch[] = {
    {165, 1.0625}, /* JH */
    {168, 1.055}, /* ZH */
    {169, 1.05}, /* DH */
    {171, 1.04}, /* BO */
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
  short ms, glide;   /* hundredths of a millisecond */
  short freq[3];     /* FREQ_STEP_HZ steps */
  short vol[3];      /* eighths */
} vf_point;

static const vf_point vf_points[] = {
    {100, 100, {80, 399, 576}, {85, 56, 36}}, /* IH */
    {7995, 7995, {79, 404, 564}, {182, 140, 81}},
    {2425, 2425, {79, 399, 570}, {47, 45, 23}},
    {1150, 1150, {79, 399, 570}, {47, 45, 23}},
    {100, 100, {57, 262, 592}, {6, 2, 2}}, /* NO */
    {1239, 1239, {58, 255, 594}, {60, 14, 26}},
    {5719, 5719, {58, 263, 593}, {74, 16, 32}},
    {1152, 1152, {59, 264, 583}, {56, 10, 23}},
    {1150, 1150, {59, 264, 583}, {56, 10, 23}},
    {100, 100, {101, 200, 546}, {38, 40, 3}}, /* LO */
    {5831, 5831, {102, 200, 546}, {74, 83, 14}},
    {2285, 2285, {101, 198, 559}, {44, 48, 4}},
    {1150, 1150, {101, 198, 559}, {44, 48, 4}},
    {100, 100, {82, 305, 567}, {32, 19, 23}}, /* IYRR */
    {1215, 1215, {53, 504, 582}, {96, 64, 50}},
    {14859, 14859, {90, 344, 557}, {129, 103, 8}},
    {6789, 6789, {98, 302, 557}, {82, 76, 10}},
    {100, 100, {121, 343, 540}, {96, 64, 47}}, /* AXRR */
    {5804, 5804, {116, 335, 553}, {200, 133, 49}},
    {6886, 6886, {109, 321, 441}, {171, 121, 48}},
    {1146, 1146, {107, 316, 443}, {171, 120, 44}},
    {1150, 1150, {106, 314, 535}, {158, 121, 17}},
    {4523, 4523, {99, 306, 535}, {156, 129, 7}},
    {3504, 3504, {99, 302, 535}, {59, 54, 6}},
    {100, 100, {166, 212, 577}, {59, 54, 20}}, /* AWRR */
    {2376, 2376, {158, 224, 561}, {182, 170, 50}},
    {11565, 11565, {125, 273, 450}, {142, 127, 34}},
    {1141, 1141, {121, 281, 508}, {145, 126, 33}},
    {8002, 8002, {102, 301, 547}, {76, 77, 8}},
    {100, 100, {118, 208, 547}, {8, 7, 7}}, /* OWRR */
    {1289, 1289, {128, 197, 547}, {55, 62, 7}},
    {968, 968, {121, 201, 554}, {132, 161, 37}},
    {3645, 3645, {120, 217, 547}, {94, 108, 60}},
    {1074, 1074, {119, 226, 528}, {114, 129, 23}},
    {1134, 1134, {118, 234, 547}, {118, 133, 9}},
    {2287, 2287, {113, 247, 583}, {119, 127, 26}},
    {3423, 3423, {109, 267, 589}, {139, 129, 20}},
    {2281, 2281, {106, 282, 543}, {139, 128, 16}},
    {2274, 2274, {103, 298, 589}, {139, 127, 15}},
    {1262, 1262, {103, 302, 566}, {99, 90, 32}},
    {959, 959, {102, 300, 547}, {88, 80, 14}},
    {1329, 1329, {103, 208, 547}, {46, 41, 3}},
    {1150, 1150, {103, 208, 547}, {46, 41, 3}},
    {100, 100, {84, 229, 561}, {3, 4, 4}}, /* OWIY */
    {1315, 1315, {126, 199, 561}, {46, 49, 9}},
    {1223, 1223, {126, 206, 580}, {81, 98, 34}},
    {1138, 1138, {122, 209, 571}, {94, 104, 56}},
    {3337, 3337, {109, 264, 546}, {121, 126, 45}},
    {1138, 1138, {104, 281, 553}, {131, 125, 30}},
    {1081, 1081, {99, 298, 554}, {120, 108, 59}},
    {4662, 4662, {87, 364, 592}, {134, 101, 39}},
    {3480, 3480, {77, 413, 575}, {129, 105, 57}},
    {4582, 4582, {55, 477, 584}, {129, 69, 64}},
    {2291, 2291, {53, 509, 584}, {113, 68, 56}},
    {1268, 1268, {57, 509, 580}, {57, 41, 28}},
    {1150, 1150, {57, 509, 580}, {57, 41, 28}},
    {100, 100, {136, 395, 591}, {9, 3, 3}}, /* IYEH */
    {1369, 1369, {136, 487, 582}, {43, 31, 30}},
    {1056, 1056, {60, 499, 569}, {110, 85, 63}},
    {1267, 1267, {58, 495, 576}, {134, 80, 69}},
    {1147, 1147, {60, 489, 591}, {140, 84, 70}},
    {2250, 2250, {79, 469, 574}, {154, 86, 83}},
    {6829, 6829, {120, 409, 573}, {182, 103, 60}},
    {4530, 4530, {140, 385, 537}, {207, 123, 80}},
    {2519, 2519, {139, 386, 534}, {72, 42, 27}},
    {1150, 1150, {139, 386, 534}, {72, 42, 27}},
    {100, 100, {141, 383, 551}, {5, 3, 2}}, /* EHLL */
    {1047, 1047, {141, 383, 551}, {39, 26, 19}},
    {2420, 2420, {141, 383, 537}, {202, 117, 86}},
    {9172, 9172, {114, 264, 600}, {119, 90, 29}},
    {3482, 3482, {104, 221, 551}, {77, 83, 10}},
    {1147, 1147, {141, 220, 551}, {42, 49, 4}},
    {1150, 1150, {141, 220, 551}, {42, 49, 4}},
    {100, 100, {49, 255, 571}, {34, 23, 20}}, /* IYUW */
    {1244, 1244, {47, 496, 581}, {112, 62, 46}},
    {1108, 1108, {49, 504, 572}, {148, 79, 64}},
    {2298, 2298, {59, 466, 582}, {117, 66, 66}},
    {2251, 2251, {60, 419, 571}, {122, 89, 48}},
    {1023, 1023, {70, 403, 540}, {126, 116, 60}},
    {2455, 2455, {66, 364, 552}, {106, 73, 27}},
    {5579, 5579, {76, 275, 501}, {109, 101, 31}},
    {2300, 2300, {77, 243, 534}, {120, 116, 25}},
    {1212, 1212, {74, 242, 582}, {71, 68, 5}},
    {1150, 1150, {74, 242, 582}, {71, 68, 5}},
    {100, 100, {154, 275, 542}, {18, 17, 12}}, /* AXUW */
    {1099, 1099, {121, 343, 538}, {108, 70, 52}},
    {1164, 1164, {121, 342, 542}, {197, 126, 93}},
    {11541, 11541, {156, 259, 555}, {185, 137, 70}},
    {2246, 2246, {117, 249, 534}, {149, 122, 30}},
    {1156, 1156, {93, 246, 573}, {130, 109, 25}},
    {1154, 1154, {78, 242, 542}, {114, 110, 10}},
    {1158, 1158, {72, 242, 542}, {64, 59, 4}},
    {1150, 1150, {72, 242, 542}, {64, 59, 4}},
    {100, 100, {80, 380, 555}, {35, 17, 14}}, /* IHWW */
    {3495, 3495, {80, 391, 555}, {180, 136, 77}},
    {6975, 6975, {87, 309, 556}, {121, 103, 23}},
    {5700, 5700, {91, 243, 508}, {105, 107, 30}},
    {1147, 1147, {93, 231, 555}, {98, 106, 13}},
    {4693, 4693, {98, 185, 555}, {54, 73, 18}},
    {1216, 1216, {80, 183, 555}, {26, 39, 9}},
    {1150, 1150, {80, 183, 555}, {26, 39, 9}},
    {100, 100, {155, 357, 496}, {8, 6, 6}}, /* AYWW */
    {2429, 2429, {162, 381, 595}, {155, 78, 17}},
    {6824, 6824, {144, 319, 481}, {195, 113, 38}},
    {5752, 5752, {128, 255, 508}, {137, 107, 35}},
    {1147, 1147, {123, 243, 617}, {128, 107, 19}},
    {5851, 5851, {105, 186, 561}, {58, 79, 20}},
    {1177, 1177, {155, 180, 496}, {37, 48, 10}},
    {1150, 1150, {155, 180, 496}, {37, 48, 10}},
    {100, 100, {129, 197, 570}, {52, 56, 7}}, /* OWWW */
    {1198, 1198, {125, 206, 577}, {81, 96, 33}},
    {1148, 1148, {128, 203, 566}, {82, 105, 20}},
    {1146, 1146, {124, 203, 570}, {80, 102, 33}},
    {1147, 1147, {125, 198, 544}, {80, 103, 18}},
    {1143, 1143, {118, 198, 593}, {71, 96, 22}},
    {2292, 2292, {119, 190, 572}, {73, 94, 27}},
    {1146, 1146, {111, 189, 580}, {69, 91, 16}},
    {1148, 1148, {114, 191, 577}, {66, 89, 9}},
    {1146, 1146, {111, 188, 592}, {65, 88, 27}},
    {2294, 2294, {107, 187, 578}, {57, 81, 20}},
    {1147, 1147, {106, 188, 589}, {56, 77, 30}},
    {2287, 2287, {103, 181, 569}, {51, 70, 18}},
    {1241, 1241, {128, 180, 570}, {20, 32, 8}},
    {1150, 1150, {128, 180, 570}, {20, 32, 8}},
    {100, 100, {62, 342, 544}, {3, 2, 2}}, /* ZZ */
    {1145, 1145, {68, 340, 546}, {38, 28, 30}},
    {1139, 1139, {62, 342, 546}, {80, 55, 58}},
    {7893, 7893, {63, 344, 546}, {66, 42, 43}},
    {100, 100, {99, 382, 585}, {54, 37, 15}}, /* ZH */
    {6710, 6710, {100, 382, 570}, {53, 36, 19}},
    {100, 100, {64, 362, 536}, {31, 31, 10}}, /* DH */
    {9033, 9033, {56, 364, 532}, {20, 16, 6}},
    {100, 100, {142, 321, 546}, {10, 3, 1}}, /* BO */
    {1112, 1112, {62, 321, 546}, {55, 3, 5}},
    {1137, 1137, {62, 321, 546}, {59, 2, 6}},
    {1130, 1130, {94, 321, 546}, {68, 12, 13}},
    {1187, 1187, {142, 321, 546}, {161, 120, 62}},
    {3503, 3503, {142, 319, 546}, {65, 51, 25}},
    {100, 100, {100, 339, 567}, {3, 2, 3}}, /* DO */
    {1133, 1133, {60, 339, 567}, {37, 2, 5}},
    {2163, 2163, {62, 339, 567}, {60, 3, 6}},
    {1234, 1234, {100, 339, 567}, {84, 57, 33}},
    {1241, 1241, {100, 339, 573}, {63, 44, 22}},
    {1177, 1177, {100, 339, 567}, {13, 8, 2}},
    {1150, 1150, {100, 339, 567}, {13, 8, 2}},
    {100, 100, {58, 450, 548}, {20, 14, 8}}, /* ED */
    {1153, 1153, {55, 445, 601}, {32, 15, 12}},
    {1284, 1284, {47, 445, 601}, {16, 3, 1}},
    {1150, 1150, {47, 445, 601}, {16, 3, 1}},
    {100, 100, {60, 343, 563}, {27, 26, 12}}, /* OD */
    {1081, 1081, {59, 347, 558}, {25, 16, 8}},
    {1292, 1292, {62, 343, 563}, {13, 4, 2}},
    {1150, 1150, {62, 343, 563}, {13, 4, 2}},
    {100, 100, {58, 298, 575}, {24, 3, 4}}, /* GE */
    {1053, 1053, {60, 242, 582}, {51, 2, 6}},
    {1146, 1146, {60, 298, 572}, {51, 3, 5}},
    {1141, 1141, {61, 447, 589}, {46, 3, 7}},
    {1150, 1150, {61, 447, 589}, {46, 3, 7}},
    {100, 100, {57, 459, 544}, {45, 2, 4}}, /* GO */
    {1145, 1145, {59, 459, 566}, {55, 8, 4}},
    {1143, 1143, {60, 328, 586}, {54, 4, 7}},
    {2297, 2297, {64, 392, 582}, {24, 5, 3}},
    {100, 100, {60, 483, 566}, {64, 41, 9}}, /* EG */
    {1145, 1145, {59, 484, 563}, {44, 27, 5}},
    {1150, 1150, {59, 484, 563}, {44, 27, 5}},
    {100, 100, {64, 300, 571}, {8, 4, 3}}, /* OG */
    {1062, 1062, {64, 300, 553}, {60, 50, 8}},
    {2294, 2294, {60, 301, 566}, {44, 32, 7}},
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
      add_part(s, p[i].ms / 100.0, p[i].glide / 100.0,
               voiced_frame(FM(p[i].freq[0] * FREQ_STEP_HZ, p[i].freq[1] * FREQ_STEP_HZ, p[i].freq[2] * FREQ_STEP_HZ),
                            p[i].vol[0] / 8.0, p[i].vol[1] / 8.0, p[i].vol[2] / 8.0));
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

/* the effects traced from recordings that are not pure tones (the tone effects
 * are in fx_knots). each point is where it is reached (ms from the start of
 * the sound), its kind, a buzz's pitch, its level in tenths of a dB (-600 is
 * silence), up to three frequencies in FREQ_STEP_HZ steps (0 for none) and the
 * second and third's levels against the first in tenths of a dB. a tone is a pure sine; noise is noise around its frequency; a
 * buzz is voiced at its pitch with oscillators 1 to 3 on its formants. the
 * levels have the output filter's loss added back (see CHIP_RATE). A1 is three
 * formants (steady at 175Hz, rising 720 to 1645Hz, and rising faster from 120
 * to 2870Hz) */
enum { FX_TONE, FX_NOISE, FX_BUZZ };

typedef struct fx_point {
  short ms, kind, pitch;
  short db;        /* tenths of a dB */
  short freq[3];   /* FREQ_STEP_HZ steps */
  short rel[3];    /* tenths of a dB */
} fx_point;

static const fx_point fx_points[] = {
    /* A1 */
    {15, 2, 87, -600, {40, 164, 27}, {0, 150, -30}},
    {35, 2, 87, -161, {40, 164, 27}, {0, 150, -30}},
    {60, 2, 87, 47, {40, 171, 39}, {0, -53, -30}},
    {85, 2, 87, 182, {40, 178, 51}, {0, -167, -30}},
    {160, 2, 87, 79, {40, 198, 98}, {0, -43, 36}},
    {185, 2, 87, 57, {40, 205, 113}, {0, -29, -10}},
    {210, 2, 87, 76, {40, 212, 129}, {0, -108, -64}},
    {235, 2, 87, 67, {40, 219, 146}, {0, -12, -69}},
    {260, 2, 87, 84, {40, 226, 162}, {0, -119, -39}},
    {285, 2, 87, 85, {40, 234, 177}, {0, -30, -30}},
    {485, 2, 87, 29, {40, 277, 330}, {0, -30, -30}},
    {510, 2, 87, 23, {40, 283, 348}, {0, -34, -77}},
    {535, 2, 87, 15, {40, 288, 366}, {0, -26, -14}},
    {560, 2, 87, 27, {40, 295, 385}, {0, -75, -61}},
    {585, 2, 87, 39, {40, 303, 404}, {0, -35, -76}},
    {610, 2, 87, 20, {40, 309, 424}, {0, -36, -3}},
    {635, 2, 87, 48, {40, 317, 443}, {0, -94, -113}},
    {660, 2, 87, 26, {40, 323, 462}, {0, -11, -12}},
    {710, 2, 87, 33, {40, 336, 500}, {0, -55, -81}},
    {735, 2, 87, 26, {40, 342, 518}, {0, -55, -11}},
    {760, 2, 87, 54, {40, 346, 535}, {0, -72, -126}},
    {785, 2, 87, 28, {40, 350, 552}, {0, -39, 28}},
    {810, 2, 87, 35, {40, 354, 570}, {0, -65, -90}},
    {860, 2, 87, 21, {40, 363, 604}, {0, -58, -27}},
    {885, 2, 87, 52, {40, 367, 621}, {0, -68, -124}},
    {910, 2, 87, 79, {40, 371, 638}, {0, -104, -78}},
    {975, 2, 87, 60, {40, 380, 681}, {0, -30, -30}},
    /* A3. its buzz was traced at 470 to 490Hz, but on both units of the
     * fourth VoxProbe recording it repeats at the voicing frequency (60.2,
     * 87.1 and 150Hz at pitch 60, 88 and 150), so its buzz is at the default
     * voicing pitch here and follows Pitch: its items match both units
     * better (79 to 80%, 80 to 82%) */
    {5, 2, 88, -600, {131, 223, 455}, {0, 272, 365}},
    {20, 2, 88, -600, {131, 226, 458}, {0, 263, 380}},
    {25, 2, 88, -65, {132, 226, 544}, {0, -350, -121}},
    {30, 2, 88, -55, {132, 223, 544}, {0, -16, -131}},
    {35, 2, 88, -39, {132, 223, 544}, {0, -10, -139}},
    {80, 2, 88, -46, {132, 243, 565}, {0, 10, -140}},
    {120, 2, 88, -35, {143, 265, 568}, {0, 2, -152}},
    {125, 2, 88, -55, {144, 268, 422}, {0, 1, 36}},
    {135, 2, 88, -49, {147, 273, 422}, {0, -1, 27}},
    {520, 2, 88, -87, {331, 538, 779}, {0, 41, 60}},
    {545, 2, 88, -142, {334, 543, 786}, {0, 52, 66}},
    {550, 2, 88, -260, {333, 543, 783}, {0, 40, 65}},
    {555, 2, 88, -600, {297, 543, 781}, {0, -217, 65}},
    {570, 2, 88, -600, {231, 453, 679}, {0, 90, 166}},
    /* B9: traced again from unit B's VoxRecord recording every 2ms (8ms
     * windows), its three strongest tones followed by continuity, with its own
     * peaking filters (see sound_eq): all 14 recordings of both units 1.83 to
     * 1.43, the other pitches and bends 1.83 to 1.45 */
    {2, 0, 0, -600, {0, 0, 0}, {0, -600, -600}},
    {18, 0, 0, -600, {0, 0, 0}, {0, -600, -600}},
    {20, 0, 0, -247, {443, 61, 0}, {0, -258, -600}},
    {22, 0, 0, -101, {614, 416, 201}, {0, -6, -123}},
    {26, 0, 0, -8, {617, 412, 204}, {0, 2, -121}},
    {78, 0, 0, -51, {609, 409, 192}, {0, 5, -123}},
    {80, 0, 0, -600, {609, 174, 236}, {0, -600, -600}},
    {98, 0, 0, -600, {609, 172, 55}, {0, -600, -600}},
    {100, 0, 0, -395, {143, 227, 185}, {0, 58, 49}},
    {104, 0, 0, -119, {152, 227, 185}, {0, -600, -600}},
    {110, 0, 0, -278, {142, 227, 31}, {0, -600, -264}},
    {112, 0, 0, -329, {126, 227, 31}, {0, -600, -600}},
    {116, 0, 0, -108, {130, 227, 31}, {0, -600, -600}},
    {120, 0, 0, -194, {125, 227, 31}, {0, -600, -600}},
    {122, 0, 0, -139, {857, 570, 104}, {0, 24, -175}},
    {124, 0, 0, -54, {862, 571, 278}, {0, 27, -180}},
    {126, 0, 0, -600, {862, 572, 116}, {0, -600, -600}},
    {130, 0, 0, -600, {862, 573, 120}, {0, -600, -600}},
    {132, 0, 0, -28, {870, 573, 118}, {0, 26, -190}},
    {134, 0, 0, -28, {870, 573, 93}, {0, 26, -356}},
    {136, 0, 0, -28, {870, 573, 78}, {0, 26, -244}},
    {138, 0, 0, -600, {870, 573, 115}, {0, -600, -600}},
    {140, 0, 0, -600, {870, 572, 119}, {0, -600, -600}},
    {142, 0, 0, -26, {869, 573, 120}, {0, 23, -128}},
    {144, 0, 0, -26, {869, 573, 119}, {0, 24, -228}},
    {146, 0, 0, -27, {869, 573, 163}, {0, 25, -296}},
    {148, 0, 0, -27, {869, 573, 109}, {0, 25, -167}},
    {150, 0, 0, -600, {869, 573, 117}, {0, -600, -600}},
    {152, 0, 0, -600, {869, 573, 119}, {0, -600, -600}},
    {154, 0, 0, -28, {870, 573, 119}, {0, 26, -149}},
    {156, 0, 0, -27, {869, 573, 115}, {0, 25, -266}},
    {158, 0, 0, -28, {869, 573, 40}, {0, 27, -365}},
    {160, 0, 0, -34, {870, 571, 115}, {0, 21, -128}},
    {162, 0, 0, -600, {870, 571, 118}, {0, -600, -600}},
    {190, 0, 0, -600, {870, 62, 164}, {0, -600, -600}},
    {192, 0, 0, -364, {117, 159, 199}, {0, 49, 67}},
    {194, 0, 0, -600, {117, 171, 199}, {0, -600, -600}},
    {206, 0, 0, -600, {117, 188, 200}, {0, -600, -600}},
    {208, 0, 0, -114, {203, 797, 409}, {0, 76, 101}},
    {264, 0, 0, -130, {205, 799, 412}, {0, 107, 129}},
    {268, 0, 0, -212, {204, 800, 412}, {0, 189, 210}},
    {270, 0, 0, -600, {204, 799, 412}, {0, -600, -600}},
    {274, 0, 0, -600, {204, 800, 412}, {0, -600, -600}},
    {276, 0, 0, -200, {202, 799, 412}, {0, 162, 182}},
    {280, 0, 0, -268, {198, 799, 412}, {0, 217, 238}},
    {282, 0, 0, -600, {198, 800, 412}, {0, -600, -600}},
    {286, 0, 0, -600, {198, 801, 412}, {0, -600, -600}},
    {288, 0, 0, -267, {205, 800, 412}, {0, 180, 207}},
    {292, 0, 0, -323, {213, 799, 412}, {0, 214, 242}},
    {294, 0, 0, -600, {213, 799, 412}, {0, -600, -600}},
    {298, 0, 0, -600, {213, 801, 412}, {0, -600, -600}},
    {300, 0, 0, -422, {172, 799, 413}, {0, 242, 265}},
    {302, 0, 0, -462, {149, 798, 412}, {0, 250, 271}},
    {304, 0, 0, -420, {186, 104, 414}, {0, -83, 176}},
    {306, 0, 0, -600, {186, 82, 414}, {0, -600, -600}},
    {319, 0, 0, -600, {186, 47, 414}, {0, -600, -600}},
    /* C0 */
    {5, 0, 0, -600, {55, 226, 352}, {0, -149, -600}},
    {20, 0, 0, -600, {55, 226, 352}, {0, -1, -600}},
    {25, 0, 0, -42, {528, 226, 352}, {0, -54, -600}},
    {30, 0, 0, -39, {528, 469, 352}, {0, -142, -600}},
    {35, 0, 0, -8, {528, 469, 352}, {0, -161, -600}},
    {40, 0, 0, -21, {469, 469, 352}, {0, -22, -600}},
    {45, 0, 0, -27, {468, 469, 352}, {0, 0, -26}},
    {55, 0, 0, -40, {408, 403, 531}, {0, -135, -106}},
    {70, 0, 0, -11, {349, 403, 531}, {0, -118, -90}},
    {75, 0, 0, -10, {289, 403, 531}, {0, -35, -49}},
    {80, 0, 0, -14, {289, 524, 291}, {0, -69, -146}},
    {85, 0, 0, 5, {287, 346, 531}, {0, -6, -29}},
    {90, 0, 0, -14, {230, 231, 511}, {0, -117, -93}},
    {95, 0, 0, 14, {226, 229, 511}, {0, -81, -59}},
    {100, 0, 0, 7, {178, 174, 511}, {0, -102, -80}},
    {105, 0, 0, 17, {174, 132, 511}, {0, -137, 48}},
    {110, 0, 0, 23, {165, 92, 56}, {0, -123, -133}},
    {115, 0, 0, 2, {148, 70, 56}, {0, 1, -600}},
    {130, 0, 0, 25, {114, 70, 56}, {0, -600, -600}},
    {135, 0, 0, 31, {83, 70, 56}, {0, -600, -600}},
    {145, 0, 0, 36, {77, 104, 56}, {0, -600, -600}},
    {150, 0, 0, 20, {45, 104, 133}, {0, -600, -600}},
    {165, 0, 0, -51, {45, 104, 133}, {0, -600, -600}},
    {170, 0, 0, -62, {68, 104, 133}, {0, -63, -600}},
    {175, 0, 0, -58, {75, 136, 133}, {0, -1, -600}},
    {180, 0, 0, -70, {68, 104, 133}, {0, -60, -147}},
    {185, 0, 0, -99, {75, 136, 179}, {0, -27, -61}},
    {190, 0, 0, -90, {75, 136, 205}, {0, -64, -99}},
    {200, 0, 0, -134, {57, 110, 179}, {0, -82, -138}},
    {205, 0, 0, -127, {57, 129, 201}, {0, -93, -120}},
    {210, 0, 0, -145, {57, 110, 201}, {0, 0, -1}},
    {215, 0, 0, -173, {49, 112, 201}, {0, -88, -600}},
    {220, 0, 0, -171, {49, 112, 201}, {0, 0, -600}},
    {225, 0, 0, -600, {61, 112, 201}, {0, -107, -600}},
    {245, 0, 0, -600, {55, 105, 201}, {0, -600, -600}},
    /* C2 */
    {5, 0, 0, -600, {53, 49, 222}, {0, -600, -600}},
    {30, 0, 0, -600, {109, 208, 222}, {0, -51, -1}},
    {35, 0, 0, -68, {114, 206, 222}, {0, -1, -1}},
    {45, 0, 0, -64, {116, 206, 39}, {0, -28, -110}},
    {50, 0, 0, -8, {116, 208, 504}, {0, -1, 45}},
    {55, 0, 0, -14, {116, 209, 501}, {0, -27, -28}},
    {60, 0, 0, -15, {116, 210, 40}, {0, -26, -103}},
    {65, 0, 0, 9, {118, 213, 459}, {0, -29, -75}},
    {75, 0, 0, -16, {120, 225, 458}, {0, -32, -64}},
    {80, 0, 0, 7, {121, 230, 55}, {0, -43, -94}},
    {85, 0, 0, -10, {124, 233, 414}, {0, 0, 19}},
    {90, 0, 0, -19, {125, 225, 55}, {0, -83, -109}},
    {100, 0, 0, -16, {129, 254, 55}, {0, 0, 1}},
    {135, 0, 0, -97, {128, 267, 56}, {0, -35, -99}},
    {140, 0, 0, -95, {126, 267, 78}, {0, -70, -124}},
    {165, 0, 0, -257, {102, 263, 57}, {0, -143, 0}},
    {170, 0, 0, -252, {102, 263, 57}, {0, -91, -600}},
    {175, 0, 0, -227, {102, 40, 57}, {0, 0, -600}},
    {180, 0, 0, -93, {755, 40, 57}, {0, -600, -600}},
    {190, 0, 0, -39, {761, 40, 57}, {0, -131, -600}},
    {195, 0, 0, -44, {761, 40, 141}, {0, -266, -600}},
    {205, 0, 0, 3, {762, 44, 141}, {0, -132, -600}},
    {215, 0, 0, -10, {762, 49, 141}, {0, -265, -600}},
    {220, 0, 0, -8, {762, 68, 141}, {0, -175, -228}},
    {225, 0, 0, -207, {382, 72, 141}, {0, -66, -105}},
    {230, 0, 0, -213, {380, 72, 141}, {0, -72, -107}},
    {235, 0, 0, -224, {60, 108, 141}, {0, -29, -92}},
    {240, 0, 0, -600, {56, 108, 110}, {0, -146, 0}},
    {250, 0, 0, -600, {53, 108, 110}, {0, -600, -600}},
    /* C3 */
    {5, 0, 0, -600, {51, 112, 94}, {0, -600, -600}},
    {20, 0, 0, -600, {61, 112, 94}, {0, -74, -600}},
    {25, 0, 0, -94, {717, 49, 94}, {0, -117, -600}},
    {30, 0, 0, -83, {721, 40, 94}, {0, -191, -600}},
    {40, 0, 0, -30, {722, 43, 94}, {0, -118, -600}},
    {45, 0, 0, -32, {723, 40, 94}, {0, -257, -600}},
    {50, 0, 0, -9, {722, 40, 94}, {0, -600, -600}},
    {115, 0, 0, 38, {770, 40, 94}, {0, -600, -600}},
    {120, 0, 0, 39, {791, 47, 94}, {0, -600, -600}},
    {180, 0, 0, 43, {863, 47, 94}, {0, -600, -600}},
    {185, 0, 0, 44, {863, 47, 94}, {0, -168, -600}},
    {190, 0, 0, 44, {863, 47, 94}, {0, -168, -600}},
    {195, 0, 0, 44, {863, 47, 94}, {0, -264, -600}},
    {200, 0, 0, 5, {863, 77, 94}, {0, -189, -600}},
    {205, 0, 0, 7, {861, 77, 94}, {0, -302, -600}},
    {210, 0, 0, 6, {861, 81, 94}, {0, -311, -168}},
    {215, 0, 0, 6, {861, 81, 94}, {0, -272, -314}},
    {220, 0, 0, -189, {53, 862, 130}, {0, 103, -86}},
    {225, 0, 0, -203, {53, 862, 169}, {0, 165, -104}},
    {230, 0, 0, -205, {53, 862, 130}, {0, 167, -89}},
    {235, 0, 0, -203, {55, 856, 130}, {0, 101, -80}},
    {240, 0, 0, -600, {55, 227, 130}, {0, -134, -600}},
    {250, 0, 0, -600, {55, 226, 130}, {0, -135, -600}},
    /* C4: traced again from unit B's VoxRecord recording every 2ms (8ms
     * windows), the one tone it is, with its own peaking filters (see sound_eq);
     * its pulsing with the voicing and its noise stay as they were (see
     * fx_pulse, fx_noise): all 13 recordings of both units 1.30 to 1.05, the
     * other pitches and bends 1.28 to 1.06 */
    {2, 0, 0, -600, {0, 0, 0}, {0, -600, -600}},
    {16, 0, 0, -574, {48, 0, 0}, {0, -600, -600}},
    {18, 0, 0, -361, {219, 0, 0}, {0, -600, -600}},
    {24, 0, 0, -600, {215, 0, 0}, {0, -600, -600}},
    {26, 0, 0, -246, {208, 0, 0}, {0, -600, -600}},
    {28, 0, 0, -147, {217, 0, 0}, {0, -600, -600}},
    {34, 0, 0, -277, {218, 0, 0}, {0, -600, -600}},
    {36, 0, 0, -545, {44, 0, 0}, {0, -600, -600}},
    {38, 0, 0, -494, {42, 0, 0}, {0, -600, -600}},
    {40, 0, 0, -140, {213, 0, 0}, {0, -600, -600}},
    {42, 0, 0, -73, {218, 0, 0}, {0, -600, -600}},
    {48, 0, 0, -255, {221, 0, 0}, {0, -600, -600}},
    {50, 0, 0, -361, {295, 0, 0}, {0, -600, -600}},
    {52, 0, 0, -138, {209, 0, 0}, {0, -600, -600}},
    {54, 0, 0, -47, {218, 0, 0}, {0, -600, -600}},
    {58, 0, 0, -92, {220, 0, 0}, {0, -600, -600}},
    {62, 0, 0, -279, {249, 0, 0}, {0, -600, -600}},
    {66, 0, 0, -27, {218, 0, 0}, {0, -600, -600}},
    {72, 0, 0, -208, {218, 0, 0}, {0, -600, -600}},
    {74, 0, 0, -253, {177, 0, 0}, {0, -600, -600}},
    {76, 0, 0, -73, {213, 0, 0}, {0, -600, -600}},
    {78, 0, 0, -22, {218, 0, 0}, {0, -600, -600}},
    {84, 0, 0, -251, {216, 0, 0}, {0, -600, -600}},
    {86, 0, 0, -198, {180, 0, 0}, {0, -600, -600}},
    {90, 0, 0, -20, {219, 0, 0}, {0, -600, -600}},
    {96, 0, 0, -278, {221, 0, 0}, {0, -600, -600}},
    {98, 0, 0, -128, {448, 0, 0}, {0, -600, -600}},
    {100, 0, 0, -36, {215, 0, 0}, {0, -600, -600}},
    {102, 0, 0, -25, {442, 0, 0}, {0, -600, -600}},
    {108, 0, 0, -240, {474, 0, 0}, {0, -600, -600}},
    {112, 0, 0, -7, {442, 0, 0}, {0, -600, -600}},
    {118, 0, 0, -199, {442, 0, 0}, {0, -600, -600}},
    {124, 0, 0, -3, {440, 0, 0}, {0, -600, -600}},
    {130, 0, 0, -241, {441, 0, 0}, {0, -600, -600}},
    {136, 0, 0, 0, {437, 0, 0}, {0, -600, -600}},
    {142, 0, 0, -271, {440, 0, 0}, {0, -600, -600}},
    {146, 0, 0, -11, {435, 0, 0}, {0, -600, -600}},
    {152, 0, 0, -172, {440, 0, 0}, {0, -600, -600}},
    {154, 0, 0, -239, {708, 0, 0}, {0, -600, -600}},
    {158, 0, 0, -15, {684, 0, 0}, {0, -600, -600}},
    {164, 0, 0, -216, {678, 0, 0}, {0, -600, -600}},
    {170, 0, 0, -13, {683, 0, 0}, {0, -600, -600}},
    {176, 0, 0, -268, {702, 0, 0}, {0, -600, -600}},
    {180, 0, 0, -33, {681, 0, 0}, {0, -600, -600}},
    {182, 0, 0, -15, {681, 0, 0}, {0, -600, -600}},
    {186, 0, 0, -153, {681, 0, 0}, {0, -600, -600}},
    {188, 0, 0, -600, {681, 0, 0}, {0, -600, -600}},
    {190, 0, 0, -102, {678, 0, 0}, {0, -600, -600}},
    {192, 0, 0, -22, {681, 0, 0}, {0, -600, -600}},
    {200, 0, 0, -237, {708, 0, 0}, {0, -600, -600}},
    {204, 0, 0, -20, {682, 0, 0}, {0, -600, -600}},
    {210, 0, 0, -247, {676, 0, 0}, {0, -600, -600}},
    {212, 0, 0, -541, {58, 0, 0}, {0, -600, -600}},
    {214, 0, 0, -110, {680, 0, 0}, {0, -600, -600}},
    {216, 0, 0, -52, {681, 0, 0}, {0, -600, -600}},
    {222, 0, 0, -255, {683, 0, 0}, {0, -600, -600}},
    {224, 0, 0, -555, {54, 0, 0}, {0, -600, -600}},
    {226, 0, 0, -561, {52, 0, 0}, {0, -600, -600}},
    {228, 0, 0, -166, {680, 0, 0}, {0, -600, -600}},
    {230, 0, 0, -133, {682, 0, 0}, {0, -600, -600}},
    {234, 0, 0, -261, {684, 0, 0}, {0, -600, -600}},
    {236, 0, 0, -600, {684, 0, 0}, {0, -600, -600}},
    {238, 0, 0, -590, {52, 0, 0}, {0, -600, -600}},
    {249, 0, 0, -600, {52, 0, 0}, {0, -600, -600}},
    /* C6 */
    {5, 0, 0, -600, {53, 109, 133}, {0, -600, -600}},
    {25, 0, 0, -600, {459, 226, 136}, {0, -147, -151}},
    {30, 0, 0, -221, {463, 224, 227}, {0, -105, -157}},
    {35, 0, 0, -134, {466, 52, 136}, {0, -75, -98}},
    {40, 0, 0, -130, {463, 52, 358}, {0, -148, -173}},
    {55, 0, 0, -82, {461, 52, 358}, {0, -31, -600}},
    {75, 0, 0, -52, {461, 336, 257}, {0, -26, -600}},
    {90, 0, 0, -19, {396, 336, 257}, {0, -9, -15}},
    {100, 0, 0, -6, {314, 261, 257}, {0, -142, -148}},
    {110, 0, 0, 11, {243, 261, 638}, {0, -98, -24}},
    {130, 0, 0, -14, {206, 148, 146}, {0, 0, 0}},
    {135, 0, 0, -23, {206, 443, 146}, {0, 27, 0}},
    {140, 0, 0, -28, {206, 443, 208}, {0, 18, -70}},
    {145, 0, 0, -19, {440, 443, 208}, {0, -18, -62}},
    {150, 0, 0, -18, {442, 443, 383}, {0, -91, -131}},
    {160, 0, 0, -20, {440, 205, 208}, {0, -51, -85}},
    {165, 0, 0, -30, {207, 205, 148}, {0, -14, -103}},
    {185, 0, 0, 12, {206, 146, 635}, {0, -137, -50}},
    {200, 0, 0, 0, {279, 250, 661}, {0, -5, 74}},
    {205, 0, 0, -4, {303, 245, 315}, {0, -107, -127}},
    {240, 0, 0, -60, {463, 250, 273}, {0, -600, -600}},
    {280, 0, 0, -161, {464, 130, 121}, {0, -75, -110}},
    {285, 0, 0, -600, {55, 130, 121}, {0, -80, -103}},
    {305, 0, 0, -600, {55, 109, 129}, {0, -130, -600}},
    /* C7 is one tone that sweeps up from 500Hz to 2.7kHz and drops back five
     * times in 300ms, faster each time: traced every 2ms */
    {2, 0, 0, -600, {0, 0, 0}, {0, -600, -600}},
    {20, 0, 0, -554, {60, 0, 0}, {0, -600, -600}},
    {22, 0, 0, -322, {114, 0, 0}, {0, -600, -600}},
    {26, 0, 0, -260, {120, 0, 0}, {0, -600, -600}},
    {30, 0, 0, -428, {118, 0, 0}, {0, -600, -600}},
    {32, 0, 0, -570, {42, 0, 0}, {0, -600, -600}},
    {34, 0, 0, -462, {46, 0, 0}, {0, -600, -600}},
    {36, 0, 0, -239, {111, 0, 0}, {0, -600, -600}},
    {38, 0, 0, -178, {117, 0, 0}, {0, -600, -600}},
    {44, 0, 0, -378, {119, 0, 0}, {0, -600, -600}},
    {46, 0, 0, -497, {36, 0, 0}, {0, -600, -600}},
    {48, 0, 0, -270, {105, 0, 0}, {0, -600, -600}},
    {50, 0, 0, -158, {116, 0, 0}, {0, -600, -600}},
    {56, 0, 0, -275, {120, 0, 0}, {0, -600, -600}},
    {58, 0, 0, -494, {28, 0, 0}, {0, -600, -600}},
    {60, 0, 0, -262, {105, 0, 0}, {0, -600, -600}},
    {64, 0, 0, -112, {120, 0, 0}, {0, -600, -600}},
    {68, 0, 0, -228, {139, 0, 0}, {0, -600, -600}},
    {70, 0, 0, -337, {194, 0, 0}, {0, -600, -600}},
    {74, 0, 0, -45, {235, 0, 0}, {0, -600, -600}},
    {82, 0, 0, -246, {312, 0, 0}, {0, -600, -600}},
    {84, 0, 0, -63, {360, 0, 0}, {0, -600, -600}},
    {86, 0, 0, -6, {374, 0, 0}, {0, -600, -600}},
    {92, 0, 0, -214, {420, 0, 0}, {0, -600, -600}},
    {98, 0, 0, 0, {511, 0, 0}, {0, -600, -600}},
    {104, 0, 0, -255, {555, 0, 0}, {0, -600, -600}},
    {108, 0, 0, -28, {623, 0, 0}, {0, -600, -600}},
    {110, 0, 0, -39, {603, 0, 0}, {0, -600, -600}},
    {116, 0, 0, -327, {349, 0, 0}, {0, -600, -600}},
    {118, 0, 0, -198, {124, 0, 0}, {0, -600, -600}},
    {122, 0, 0, -88, {155, 0, 0}, {0, -600, -600}},
    {126, 0, 0, -229, {188, 0, 0}, {0, -600, -600}},
    {132, 0, 0, -25, {272, 0, 0}, {0, -600, -600}},
    {136, 0, 0, -141, {307, 0, 0}, {0, -600, -600}},
    {140, 0, 0, -13, {372, 0, 0}, {0, -600, -600}},
    {154, 0, 0, -77, {503, 0, 0}, {0, -600, -600}},
    {158, 0, 0, -34, {450, 0, 0}, {0, -600, -600}},
    {162, 0, 0, -151, {358, 0, 0}, {0, -600, -600}},
    {164, 0, 0, -112, {134, 0, 0}, {0, -600, -600}},
    {166, 0, 0, -101, {131, 0, 0}, {0, -600, -600}},
    {202, 0, 0, -34, {475, 0, 0}, {0, -600, -600}},
    {206, 0, 0, -47, {364, 0, 0}, {0, -600, -600}},
    {208, 0, 0, -129, {174, 0, 0}, {0, -600, -600}},
    {212, 0, 0, -106, {143, 0, 0}, {0, -600, -600}},
    {220, 0, 0, -63, {188, 0, 0}, {0, -600, -600}},
    {224, 0, 0, -87, {176, 0, 0}, {0, -600, -600}},
    {234, 0, 0, -64, {235, 0, 0}, {0, -600, -600}},
    {236, 0, 0, -26, {296, 0, 0}, {0, -600, -600}},
    {244, 0, 0, -45, {293, 0, 0}, {0, -600, -600}},
    {246, 0, 0, -14, {355, 0, 0}, {0, -600, -600}},
    {252, 0, 0, -36, {276, 0, 0}, {0, -600, -600}},
    {254, 0, 0, -95, {174, 0, 0}, {0, -600, -600}},
    {258, 0, 0, -106, {124, 0, 0}, {0, -600, -600}},
    {264, 0, 0, -127, {106, 0, 0}, {0, -600, -600}},
    {268, 0, 0, -74, {167, 0, 0}, {0, -600, -600}},
    {276, 0, 0, -80, {161, 0, 0}, {0, -600, -600}},
    {280, 0, 0, -43, {214, 0, 0}, {0, -600, -600}},
    {288, 0, 0, -42, {223, 0, 0}, {0, -600, -600}},
    {290, 0, 0, -47, {284, 0, 0}, {0, -600, -600}},
    {294, 0, 0, -12, {280, 0, 0}, {0, -600, -600}},
    {302, 0, 0, -73, {195, 0, 0}, {0, -600, -600}},
    {304, 0, 0, -116, {111, 0, 0}, {0, -600, -600}},
    {306, 0, 0, -129, {98, 0, 0}, {0, -600, -600}},
    {322, 0, 0, -139, {102, 0, 0}, {0, -600, -600}},
    {330, 0, 0, -188, {84, 0, 0}, {0, -600, -600}},
    {332, 0, 0, -158, {103, 0, 0}, {0, -600, -600}},
    {334, 0, 0, -221, {82, 0, 0}, {0, -600, -600}},
    {336, 0, 0, -204, {86, 0, 0}, {0, -600, -600}},
    {340, 0, 0, -164, {128, 0, 0}, {0, -600, -600}},
    {354, 0, 0, -211, {100, 0, 0}, {0, -600, -600}},
    {358, 0, 0, -250, {107, 0, 0}, {0, -600, -600}},
    {360, 0, 0, -241, {140, 0, 0}, {0, -600, -600}},
    {362, 0, 0, -226, {105, 0, 0}, {0, -600, -600}},
    {372, 0, 0, -256, {109, 0, 0}, {0, -600, -600}},
    {376, 0, 0, -396, {127, 0, 0}, {0, -600, -600}},
    {382, 0, 0, -327, {110, 0, 0}, {0, -600, -600}},
    {388, 0, 0, -600, {127, 0, 0}, {0, -600, -600}},
    {405, 0, 0, -600, {127, 0, 0}, {0, -600, -600}},
    /* M2 */
    {5, 2, 128, -600, {123, 385, 651}, {0, 376, 435}},
    {10, 2, 177, -600, {123, 389, 652}, {0, 381, 446}},
    {15, 2, 498, -600, {123, 389, 654}, {0, 42, 83}},
    {20, 2, 498, -110, {123, 388, 654}, {0, 22, 66}},
    {25, 2, 85, -91, {121, 388, 654}, {0, 22, 66}},
    {45, 2, 87, -43, {114, 393, 666}, {0, 29, 71}},
    {155, 2, 87, -71, {168, 446, 722}, {0, 32, 72}},
    {285, 2, 87, -66, {225, 501, 779}, {0, 59, 92}},
    {410, 2, 87, -54, {163, 441, 717}, {0, 28, 69}},
    {525, 2, 87, -64, {111, 390, 662}, {0, 28, 75}},
    {530, 2, 103, -67, {111, 390, 662}, {0, 25, 72}},
    {535, 2, 492, -136, {112, 390, 662}, {0, 24, 72}},
    {540, 2, 83, -138, {112, 390, 662}, {0, 27, 72}},
    {545, 2, 492, -222, {112, 390, 662}, {0, 47, 94}},
    {550, 2, 492, -600, {112, 390, 662}, {0, 54, 99}},
    {555, 2, 492, -600, {112, 389, 661}, {0, 382, 438}},
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
  const double g = pow(10, p->db / 200.0);
  frame f;
  memset(&f, 0, sizeof(f));
  if (p->kind == FX_NOISE) {
    f = noise_frame(lGlottal, p->freq[0] * FREQ_STEP_HZ * 0.85, p->freq[0] * FREQ_STEP_HZ * 1.15, 24 * g, 18 * g, 200);
  } else {
    const double base = p->freq[1] > 0 ? 24 : 28;
    for (int i = 0; i < 3; i++) {
      f.freq[i] = p->freq[i] * FREQ_STEP_HZ;
      if (p->freq[i] > 0)
        f.vol[i] = base * g * pow(10, p->rel[i] / 200.0);
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
  if (p->db <= -600)
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
      on[i] = p[i].db > -600 && p[i].freq[j] > 0 && (j == 0 || p[i].rel[j] > -590);
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

/* C8 and C9 are a band of noise swept up and down every 19.5ms (see
 * SWEEP_LO_HZ), each random value held 0.75ms (a band with edges, as on the
 * chip), at these levels in dB every 5ms (C8 is 80ms on both units). the
 * sweep keeps time with Speed, not Pitch */
static const double c8_db[] = {
    -16.2, -11.8, -9.5, -5.5, -1.1, -0.5, -0.2, -0.1, -0.3, -0.5,
    -0.5, -0.2, -0.9, -1.3, -1.8, -5.2,
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
  f.sweep = true;
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
 * half milliseconds, its frequency in FREQ_STEP_HZ steps and its volume in eighths,
 * glided between in straight lines (only the knots a straight line can't pass
 * close enough to are kept). before its first knot a tone sits silent at its
 * first frequency */
static const short fx_knots[] = {
    2, 181, 0, 10, 181, 0, 12, 181, 20, 34, 183, 248, 130, 183, 241, 156, 185, 51, 158, 185, 0, 164, 185, 0,
    2, 93, 0, 34, 93, 0, 36, 93, 33, 58, 103, 56, 106, 123, 68, 116, 130, 84, 118, 130, 0, 164, 130, 0,
    2, 145, 0, 6, 145, 0, 8, 145, 3, 10, 155, 6, 16, 155, 14, 24, 156, 14, 32, 151, 20, 40, 159, 26,
    56, 159, 28, 70, 167, 27, 100, 168, 27, 116, 177, 27, 134, 175, 26, 138, 163, 21, 144, 160, 19, 152, 170, 21,
    156, 175, 12, 158, 175, 0, 172, 175, 0, 2, 252, 0, 12, 252, 0, 14, 252, 17, 16, 252, 26, 22, 252, 106,
    32, 252, 231, 130, 252, 248, 156, 252, 29, 158, 252, 20, 164, 252, 3, 172, 252, 0, 2, 296, 0, 8, 296, 1,
    12, 296, 8, 14, 296, 20, 16, 296, 33, 18, 296, 57, 24, 297, 149, 32, 297, 246, 134, 297, 248, 144, 297, 170,
    150, 297, 94, 152, 298, 78, 158, 298, 43, 160, 298, 57, 162, 298, 0, 190, 298, 0, 2, 167, 0, 12, 167, 0,
    14, 167, 8, 16, 149, 6, 24, 150, 19, 32, 156, 25, 140, 86, 28, 146, 86, 26, 150, 95, 21, 152, 95, 19,
    160, 86, 21, 166, 94, 18, 180, 96, 2, 182, 96, 5, 184, 96, 0, 190, 96, 0, 2, 387, 0, 30, 387, 0,
    32, 387, 29, 40, 380, 41, 48, 366, 55, 50, 366, 0, 190, 366, 0, 2, 366, 0, 6, 366, 0, 8, 366, 8,
    14, 366, 36, 26, 366, 195, 132, 366, 226, 160, 367, 9, 162, 366, 11, 164, 366, 0, 192, 366, 0, 2, 145, 0,
    6, 145, 0, 40, 158, 59, 114, 176, 26, 192, 161, 0, 2, 79, 0, 34, 79, 0, 36, 79, 10, 130, 84, 35,
    146, 68, 14, 164, 68, 5, 166, 68, 0, 192, 68, 0, 2, 434, 1, 6, 434, 6, 8, 434, 15, 10, 434, 27,
    28, 435, 248, 130, 435, 234, 158, 434, 10, 160, 434, 0, 166, 434, 0, 2, 143, 0, 6, 143, 0, 8, 143, 5,
    12, 138, 7, 18, 134, 25, 22, 127, 31, 28, 125, 34, 34, 143, 38, 78, 197, 17, 108, 197, 8, 118, 195, 0,
    166, 195, 0, 2, 105, 0, 42, 105, 0, 44, 105, 42, 46, 106, 32, 118, 164, 29, 128, 168, 32, 136, 160, 25,
    144, 166, 23, 146, 166, 0, 166, 166, 0, 2, 64, 0, 66, 64, 0, 68, 64, 16, 74, 64, 18, 94, 72, 40,
    126, 84, 36, 136, 83, 33, 138, 85, 28, 140, 101, 30, 142, 101, 0, 166, 101, 0, 2, 337, 0, 74, 337, 0,
    76, 337, 14, 100, 373, 24, 102, 373, 0, 166, 373, 0, 2, 79, 1, 10, 79, 6, 14, 79, 15, 20, 70, 21,
    24, 69, 23, 26, 71, 24, 30, 82, 30, 34, 86, 34, 128, 87, 34, 138, 80, 32, 152, 98, 34, 160, 112, 16,
    166, 112, 9, 170, 113, 8, 172, 113, 0, 178, 113, 0, 2, 503, 0, 6, 503, 3, 10, 503, 19, 14, 503, 50,
    24, 503, 199, 134, 504, 200, 138, 504, 143, 154, 503, 22, 158, 504, 8, 166, 504, 0, 178, 504, 0, 2, 185, 0,
    18, 185, 0, 20, 185, 28, 34, 175, 33, 128, 173, 33, 134, 169, 32, 136, 185, 0, 138, 185, 14, 144, 190, 17,
    152, 179, 17, 158, 168, 14, 160, 168, 0, 178, 168, 0, 2, 380, 0, 8, 380, 5, 16, 375, 23, 26, 340, 17,
    28, 389, 0, 50, 389, 0, 52, 389, 16, 68, 375, 34, 92, 338, 25, 94, 124, 0, 136, 124, 0, 138, 124, 46,
    150, 118, 39, 156, 107, 22, 162, 101, 33, 170, 115, 27, 172, 115, 0, 186, 115, 0, 2, 109, 0, 6, 109, 0,
    8, 109, 11, 14, 107, 33, 22, 105, 42, 28, 86, 46, 32, 82, 56, 44, 85, 58, 80, 70, 54, 104, 65, 16,
    106, 370, 0, 138, 370, 0, 140, 370, 28, 152, 381, 21, 166, 385, 23, 172, 383, 10, 180, 383, 0, 186, 383, 0,
    2, 573, 0, 8, 573, 0, 10, 573, 6, 14, 573, 26, 16, 573, 42, 26, 572, 247, 140, 572, 248, 156, 572, 54,
    162, 572, 14, 166, 572, 4, 168, 572, 0, 186, 572, 0, 2, 173, 0, 26, 173, 0, 28, 173, 26, 30, 163, 37,
    44, 168, 48, 122, 106, 64, 124, 106, 0, 186, 106, 0, 2, 200, 0, 50, 200, 0, 52, 200, 8, 88, 198, 27,
    128, 147, 55, 136, 143, 60, 138, 143, 0, 186, 143, 0, 2, 640, 0, 4, 640, 0, 6, 640, 6, 8, 640, 13,
    22, 641, 193, 128, 641, 237, 150, 641, 39, 158, 641, 0, 194, 641, 0, 2, 135, 0, 6, 135, 0, 8, 135, 9,
    14, 129, 28, 18, 112, 27, 24, 108, 29, 26, 112, 39, 28, 229, 0, 30, 229, 0, 32, 229, 27, 56, 258, 11,
    58, 59, 0, 76, 59, 0, 78, 59, 15, 134, 65, 36, 150, 92, 38, 164, 93, 8, 194, 94, 0, 2, 146, 0,
    20, 146, 0, 22, 146, 24, 36, 136, 45, 68, 159, 27, 70, 174, 0, 80, 174, 0, 82, 174, 15, 100, 178, 10,
    102, 246, 0, 134, 246, 0, 136, 246, 20, 154, 260, 23, 164, 260, 6, 176, 260, 6, 182, 261, 0, 194, 261, 0,
    2, 97, 0, 40, 97, 0, 42, 97, 34, 122, 135, 29, 142, 130, 28, 144, 130, 0, 194, 130, 0, 2, 205, 0,
    54, 205, 0, 56, 205, 23, 128, 273, 31, 140, 290, 11, 142, 290, 0, 194, 290, 0, 2, 166, 0, 4, 166, 0,
    6, 166, 7, 12, 161, 43, 24, 172, 96, 26, 635, 0, 28, 635, 33, 34, 632, 47, 46, 606, 50, 62, 607, 22,
    70, 607, 14, 72, 609, 18, 74, 436, 0, 84, 436, 0, 86, 436, 7, 96, 434, 18, 102, 434, 32, 132, 417, 48,
    144, 409, 36, 146, 395, 31, 148, 157, 0, 149, 157, 0, 151, 157, 62, 157, 156, 61, 160, 142, 64, 162, 140, 62,
    167, 140, 29, 177, 141, 0, 183, 141, 0, 2, 131, 0, 22, 131, 0, 24, 131, 72, 28, 142, 91, 40, 155, 134,
    134, 140, 134, 144, 123, 108, 152, 123, 89, 154, 131, 140, 156, 131, 0, 183, 131, 0, 2, 710, 0, 10, 710, 0,
    12, 710, 14, 22, 710, 113, 32, 709, 229, 138, 710, 206, 151, 710, 74, 160, 710, 17, 162, 710, 10, 165, 710, 10,
    167, 710, 0, 183, 710, 0, 2, 527, 0, 38, 527, 0, 40, 527, 9, 54, 528, 14, 68, 528, 32, 82, 508, 31,
    108, 506, 15, 110, 504, 19, 112, 504, 0, 183, 504, 0, 2, 155, 0, 4, 155, 0, 8, 155, 16, 14, 156, 50,
    20, 157, 72, 30, 148, 111, 38, 155, 120, 134, 140, 115, 142, 122, 94, 146, 121, 86, 153, 120, 76, 155, 124, 110,
    156, 124, 0, 183, 124, 0, 2, 386, 0, 8, 386, 0, 10, 386, 10, 14, 390, 22, 24, 411, 32, 26, 382, 0,
    32, 382, 0, 34, 382, 26, 46, 380, 42, 68, 380, 41, 80, 366, 41, 104, 365, 40, 120, 351, 44, 146, 353, 33,
    150, 345, 29, 160, 362, 26, 161, 362, 0, 183, 362, 0, 2, 778, 0, 8, 778, 0, 10, 778, 12, 14, 778, 42,
    18, 778, 95, 34, 778, 341, 132, 778, 326, 152, 778, 82, 156, 778, 50, 160, 778, 36, 161, 778, 0, 183, 778, 0,
    2, 76, 0, 36, 76, 0, 38, 76, 21, 42, 76, 12, 44, 80, 11, 50, 79, 10, 52, 75, 10, 56, 79, 9,
    58, 74, 8, 62, 78, 8, 64, 75, 8, 68, 82, 9, 70, 75, 8, 76, 68, 9, 82, 71, 12, 86, 78, 15,
    88, 74, 16, 90, 157, 0, 150, 157, 0, 152, 157, 50, 157, 156, 51, 161, 144, 53, 163, 141, 53, 177, 143, 0,
    183, 143, 0, 2, 368, 0, 24, 368, 0, 26, 368, 46, 408, 605, 35, 410, 587, 41, 412, 95, 0, 634, 95, 0,
    636, 95, 86, 644, 100, 96, 670, 88, 49, 674, 75, 50, 676, 75, 0, 682, 75, 0, 2, 70, 0, 64, 70, 0,
    66, 70, 15, 68, 69, 11, 80, 67, 43, 94, 49, 23, 104, 64, 48, 118, 55, 40, 130, 65, 64, 140, 60, 53,
    224, 78, 67, 234, 72, 74, 242, 82, 65, 254, 61, 68, 256, 62, 60, 258, 82, 74, 260, 51, 0, 272, 51, 0,
    274, 51, 63, 276, 61, 69, 282, 66, 99, 292, 54, 91, 300, 64, 123, 308, 56, 114, 316, 64, 142, 338, 60, 146,
    586, 88, 248, 632, 70, 136, 634, 70, 0, 682, 70, 0, 2, 56, 3, 22, 56, 84, 28, 56, 163, 42, 56, 157,
    64, 60, 248, 186, 113, 248, 194, 122, 248, 290, 256, 224, 322, 390, 163, 362, 562, 150, 370, 613, 115, 394, 816, 83,
    402, 884, 149, 410, 892, 184, 418, 892, 172, 498, 892, 33, 500, 892, 0, 506, 892, 0, 2, 147, 0, 6, 147, 0,
    8, 147, 13, 10, 146, 13, 18, 146, 101, 26, 145, 152, 378, 92, 153, 384, 95, 151, 390, 89, 158, 398, 94, 146,
    406, 82, 174, 412, 93, 141, 414, 93, 140, 420, 79, 193, 422, 77, 207, 424, 78, 197, 432, 93, 126, 438, 78, 191,
    448, 77, 199, 453, 86, 126, 455, 94, 122, 456, 94, 0, 1118, 94, 0, 2, 76, 0, 455, 76, 0, 456, 76, 85,
    460, 76, 73, 480, 75, 248, 514, 76, 50, 516, 90, 37, 518, 93, 34, 522, 89, 43, 524, 76, 64, 536, 75, 212,
    552, 76, 136, 554, 88, 98, 558, 94, 83, 560, 91, 91, 564, 76, 157, 572, 76, 198, 574, 76, 168, 578, 92, 109,
    580, 94, 105, 584, 88, 123, 586, 77, 156, 590, 76, 182, 592, 78, 161, 594, 88, 132, 598, 94, 117, 602, 88, 130,
    604, 80, 151, 606, 79, 157, 612, 94, 125, 616, 93, 127, 620, 86, 140, 626, 95, 129, 632, 92, 130, 640, 96, 130,
    1000, 143, 116, 1102, 143, 5, 1118, 143, 0, 2, 70, 2, 14, 70, 14, 22, 70, 96, 30, 104, 225, 114, 376, 149,
    254, 376, 6, 256, 376, 0, 394, 376, 0, 2, 60, 0, 32, 60, 0, 34, 60, 171, 48, 69, 245, 142, 182, 248,
    330, 183, 121, 380, 183, 11, 388, 183, 0, 394, 183, 0, 2, 261, 0, 4, 261, 0, 6, 261, 5, 24, 264, 185,
    308, 333, 83, 480, 334, 18, 600, 334, 9, 692, 334, 6, 824, 332, 7, 826, 332, 0, 1180, 332, 0, 2, 95, 0,
    22, 95, 0, 24, 95, 4, 30, 95, 4, 188, 95, 94, 478, 117, 247, 954, 198, 228, 1130, 212, 16, 1150, 212, 0,
    1180, 212, 0, 2, 200, 0, 28, 200, 0, 30, 200, 4, 170, 199, 72, 490, 280, 197, 954, 399, 175, 1174, 414, 0,
    1180, 414, 0, 2, 79, 1, 14, 79, 8, 40, 79, 86, 128, 228, 248, 260, 229, 3, 262, 65, 0, 324, 65, 0,
    346, 64, 32, 360, 64, 94, 420, 114, 237, 538, 114, 3, 552, 114, 0, 2, 66, 0, 58, 66, 0, 60, 66, 88,
    70, 66, 110, 130, 114, 234, 246, 114, 10, 248, 124, 0, 292, 124, 1, 322, 124, 44, 414, 448, 190, 536, 458, 4,
    552, 458, 0, 2, 297, 0, 4, 297, 0, 8, 297, 14, 10, 297, 29, 20, 297, 151, 112, 295, 165, 426, 69, 248,
    450, 57, 97, 480, 57, 0, 512, 57, 0, 2, 554, 0, 6, 554, 0, 8, 554, 10, 22, 559, 134, 162, 554, 153,
    484, 304, 184, 498, 298, 31, 504, 298, 4, 506, 298, 0, 512, 298, 0, 2, 594, 0, 4, 594, 0, 6, 594, 8,
    22, 594, 246, 230, 595, 248, 348, 594, 2, 356, 594, 0, 2, 324, 0, 6, 324, 0, 8, 324, 14, 16, 323, 125,
    26, 336, 233, 298, 875, 71, 306, 885, 80, 314, 872, 70, 526, 353, 220, 540, 323, 246, 774, 321, 4, 788, 321, 0,
    2, 321, 0, 6, 321, 0, 8, 321, 9, 10, 321, 27, 24, 322, 243, 244, 322, 241, 254, 356, 132, 256, 426, 0,
    262, 426, 0, 264, 426, 197, 278, 517, 202, 280, 546, 0, 282, 546, 214, 292, 572, 242, 318, 572, 241, 326, 561, 138,
    328, 403, 0, 330, 403, 136, 342, 389, 232, 564, 389, 231, 784, 390, 9, 798, 390, 1, 806, 390, 0, 2, 59, 0,
    408, 59, 0, 414, 59, 3, 432, 59, 20, 446, 68, 41, 522, 137, 41, 784, 137, 0, 806, 137, 0, 2, 708, 0,
    4, 708, 0, 6, 708, 10, 24, 710, 171, 220, 710, 166, 230, 702, 63, 234, 701, 12, 236, 701, 0, 620, 701, 0,
    2, 140, 0, 6, 140, 0, 8, 140, 10, 24, 136, 65, 456, 137, 65, 604, 137, 0, 620, 137, 0, 2, 323, 0,
    220, 323, 0, 222, 323, 5, 228, 324, 50, 240, 320, 240, 462, 320, 247, 614, 320, 0, 620, 320, 0, 2, 417, 0,
    4, 417, 0, 6, 417, 17, 14, 417, 131, 28, 423, 242, 46, 419, 134, 62, 412, 248, 72, 420, 200, 546, 412, 236,
    556, 423, 135, 572, 412, 120, 584, 424, 246, 738, 418, 13, 748, 418, 2, 758, 419, 0, 2, 369, 0, 80, 369, 0,
    82, 369, 122, 294, 81, 173, 304, 74, 169, 312, 81, 173, 520, 359, 119, 528, 369, 154, 530, 369, 0, 758, 369, 0,
    2, 224, 0, 6, 224, 0, 10, 224, 22, 24, 220, 205, 156, 73, 248, 174, 59, 110, 206, 59, 3, 208, 59, 0,
    332, 59, 0, 2, 800, 0, 6, 800, 0, 8, 800, 4, 10, 799, 17, 26, 801, 132, 220, 801, 128, 324, 802, 11,
    326, 802, 0, 332, 802, 0, 2, 799, 0, 4, 799, 0, 6, 799, 11, 18, 800, 184, 54, 787, 104, 56, 797, 0,
    96, 797, 0, 98, 797, 92, 122, 801, 212, 136, 796, 76, 138, 791, 0, 164, 791, 0, 166, 791, 102, 178, 800, 207,
    188, 790, 105, 190, 790, 0, 342, 790, 0, 2, 415, 0, 46, 415, 0, 48, 415, 8, 52, 415, 36, 62, 412, 232,
    92, 411, 247, 102, 411, 82, 104, 411, 0, 122, 411, 0, 124, 411, 4, 126, 411, 13, 128, 411, 29, 140, 411, 248,
    160, 413, 206, 170, 416, 24, 174, 416, 0, 180, 416, 0, 182, 416, 9, 186, 416, 40, 194, 412, 205, 224, 411, 248,
    324, 412, 16, 334, 412, 3, 342, 412, 0, 2, 663, 0, 4, 663, 0, 6, 663, 14, 12, 664, 93, 20, 665, 232,
    82, 664, 248, 88, 664, 204, 98, 663, 35, 102, 663, 6, 104, 886, 0, 122, 886, 0, 124, 886, 9, 126, 890, 37,
    132, 889, 108, 146, 894, 225, 212, 893, 219, 270, 894, 111, 318, 893, 16, 320, 893, 0, 326, 893, 0, 2, 476, 0,
    4, 476, 0, 6, 476, 7, 8, 476, 19, 22, 475, 224, 132, 400, 228, 250, 342, 241, 402, 303, 248, 548, 298, 193,
    630, 298, 10, 634, 298, 4, 644, 298, 0, 2, 825, 0, 70, 825, 0, 72, 825, 36, 82, 826, 112, 116, 825, 179,
    248, 685, 232, 460, 596, 196, 538, 595, 196, 568, 596, 142, 630, 596, 8, 634, 596, 7, 636, 596, 0, 644, 596, 0,
    2, 163, 0, 17, 163, 0, 19, 163, 10, 30, 163, 21, 102, 163, 21, 132, 163, 76, 188, 136, 174, 260, 134, 173,
    319, 108, 171, 325, 96, 175, 329, 103, 159, 334, 101, 153, 336, 77, 0, 353, 77, 0, 355, 77, 170, 366, 76, 248,
    376, 74, 92, 378, 93, 0, 379, 93, 0, 380, 93, 141, 382, 81, 81, 386, 73, 104, 402, 69, 203, 410, 74, 191,
    422, 66, 186, 452, 68, 174, 466, 70, 68, 486, 70, 37, 498, 70, 55, 510, 70, 38, 522, 70, 54, 532, 70, 37,
    544, 70, 54, 556, 70, 37, 566, 70, 54, 578, 70, 38, 590, 70, 55, 602, 70, 38, 614, 70, 54, 624, 70, 37,
    636, 70, 54, 648, 70, 32, 660, 70, 32, 676, 70, 5, 678, 72, 10, 680, 72, 0, 686, 72, 0, 2, 617, 0,
    19, 617, 0, 20, 617, 64, 22, 618, 31, 36, 618, 102, 108, 609, 85, 110, 618, 128, 112, 205, 0, 161, 205, 0,
    162, 205, 104, 182, 206, 167, 210, 206, 144, 258, 205, 25, 260, 205, 21, 262, 205, 29, 264, 64, 0, 330, 64, 0,
    332, 64, 140, 338, 82, 210, 342, 82, 204, 346, 95, 134, 350, 97, 120, 353, 93, 130, 355, 93, 0, 686, 93, 0,
    2, 69, 0, 172, 69, 0, 174, 69, 20, 188, 69, 39, 222, 68, 146, 258, 68, 160, 296, 68, 178, 298, 68, 0,
    686, 68, 0, 2, 220, 0, 6, 220, 0, 8, 220, 25, 10, 221, 61, 14, 220, 142, 22, 217, 248, 202, 217, 245,
    238, 217, 143, 284, 216, 72, 296, 216, 43, 308, 216, 19, 314, 216, 8, 318, 217, 6, 320, 217, 0, 328, 217, 0,
    2, 304, 0, 14, 304, 0, 16, 304, 140, 28, 307, 218, 196, 306, 217, 238, 306, 148, 284, 306, 51, 296, 306, 39,
    302, 306, 24, 310, 306, 11, 318, 306, 2, 328, 307, 0, 2, 157, 0, 8, 157, 0, 10, 157, 30, 14, 159, 70,
    18, 161, 163, 26, 161, 248, 206, 160, 244, 232, 160, 161, 272, 160, 86, 288, 160, 66, 310, 160, 17, 322, 160, 0,
    328, 160, 0, 2, 281, 0, 14, 281, 0, 16, 281, 100, 18, 277, 138, 26, 277, 207, 206, 277, 164, 232, 277, 158,
    306, 277, 20, 320, 277, 0, 328, 277, 0, 2, 163, 0, 8, 163, 0, 10, 163, 26, 12, 158, 33, 26, 161, 248,
    208, 160, 248, 318, 160, 5, 324, 160, 0, 332, 160, 0, 2, 308, 0, 12, 308, 0, 18, 306, 112, 28, 307, 203,
    222, 306, 173, 300, 306, 34, 318, 306, 4, 332, 306, 0, 2, 159, 0, 8, 159, 0, 12, 159, 34, 14, 159, 63,
    16, 159, 116, 26, 161, 248, 204, 160, 244, 310, 160, 16, 322, 160, 0, 328, 160, 0, 2, 340, 0, 12, 340, 0,
    14, 340, 82, 26, 337, 189, 222, 338, 160, 308, 338, 15, 320, 338, 0, 328, 338, 0, 2, 181, 0, 8, 181, 0,
    16, 178, 125, 26, 176, 248, 200, 176, 248, 318, 176, 4, 330, 176, 0, 2, 274, 0, 16, 274, 0, 18, 274, 147,
    230, 277, 163, 292, 277, 48, 318, 277, 0, 330, 277, 0, 2, 174, 0, 8, 174, 0, 10, 174, 18, 14, 174, 62,
    16, 174, 115, 26, 176, 248, 206, 176, 244, 318, 176, 4, 322, 176, 0, 328, 176, 0, 2, 309, 0, 14, 309, 0,
    16, 309, 117, 28, 307, 203, 230, 306, 158, 256, 306, 94, 308, 306, 19, 320, 306, 0, 328, 306, 0, 2, 178, 0,
    8, 178, 0, 10, 178, 36, 12, 173, 40, 14, 173, 63, 18, 173, 151, 26, 176, 248, 208, 176, 242, 264, 176, 97,
    290, 176, 63, 310, 176, 18, 322, 176, 0, 328, 176, 0, 2, 337, 0, 12, 337, 0, 14, 337, 34, 16, 335, 87,
    26, 337, 195, 210, 338, 187, 308, 338, 17, 322, 338, 0, 328, 338, 0, 2, 204, 0, 6, 204, 0, 8, 204, 46,
    10, 197, 36, 26, 196, 248, 216, 195, 221, 314, 195, 6, 320, 195, 0, 326, 195, 0, 2, 278, 0, 14, 278, 0,
    18, 276, 152, 26, 277, 217, 210, 277, 206, 316, 277, 0, 326, 277, 0, 2, 200, 0, 6, 200, 0, 10, 195, 25,
    26, 196, 248, 206, 195, 244, 318, 195, 4, 328, 195, 0, 2, 310, 0, 14, 310, 0, 16, 310, 68, 26, 307, 209,
    214, 306, 190, 318, 306, 3, 320, 306, 0, 328, 306, 0, 2, 197, 0, 8, 197, 0, 18, 194, 161, 210, 195, 239,
    320, 195, 4, 330, 195, 0, 2, 339, 0, 12, 339, 0, 14, 339, 59, 26, 338, 200, 214, 338, 187, 322, 338, 0,
    330, 338, 0, 2, 213, 0, 8, 213, 0, 10, 213, 41, 26, 217, 248, 202, 216, 245, 314, 216, 7, 316, 216, 9,
    318, 216, 0, 324, 216, 0, 2, 276, 0, 20, 276, 0, 22, 276, 174, 28, 277, 226, 202, 277, 223, 300, 277, 28,
    304, 277, 24, 306, 278, 30, 308, 278, 0, 324, 278, 0, 2, 217, 0, 8, 217, 0, 10, 217, 68, 26, 216, 248,
    210, 217, 232, 312, 216, 10, 320, 216, 0, 328, 216, 0, 2, 335, 0, 14, 335, 0, 16, 335, 122, 198, 337, 206,
    318, 337, 2, 328, 337, 0, 2, 395, 0, 10, 395, 0, 12, 395, 248, 14, 407, 158, 20, 413, 248, 514, 406, 248,
    536, 421, 248, 672, 406, 248, 1718, 407, 34, 1944, 407, 29, 1970, 407, 20, 2184, 407, 20, 2208, 407, 11, 2376, 407, 10,
    2390, 407, 3, 2392, 409, 6, 2394, 409, 0, 2400, 409, 0,
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
    p[i].ms = (float)((times[i] - (i > 0 ? times[i - 1] : 0)) / 2.0);
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
      p[i].freq[fx_tones[k].osc] = (float)(f * FREQ_STEP_HZ);
      p[i].vol[fx_tones[k].osc] = (float)(v / 8);
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
  short ms;        /* hundredths of a millisecond */
  short freq[3];   /* FREQ_STEP_HZ steps */
  short vol[3];    /* eighths */
} fxv_point;

static const fxv_point fxv_points[] = {
    {100, {60, 377, 324}, {44, 79, 25}}, /* A2 */
    {12561, {59, 353, 583}, {45, 74, 75}},
    {2295, {54, 350, 567}, {53, 78, 71}},
    {2434, {52, 347, 551}, {58, 97, 87}},
    {2309, {155, 344, 535}, {39, 125, 116}},
    {1152, {50, 342, 527}, {62, 130, 113}},
    {1148, {52, 341, 517}, {56, 127, 122}},
    {1150, {51, 339, 516}, {56, 127, 128}},
    {1152, {49, 335, 508}, {57, 125, 123}},
    {1153, {48, 333, 504}, {58, 126, 110}},
    {1145, {48, 329, 500}, {56, 126, 101}},
    {1145, {43, 325, 479}, {61, 127, 109}},
    {1166, {45, 327, 460}, {66, 135, 110}},
    {1150, {58, 324, 505}, {52, 140, 64}},
    {1146, {48, 320, 442}, {75, 140, 124}},
    {1143, {47, 320, 437}, {81, 140, 127}},
    {1142, {45, 318, 429}, {86, 141, 120}},
    {1178, {47, 318, 411}, {76, 145, 130}},
    {1146, {48, 314, 407}, {69, 142, 135}},
    {1144, {54, 308, 404}, {50, 137, 129}},
    {1144, {52, 306, 395}, {53, 135, 134}},
    {1145, {51, 303, 382}, {54, 138, 130}},
    {1146, {57, 304, 368}, {46, 143, 133}},
    {1145, {55, 298, 364}, {47, 140, 132}},
    {1144, {57, 293, 356}, {46, 138, 128}},
    {1147, {155, 289, 345}, {41, 132, 123}},
    {1145, {155, 292, 335}, {39, 140, 137}},
    {1141, {59, 287, 324}, {42, 145, 145}},
    {1150, {60, 300, 456}, {49, 137, 80}},
    {1145, {64, 280, 451}, {42, 134, 73}},
    {1144, {64, 288, 583}, {42, 186, 48}},
    {1142, {62, 284, 567}, {44, 214, 51}},
    {1143, {62, 278, 588}, {46, 237, 61}},
    {1144, {63, 272, 589}, {42, 248, 43}},
    {1146, {61, 268, 588}, {44, 248, 56}},
    {1138, {155, 261, 324}, {37, 248, 25}},
    {1120, {57, 255, 568}, {55, 248, 47}},
    {1150, {55, 249, 324}, {55, 248, 34}},
    {1139, {44, 243, 324}, {90, 248, 13}},
    {1146, {42, 236, 324}, {94, 248, 27}},
    {1129, {155, 232, 324}, {26, 229, 20}},
    {1159, {208, 246, 324}, {144, 135, 22}},
    {1197, {201, 240, 324}, {146, 136, 19}},
    {1087, {194, 237, 324}, {152, 139, 22}},
    {1209, {185, 235, 324}, {151, 135, 13}},
    {1145, {178, 231, 324}, {158, 140, 16}},
    {1074, {169, 228, 324}, {159, 142, 25}},
    {1146, {163, 224, 324}, {163, 147, 21}},
    {1145, {155, 221, 324}, {165, 150, 22}},
    {1417, {153, 209, 578}, {132, 122, 37}},
    {1075, {144, 208, 593}, {101, 95, 37}},
    {1158, {137, 209, 569}, {113, 107, 38}},
    {1154, {125, 208, 558}, {114, 106, 50}},
    {1152, {122, 211, 558}, {114, 112, 70}},
    {1152, {118, 210, 555}, {111, 111, 61}},
    {1142, {90, 200, 324}, {121, 105, 17}},
    {1082, {93, 193, 324}, {126, 118, 21}},
    {1152, {88, 192, 324}, {116, 120, 25}},
    {1157, {83, 190, 588}, {103, 118, 37}},
    {1154, {71, 190, 324}, {97, 113, 32}},
    {1150, {49, 182, 324}, {159, 114, 25}},
    {1075, {46, 183, 324}, {159, 111, 21}},
    {1159, {42, 179, 324}, {141, 102, 10}},
    {1150, {43, 175, 324}, {103, 103, 16}},
    {4573, {47, 162, 324}, {74, 128, 21}},
    {1141, {50, 163, 324}, {55, 112, 23}},
    {1217, {155, 261, 324}, {25, 59, 7}},
    {1150, {155, 261, 324}, {25, 59, 7}},
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
    f = voiced_frame(FM(p[i].freq[0] * FREQ_STEP_HZ, p[i].freq[1] * FREQ_STEP_HZ, p[i].freq[2] * FREQ_STEP_HZ),
                     p[i].vol[0] / 8.0, p[i].vol[1] / 8.0, p[i].vol[2] / 8.0);
    f.pitch = voice_hz(SJ_DEFAULT_PITCH);
    hold(s, p[i].ms / 100.0, p[i].ms / 100.0, f);
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
  short ms;        /* hundredths of a millisecond */
  short ticks;     /* the voicing period in VOICE_TICK_HZ ticks */
  short freq[3];   /* FREQ_STEP_HZ steps */
  short vol[3];    /* eighths */
} c1_point;

static const c1_point c1_points[] = {
    {100, 24, {200, 329, 455}, {81, 10, 0}},
    {190, 24, {174, 328, 476}, {53, 5, 0}},
    {190, 24, {193, 309, 478}, {93, 31, 0}},
    {380, 24, {254, 267, 471}, {208, 99, 0}},
    {380, 23, {204, 366, 502}, {138, 90, 0}},
    {190, 23, {188, 371, 513}, {131, 117, 0}},
    {380, 23, {170, 292, 524}, {175, 231, 0}},
    {380, 23, {130, 359, 497}, {181, 131, 0}},
    {190, 23, {127, 379, 487}, {188, 99, 2}},
    {380, 23, {231, 309, 499}, {239, 191, 14}},
    {190, 23, {245, 328, 506}, {206, 188, 12}},
    {380, 23, {228, 431, 518}, {70, 124, 0}},
    {380, 20, {381, 473, 488}, {206, 206, 43}},
    {190, 20, {408, 471, 480}, {230, 225, 61}},
    {380, 19, {301, 390, 483}, {136, 189, 85}},
    {380, 19, {262, 348, 441}, {161, 168, 61}},
    {190, 18, {245, 336, 418}, {181, 158, 47}},
    {380, 18, {195, 405, 390}, {191, 124, 35}},
    {190, 17, {170, 380, 395}, {188, 121, 38}},
    {380, 17, {122, 211, 440}, {164, 143, 62}},
    {190, 16, {99, 246, 463}, {141, 153, 96}},
    {190, 16, {76, 297, 487}, {116, 164, 133}},
    {190, 16, {60, 323, 506}, {89, 165, 160}},
    {190, 15, {64, 273, 509}, {53, 136, 160}},
    {190, 15, {67, 223, 512}, {17, 107, 160}},
    {190, 15, {84, 205, 515}, {12, 88, 164}},
    {190, 15, {109, 207, 518}, {25, 76, 170}},
    {190, 14, {135, 209, 521}, {38, 64, 176}},
    {380, 14, {30, 286, 527}, {52, 119, 194}},
    {190, 14, {13, 322, 523}, {48, 147, 192}},
    {190, 14, {55, 354, 508}, {27, 175, 171}},
    {190, 15, {97, 385, 493}, {5, 203, 149}},
    {190, 15, {86, 372, 494}, {29, 181, 146}},
    {190, 15, {57, 345, 499}, {67, 142, 149}},
    {190, 15, {29, 330, 498}, {95, 113, 148}},
    {380, 15, {-16, 481, 399}, {19, 187, 84}},
    {190, 15, {-29, 498, 380}, {3, 200, 70}},
    {570, 15, {-35, 368, 417}, {19, 162, 84}},
    {190, 16, {-37, 324, 429}, {24, 149, 89}},
    {190, 16, {-36, 294, 440}, {28, 139, 93}},
    {1330, 16, {24, 344, 492}, {19, 114, 125}},
    {190, 16, {30, 352, 498}, {17, 115, 123}},
    {190, 17, {30, 362, 501}, {16, 122, 111}},
    {1520, 17, {32, 437, 524}, {2, 178, 14}},
    {190, 18, {32, 446, 527}, {0, 185, 2}},
    {190, 18, {46, 434, 522}, {2, 176, 11}},
    {1710, 19, {193, 303, 462}, {23, 71, 120}},
    {190, 19, {211, 302, 460}, {26, 64, 125}},
    {380, 19, {252, 343, 470}, {35, 64, 106}},
    {190, 20, {273, 364, 475}, {39, 64, 97}},
    {1330, 20, {417, 508, 511}, {71, 64, 33}},
    {760, 22, {341, 478, 524}, {63, 63, 38}},
    {1140, 23, {220, 427, 543}, {49, 60, 47}},
    {190, 23, {209, 420, 545}, {48, 59, 48}},
    {1900, 26, {239, 372, 551}, {36, 24, 56}},
    {2280, 29, {195, 368, 601}, {15, 16, 31}},
    {1710, 33, {171, 360, 617}, {6, 0, 24}},
    {2090, 38, {121, 392, 639}, {0, 0, 17}},
    {190, 40, {121, 392, 644}, {0, 1, 15}},
    {1140, 40, {121, 392, 676}, {0, 6, 1}},
    {760, 40, {121, 392, 667}, {0, 2, 0}},
};

static void c1(sound *s) {
  const int n = (int)(sizeof(c1_points) / sizeof(c1_points[0]));
  frame f = voiced_frame(FM(0, 0, 0), 0, 0, 0);
  for (int i = 0; i < n; i++) {
    const c1_point *p = &c1_points[i];
    f = voiced_frame(FM(p->freq[0] * FREQ_STEP_HZ, p->freq[1] * FREQ_STEP_HZ, p->freq[2] * FREQ_STEP_HZ),
                     p->vol[0] / 8.0, p->vol[1] / 8.0, p->vol[2] / 8.0);
    f.pitch = VOICE_TICK_HZ / p->ticks;
    f.free_osc = 4;
    hold(s, p->ms / 100.0, i == 0 ? 1 : p->ms / 100.0, f);
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
    {142, {0.32, 1.15, 0.93, -0.4, 6.49, -0.33, -4.59, 6.95, -2.46, 0.37}, 0.00, 0.00}, /* NO */
    {143, {1.5, 1.7, 0.15, -1.93, -1.67, 3.09, 0.07, -1.45, -0.89, 0.18}, 0.00, 0.00}, /* NGE */
    {144, {1.45, 1.99, -0.97, -2.64, 3.27, -0.0, -1.75, 0.49, -1.86, -1.18}, 0.00, 0.00}, /* NGO */
    {145, {-0.59, -0.05, -0.28, -0.76, -0.61, 0.04, -0.92, -3.34, -0.26, 1.11}, 0.00, 0.00}, /* LE */
    {146, {1.98, 1.99, 1.89, 0.34, -0.47, 3.56, 1.18, -6.11, 5.89, -3.34}, 0.00, 0.00}, /* LO */
    {147, {0.14, 0.38, 0.07, -0.12, 0.13, -0.45, -1.43, -1.13, 0.17, 1.21}, 0.00, 0.00}, /* WW */
    {148, {0.94, 1.07, 0.61, -0.13, 0.39, 1.28, 0.59, 0.32, -1.79, -0.49}, 0.00, 0.00}, /* RR */
    {149, {-0.45, 0.73, 1.15, 0.42, -0.13, -0.32, -0.37, 2.8, -0.82, -0.32}, 0.00, 0.00}, /* IYRR */
    {150, {0.34, 1.33, 6.42, 1.83, -11.28, 2.66, 5.12, 0.04, -1.89, -0.47}, 0.00, 0.00}, /* EYRR */
    {152, {1.23, 1.52, 1.51, 0.73, -0.09, 0.22, 1.24, 2.67, -2.02, -1.5}, 0.00, 0.00}, /* AWRR */
    {153, {1.42, 1.71, 1.45, 0.41, 0.01, 0.32, -0.48, 7.66, -5.81, -0.96}, 0.00, 0.00}, /* OWRR */
    {155, {-1.09, -0.11, 2.06, 2.7, -1.59, -3.47, 2.26, 2.17, -0.05, -0.28}, 0.00, 0.00}, /* OHIY */
    {157, {0.5, 0.01, 1.2, 2.19, -1.63, -2.45, 2.01, 0.61, -0.21, -0.57}, 0.00, 0.00}, /* OHIH */
    {160, {-0.13, 0.39, 1.03, 1.7, 0.03, -1.4, -0.15, 2.37, -0.35, -0.58}, 0.00, 0.00}, /* IYUW */
    {162, {0.73, 0.35, 0.3, 1.06, 1.27, 0.19, 0.06, 1.95, -1.32, -0.88}, 0.00, 0.00}, /* IHWW */
    {163, {1.06, 1.35, 1.5, 0.64, -0.0, 0.8, 0.65, 0.79, -2.09, -0.57}, 0.00, 0.00}, /* AYWW */
    {164, {2.24, 2.59, 2.55, 0.43, 0.44, 2.74, -0.61, 2.59, -4.1, -1.81}, 0.00, 0.00}, /* OWWW */
    {165, {-6.4, -0.3, 6.2, -5.1, 3.7, -1.6, -1.5, -0.1, 1.3, 3.8}, 0.00, 1.19}, /* JH */
    {166, {5.57, 0.19, -1.52, 2.8, -2.28, -2.6, 1.18, 0.21, 2.03, -1.61}, 0.00, 0.23}, /* VV */
    {167, {1.9, -0.3, -0.6, 2.1, 0.4, -3.7, 0.4, -1.8, -1.1, 2.8}, 0.00, -0.45}, /* ZZ */
    {168, {0.45, 0.35, -1.52, -1.06, 3.8, -2.04, -2.27, 4.69, 0.18, -1.18}, 0.00, -0.14}, /* ZH */
    {169, {2.7, -0.9, -0.9, -1.2, 8.4, -5.4, -1.1, 1.4, -2.3, -0.5}, 0.00, -0.45}, /* DH */
    {170, {-8.2, 3.3, 3.2, 2.2, -4.5, -3.2, 5.5, 0.1, 2.1, -0.5}, 0.00, 0.56}, /* BE */
    {171, {2.0, 2.4, 0.3, -0.8, -1.7, 3.6, -0.3, -0.1, -2.2, -3.3}, 0.00, -0.97}, /* BO */
    {173, {-0.5, 4.5, -3.5, 3.5, 3.9, -11.5, 15.0, -7.0, 3.1, -7.8}, 0.00, -2.81}, /* OB */
    {174, {-5.91, -0.32, 8.46, -0.96, -4.0, 0.66, -0.66, -3.21, 0.68, 4.1}, 0.00, 1.38}, /* DE */
    {175, {1.4, 0.6, -1.0, -1.1, 4.0, 0.8, -0.9, 1.0, -1.5, -3.4}, 0.00, -0.86}, /* DO */
    {176, {-4.94, 0.68, 1.7, -4.78, 14.76, -3.74, -0.04, 3.1, -4.58, -1.06}, 0.00, 1.11}, /* ED */
    {177, {-1.5, 1.35, -2.92, -1.58, 10.94, -1.84, -3.52, 3.33, -3.72, 0.01}, 0.00, 0.78}, /* OD */
    {178, {-1.18, 1.16, -0.99, -4.38, 7.63, 8.33, -15, 6.06, -3.53, 3.27}, 0.00, 0.67}, /* GE */
    {179, {-1.2, 1.0, -1.1, -2.1, 3.5, 14.4, -15.0, 7.0, -10.8, 4.4}, 0.00, 0.12}, /* GO */
    {180, {-0.42, 0.97, -0.53, -1.12, 10.61, -4.92, -1.04, 3.87, -6.69, -1.2}, 0.00, -1.34}, /* EG */
    {181, {1.2, 1.0, -0.5, 1.9, 4.9, -3.7, 0.3, 1.3, -7.0, 0.5}, 0.00, -1.73}, /* OG */
    {184, {1.26, 0.91, -0.86, -1.22, -0.77, -0.39, -1.21, -0.71, -1.05, 1.16}, 0.00, 0.00}, /* HO */
    {185, {-2.25, -0.18, -2.37, -1.16, 0.61, -2.23, -5.49, -1.02, 1.7, 2.2}, 0.00, 0.00}, /* WH */
    {187, {7.36, 2.8, -7.71, -6.11, 2.58, 0.46, -1.0, -3.48, -0.41, 2.45}, 0.02, 0.00}, /* SE */
    {188, {7.09, 5.11, -9.01, -8.68, 6.04, 1.38, -5.15, -1.22, -1.19, -2.56}, 0.00, 0.00}, /* SO */
    {189, {5.95, -0.24, -7.18, -2.18, 3.08, -2.64, -1.37, 1.32, 1.53, -2.11}, 0.15, 0.00}, /* SH */
    {190, {-3.39, 4.46, 1.38, -1.11, -0.29, -0.47, 3.86, -4.25, 2.6, 1.63}, 0.00, 0.00}, /* TH */
    {191, {3.1, 1.19, -1.25, -1.05, -0.89, -1.47, -2.59, -1.85, -1.0, 0.8}, 0.00, 0.00}, /* TT */
    {192, {-2.71, 0.29, -3.38, -0.61, -1.42, -3.45, -6.33, -0.11, 4.07, 10.45}, 0.60, -6.18}, /* TU */
    {193, {0.63, -0.01, 0.11, 0.32, 0.19, 0.04, -1.1, -2.48, -0.11, -0.53}, 0.00, 0.00}, /* TS */
    {194, {-0.16, -1.78, 0.21, 2.53, -1.67, 0.27, 2.53, -0.37, 3.75, -2.81}, 0.00, 0.00}, /* KE */
    {195, {2.1, 2.8, -6.9, -3.0, 7.0, -2.7, 1.1, 2.7, 3.6, -6.7}, 0.30, -2.29}, /* KO */
    {196, {-0.08, 1.5, 2.88, -4.94, -2.84, -0.13, 6.49, -2.81, 6.22, -5.64}, 0.30, -1.01}, /* EK */
    {197, {3.04, 0.01, -0.06, 0.63, -3.1, 0.44, 1.48, 1.91, 0.98, -2.91}, 0.00, 0.00}, /* OK */
    {198, {7.82, 2.52, -2.54, -1.67, 1.63, -1.47, 0.05, -2.11, -6.27, -0.34}, 0.00, 0.00}, /* PE */
    {199, {6.42, 0.76, -2.16, -2.84, -0.61, -0.25, -3.13, -2.09, -1.04, 0.89}, 0.00, 0.00}, /* PO */
    {200, {3.74, 1.61, -0.69, -1.09, -0.23, -1.19, -0.44, -2.49, -0.93, -0.09}, 0.00, 0.00}, /* R0 */
    {201, {0.07, 0.7, 2.55, 1.9, -1.87, -2.68, -1.61, -0.32, -1.36, 2.31}, 0.00, 0.00}, /* R1 */
    {202, {4.54, 10.34, -5.94, -5.85, 1.35, -3.2, -0.92, -2.42, 3.72, 0.35}, 0.00, 0.00}, /* R2 */
    {203, {9.51, 0.49, -6.2, 0.24, -1.53, 0.9, -6.75, 0.52, -4.47, 0.06}, 0.00, 0.00}, /* R3 */
    {204, {5.84, -3.63, -5.75, -2.17, -4.63, -1.18, -3.04, -7.14, -0.46, 0.99}, 0.00, 0.00}, /* R4 */
    {205, {15, 1.53, -9.63, 2.26, -0.29, -1.42, 2.77, 1.86, -9.79, -1.27}, 0.00, 0.00}, /* R5 */
    {206, {5.65, -2.19, -6.24, -3.53, -4.5, -1.78, 2.62, -15.0, 0.88, 6.59}, 0.00, 0.00}, /* R6 */
    {207, {10.56, -2.69, -6.15, -1.93, -0.71, -2.44, -3.35, 3.17, -7.25, -1.67}, 0.00, 0.00}, /* R7 */
    {208, {3.7, -2.13, -2.51, 2.0, -0.93, -3.17, 0.58, -1.3, -1.96, 0.51}, 0.00, 0.00}, /* R8 */
    {209, {3.52, -0.19, -1.61, 0.37, -0.5, 0.2, 1.53, -3.03, -0.81, -1.44}, 0.00, 0.00}, /* R9 */
    {210, {15, -4.53, -11.12, -2.08, -2.03, -4.89, -1.12, -12.81, 14.02, -8.65}, 0.00, 0.00}, /* A0 */
    {211, {1.09, 0.93, 0.82, 0.97, 1.04, 0.45, 0.42, 0.81, -0.31, -1.21}, 0.00, 0.00}, /* A1 */
    {212, {3.04, 1.32, -0.5, -0.7, -0.38, -0.42, -0.82, -1.3, 1.62, -1.14}, 0.00, 0.00}, /* A2 */
    {213, {1.32, 1.67, 1.04, 0.27, 1.07, 1.18, 1.89, -0.25, -1.63, -0.92}, 0.00, 0.00}, /* A3 */
    {214, {8.15, -1.43, -3.03, -0.77, -0.58, -1.6, -0.15, -1.96, -0.2, -0.92}, 0.00, 0.00}, /* A4 */
    {215, {1.63, 0.78, 0.09, -0.06, -0.69, -1.06, -1.94, -0.89, 0.42, -0.12}, 0.00, 0.00}, /* A5 */
    {216, {6.91, 3.13, -0.62, -0.23, 0.98, 0.14, 0.56, 3.58, -8.02, -0.74}, 0.00, 0.00}, /* A6 */
    {218, {10.76, 0.42, -5.11, -2.3, -1.69, -2.8, -1.19, -3.63, -4.09, -0.41}, 0.00, 0.00}, /* A8 */
    {219, {15.0, 1.3, -4.51, -1.64, -0.22, -0.69, -0.38, 0.58, -4.75, -5.65}, 0.00, 0.00}, /* A9 */
    {220, {0.12, 1.11, 0.71, -1.17, -1.08, -0.13, -1.29, -1.72, -0.54, 0.89}, 0.00, 0.00}, /* B0 */
    {222, {15, -0.76, -15, -5.14, -3.37, -2.47, -6.95, -5.03, -10.9, -1.06}, 0.00, 0.00}, /* B2 */
    {223, {-2.63, -3.21, -4.43, -2.66, 1.86, 1.81, -4.97, 15, 1.56, -2.7}, 0.00, 0.00}, /* B3 */
    {224, {0.15, 2.13, -2.31, -2.1, -0.44, -1.51, -4.51, 4.03, -3.12, 3.6}, 0.00, 0.00}, /* B4 */
    {225, {15, -4.5, -10.73, -2.26, -4.03, -3.14, 0.01, -12.31, -5.8, -6.64}, 0.00, 0.00}, /* B5 */
    {226, {0.01, 0.14, -0.83, -1.55, -2.6, -2.11, 1.74, 2.28, -4.32, 2.42}, 0.00, 0.00}, /* B6 */
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
    {238, {0.97, 1.72, 0.91, 0.91, -4.74, -3.57, -1.35, -1.4, 6, 0.9}, 0.00, 0.00}, /* C8 */
    {239, {2.9, 2.37, 4.96, 1.7, -7.23, -3.71, 0.84, -2.6, 5.05, -1.16}, 0.00, 0.00}, /* C9 */
    {241, {1.8, 0.31, 2.76, -0.57, -8.51, -2.89, -7.03, -7.5, 7.71, 1.07}, 0.00, 0.00}, /* D1 */
    {242, {-0.81, 2.07, 6.08, 0.4, -6.12, 9.4, -2.16, -3.26, -0.95, -0.09}, 0.00, 0.00}, /* D2 */
    {243, {1.71, 1.19, 3.95, 2.44, -10.69, 4.63, -3.61, -1.53, 0.58, 0.04}, 0.00, 0.00}, /* D3 */
    {244, {4.29, 3.68, 1.89, 3.23, -6.66, -3.87, -1.81, -1.64, -0.36, -0.04}, 0.00, 0.00}, /* D4 */
    {245, {0.77, 3.91, 11.47, 10.72, -13.43, -0.64, -2.81, -4.98, -0.69, -0.07}, 0.00, 0.00}, /* D5 */
    {246, {5.59, 4.35, 6.17, 7.13, -11.45, -11.02, -5.41, -1.05, 0.59, 0.04}, 0.00, 0.00}, /* D6 */
    {247, {1.84, -0.23, -0.26, 6.42, 1.47, -14.3, 1.17, 3.93, -1.71, -0.14}, 0.00, 0.00}, /* D7 */
    {248, {-0.19, -0.33, -0.98, -1.57, -0.16, 3.08, 0.88, -1.06, 0.19, 0.02}, 0.00, 0.00}, /* D8 */
    {249, {1.15, 0.82, 0.26, 3.3, -1.35, -5.55, -4.26, -1.14, 2.24, 0.19}, 0.00, 0.00}, /* D9 */
    {250, {1.15, 1.22, -2.07, 0.34, 3.15, -7.69, -1.9, 4.38, -0.5, -0.06}, 0.00, 0.00}, /* D10 */
    {251, {-2.18, 1.99, 2.21, 1.79, 1.38, -5.71, -0.55, -2.36, 2.81, 0.25}, 0.00, 0.00}, /* D11 */
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
    s->copy1_length = copy1_length[code - 128];
    s->copy_length = copy_length[code - 128];
    for (int k = 0; k < (int)(sizeof(voicing_pitch) / sizeof(voicing_pitch[0])); k++)
      if (voicing_pitch[k].code == code)
        s->pitch_scale = voicing_pitch[k].scale;
    s->no_slow = code >= 182; /* the voiceless consonants (see msa_expand()) */
  } else if (code >= 200 && code < 200 + NUM_EFFECTS) {
    effect(s, code);
    sharp_onset(s, code);
    sound_shaping(s, code);
    s->copy1_length = 1;
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

  /* the timing in the second play and in the copies after it, when the
   * sound is repeated (see copy1_length) */
  int copy1_samples;
  int copy1_glide;
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

  /* the sound just set up starts with a voicing pulse (see KICK_HZ) */
  bool kick;
  bool auto_silenced;

  /* the length of the pauses since the last sound, in ms (see
   * msa_closure()) */
  double paused_ms;

  segment segs[MAX_PARTS];
  int nsegs;  /* segments in one copy */
  int seg;    /* the segment playing; busy while seg < nsegs */
  int copies; /* copies still to play after this one */
  bool in_copy; /* playing a copy after the first */
  int copy_k;   /* which copy is playing: 1 the second play */

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

/* at a low pitch the chip starts each speech sound (not each repeat copy)
 * with a voicing pulse at once, at full level and at the sound's own
 * formants, and restarts the voicing period there: on both units of the
 * second and third VoxProbe recordings, AW at pitch 0 is that one pulse, at
 * pitch 5 a pulse and another 200ms later. here below KICK_HZ (pitch 19 and
 * under); above it the restart made the recordings worse */
#define KICK_HZ 20.0

/* start playing the segments just set up */
static void msa_start(msa *m) {
  m->seg = 0;
  m->seg_pos = 0;
  m->seg_start = m->current;
  /* a sound kicked off at a low pitch (see KICK_HZ) starts at its own
   * frequencies, volumes and pitch, not gliding in */
  if (m->kick && m->nsegs > 0) {
    for (int i = 0; i < 3; i++) {
      m->seg_start.freq[i] = m->segs[0].target.freq[i];
      m->seg_start.vol[i] = m->segs[0].target.vol[i];
    }
    m->seg_start.pitch = m->segs[0].target.pitch;
    m->synth.kick = true;
  }
  m->kick = false;
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
      t.pitch = voice_hz((double)m->pitch) * (s->pitch_scale > 0 ? s->pitch_scale : 1);
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

    /* the copies (see copy1_length): a stop's keep their burst and release
     * and give the closure the rest of the copy's time */
    double rest = 0;
    for (int k = 1; k < s->nparts; k++)
      rest += s->parts[k].ms;
    for (int c = 0; c < 2; c++) {
      const double len = c == 0 ? s->copy1_length : s->copy_length;
      double copy_ms = p->ms * copy_scale * len;
      if (s->stop)
        copy_ms = (i == 0 ? fmax2((rest + s->parts[0].ms) * len - rest, 1.0) : p->ms) * copy_scale;
      double copy_glide = p->glide_ms * copy_scale;
      if (p->glide_ms == AUTO_GLIDE)
        copy_glide = fmin2(fmax2(copy_ms * 0.35, 20), 50);
      if (c == 0) {
        sg->copy1_samples = to_samples(copy_ms);
        sg->copy1_glide = to_samples(fmin2(copy_glide, copy_ms));
      } else {
        sg->copy_samples = to_samples(copy_ms);
        sg->copy_glide = to_samples(fmin2(copy_glide, copy_ms));
      }
    }
  }

  m->fast = m->slow = m->stress = m->relax = false;

  if (m->nsegs > 0 && m->jump_next_freq) {
    m->segs[0].jump = true;
    m->jump_next_freq = false;
  }

  m->copies = m->repeat > 1 ? m->repeat - 1 : 0;
  m->in_copy = false;
  m->copy_k = 0;
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
  const int glide = !m->in_copy ? sg->glide : m->copy_k == 1 ? sg->copy1_glide : sg->copy_glide;
  const int samples = !m->in_copy ? sg->samples : m->copy_k == 1 ? sg->copy1_samples : sg->copy_samples;
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
      m->copy_k++;
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
      /* the last sound before a pause, a command or the end (see
       * final_length) */
      if (code >= 128 && code < 128 + NUM_ALLOPHONES && m->repeat <= 1) {
        uint8_t next;
        if (!peek(sj, &next) || next < 128)
          s.length *= final_length[code - 128];
      }
      msa_expand(m, &s);
      m->kick = s.speech && voice_hz((double)m->pitch) < KICK_HZ;
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
