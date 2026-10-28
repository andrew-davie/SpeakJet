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
 * voicing pulse, every VOICE_FADE_STEP_MS from the pulse, in between in a
 * straight line, and after the last nothing (see ENV_VOICE in synth_sample()).
 * measured from the waveforms of the third VoxProbe recording, where at pitch
 * 40 each pulse stands alone and about 24 of them were averaged, on 18 vowels,
 * nasals and liquids on two AtariVoxes. each pulse was fitted as three sines
 * starting together, times one envelope shared by every sound, through one
 * output filter (see CHIP_RATE); the envelope and the filter were solved for
 * along with each sound's frequencies and amplitudes. that leaves 1/470 of the
 * pulses' energy unexplained (-26.7dB), against 1/28 for the straight 8ms ramp
 * first used. the fade starts at full level, dips to about 0.7 within half
 * a millisecond and comes back to about 0.8 (the onset shapes the whole
 * spectrum: smoothing it into a flat start costs the single pulses 1.8dB),
 * holds to 2ms, falls almost in a straight line and is gone by 7.5ms. fitted to either unit alone it fits the other within 0.2dB,
 * and fitted the same way to pulses this library makes it gives back this
 * library's own envelope, so the method doesn't shape it. Volume and Bend don't
 * change it, and nor does Pitch from 25 to 60 */
#define VOICE_FADE_STEP_MS 0.25
static const double voice_fade[] = {
    1.000, 0.807, 0.701, 0.892, 0.787, 0.800, 0.756, 0.734,
    0.753, 0.687, 0.660, 0.612, 0.598, 0.547, 0.513, 0.469,
    0.426, 0.386, 0.331, 0.288, 0.252, 0.246, 0.193, 0.134,
    0.150, 0.085, 0.096, 0.054, 0.040, 0.043, 0.003, 0.008,
    0.012, 0.000,
};
#define VOICE_FADE_N ((int)(sizeof(voice_fade) / sizeof(voice_fade[0])))

/* how long the chip takes to bring a vowel, nasal, liquid or diphthong's
 * voicing up from silence: in a straight line, from nothing to full level.
 * on the VoxRecord recording, both units, the first three voicing periods of
 * the 37 sounds played alone stand at -17, -6.3 and -1.5dB against the rest
 * (medians), which 40ms gives to within 1.4dB; ramping the volumes alone
 * gave -8, -2.7 and 0, because the mixer's limit (MAX_MIXER1) holds the sum
 * of the formants at full level long before the volumes get there. AW MM at
 * eight pitches from 60 to 250 takes about 30ms throughout on both units, so
 * it is a time rather than a number of periods. the voiced stops and
 * fricatives start as they did: the ramp on them made both units match them
 * worse. the nasals come up faster: measured from where they rise out of the
 * recording's background (which, being quieter sounds, they sit nearer to),
 * both units have them within 3dB of full level by the second period. 25ms
 * fits them best (40ms 90.7%, 25ms 91.4%) */
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
/* the chip's output. it makes its samples at CHIP_RATE, not the manual's
 * 8192 a second: on both units of the VoxRecord recording every steady tone
 * has a mirror image at the rate less the tone (B7's 3915Hz at 4124Hz, 1dB
 * down), which puts the rate at 8040Hz on one unit and 8015Hz on the other
 * (their clocks differ by the same 0.3 percent as their pitches), the same as
 * their voicing ticks. each sample is held until the next, so the images
 * repeat above that, around each multiple of the rate, at the levels holding
 * gives, up to 23kHz on the recordings, and a smooth analogue low pass after
 * the chip shapes them all.
 *
 * that low pass is measured twice. below 4kHz the voicing pulses of the third
 * VoxProbe recording give it, level and phase, on both units: -1.2dB at 1kHz,
 * -2.8 at 1.5, -5.4 at 2, -8.9 at 2.5, -11.8 at 3 and -14.3 at 3.5kHz against
 * 500Hz, holding included (it agrees within 2 to 3dB with the tone sweeps of
 * the VoxRecord recording, -10.8 at 2.9 and -15.8 at 3.6kHz). above 4kHz the
 * images give it: 160 of them from 26 tone effects on both units, each against
 * its own tone, less what holding accounts for (within 2dB of each other):
 * -18dB at 5.5kHz, -23 at 7.5, -27 at 10.5, -33 at 15.5 and -38 at 23.5kHz. it
 * includes whatever the AtariVox and the recorder add.
 *
 * so here the chip's samples are held, each output sample is the average of
 * what is held over its time, and output_sos (a filter at SJ_SAMPLE_RATE,
 * fitted to both measurements, within 0.3dB RMS below 4kHz and 1.6dB above)
 * is the low pass. the level at 500Hz is the one the output had before */
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

/* the chip's output has a floor of noise under whatever it plays: on both
 * units of the VoxRecord recording a pure tone (the DTMF tones, B3, R3 and
 * the rest) sits on noise from 100Hz to 3kHz 40 to 55dB under it, above the
 * recording's background, which this library's clean tones left out, every
 * band between the tones counting as missing. here white noise follows the
 * output's level (over about 25ms) SKIRT_DB under it: the effects of the
 * VoxRecord recording 5.94 to 4.57dB with their spectrogram match within a
 * tenth of a point, and the speech unchanged (it hides under the harmonics) */
#define SKIRT_DB (-24.0)
#define SKIRT_FOLLOW 0.995

/* the consonants' shaping (see sound_eq): EQ_BANDS peaking filters half
 * an octave apart, each made again whenever the gain asked of it changes,
 * then the overall gain. not when it has moved some step: compilers that fuse
 * a multiply and an add (clang on arm64) and those that don't (gcc on x86)
 * differ in a gliding gain's last bit, which moved the sample a step was
 * crossed on, and the filters carried that on, 21 apart in the pangram */
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

/* the most parts any sound has (a traced effect: see fx2_points) */
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
 * vowels barely clip, confirms AX, UX, EH, EY, IH, OW, UH, LO and RR to
 * within about 5% (formants read by linear prediction, the same way on the
 * recording and on this library's own output, so that the method's errors
 * cancel). it corrects five: AW F1 (590 to 695Hz), UW F1 and F2 (300 to 380,
 * 870 to 1065), AY F1 and F2 (660 to 710, 1720 to 1660), OH F1 and F2 (730
 * to 690, 1090 to 1150) and WW F1 and F2 (300 to 495, 650 to 765). IY
 * can't be read this way. the nasals (MM among them) are fitted to its
 * spectra instead (see allophone()).
 *
 * the second VoxProbe recording then measured the oscillators directly: at
 * pitch 40 each voicing pulse stands alone, a few milliseconds of the three
 * formant sines starting together, so their frequencies can be fitted to the
 * waveform itself. IY, IH, EY, EH, AY (but its weak F3), AX, UX, OH, AW, OW,
 * UH, UW, LE, WW, RR and MM take those frequencies (NE and NO were worse with
 * them on both recordings, and keep theirs). the biggest changes: IY F3 3010
 * to 2539Hz, LE F2 1300 to 970, WW F1 495 to 385, MM F1 396 to 257. with them
 * the sustained sounds of the first VoxProbe recording (not used to measure
 * them) go from 4.5 to 3.7dB of the chip's spectra.
 *
 * the third VoxProbe recording measures them again, on two units, with the
 * chip's own fade and output filter (see voice_fade): IY, IH, EY, EH, AY, AX,
 * UX, OH, AW, OW, UH, UW, LE, WW, RR, MM, NE and NO are those, the mean of
 * both units (unit B's clock is 0.29% slow), which agree within a few hertz.
 * F1 and F2 of all 18 are whole multiples of about 88.4Hz less 3.5Hz, within
 * 2.2Hz RMS */
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

/* the chip doesn't restart all three formant sines at the same phase: fitted
 * to the single pulses of the third VoxProbe recording with the chip's fade
 * and output filter (see voice_fade), each sound's second and third formants
 * start at their own phase against the first, the same on both units (IH's
 * 0.13 and 0.17 of a cycle ahead, MM's 0.25 and 0.27, IY's 0.92 and 0.93). they
 * shape the dips between the formants: restarting all three together, as
 * first assumed here, leaves those dips up to 10dB too deep (IY, MM, NE) and
 * the single pulses 4.5dB from the chip's rather than 2.0. formant_phases
 * gives a voiced frame those phases. but measured at pitch 40, they don't all
 * hold at speaking pitch: against the VoxRecord recording at the default
 * pitch (background removed, unit B and unit A's unclipped sounds) and the
 * first VoxProbe recording at pitches 88 and 60 (which nothing here was
 * fitted to), IY, EY, EH, AY, AX, UX, OH, AW and MM match better without
 * theirs (IY 85 to 91% and 3.2 to 2.0dB, and 3.4 to 2.2dB on VoxProbe), so
 * those restart together; OW, UH, UW, NGE, NGO, NE, LE, WW and RR keep
 * theirs */
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
 * allophone_gain_db. all of them were then fitted again to both units at once
 * (see STOPN()) */
static frame frSE(void) { return noise_frame(lAlveolar, 3629.7, 2609.1, 25.87, 5.78, 33.4); }
static frame frSO(void) { return noise_frame(lAlveolarBak, 2953.5, 3580, 17.5, 30.41, 27.3); }
static frame frSH(void) { return noise_frame(lPalatal, 3605.7, 2502.8, 17.72, 22.59, 44.4); }
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
 * the chip's bursts are lower than first chosen here (PE and PO by half).
 *
 * all of them but BE, BO, DO, ED, OD, GE, GO, EG and OG (which are played from
 * the chip's frames: see vf_points) and TT were then fitted again, with the
 * output filter measured from the voicing pulses (see CHIP_RATE), to both
 * units' VoxRecord recordings at once: the spectrogram, the spectrum and the
 * level over time, each unit's level compared down to its own background, so
 * that a part the chip doesn't play costs as much as one it plays wrongly
 * (the first fit had put faint tails and tones at the background's level,
 * where they cost nothing). each keeps the length it had (see
 * allophone_length), which comes from the connected groups; fitted to the
 * sounds alone the lengths came out shorter, and connected speech went from
 * 21 to 60ms out. TT keeps its values, which the connected groups fix: its
 * refit made groups ending in TT 100ms short. with these KE went from 54 to
 * 74% spectrogram match and 11.3 to 4.2dB, KO 55 to 81%, EK 71 to 88%, OK 68
 * to 86%, DE 44 to 86%, JH 49 to 80% and CH 65 to 75% */
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
 * connected groups. that takes the groups to within 2% (12ms on average).
 * TS is 190ms, measured from both units
 *
 * the vowels, nasals, liquids and diphthongs (codes 128 to 164) are 0 here:
 * they are the manual's length and TRANSITION_MS more */
static const double allophone_length[] = {
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00,
    0.00, 0.8686, 0.84, 0.99, 0.88, 0.88, 1.63, 1.40, 2.67,
    3.27, 1.40, 1.40, 3.74, 3.16, 1.40, 1.40, 1.40, 1.40,
    2.4090, 1.17, 1.15, 1.11, 1.49, 2.80, 2.23, 1.17, 1.77,
    2.07, 2.78, 1.117647, 2.44, 1.40, 2.40, 2.46, 1.40, 1.40,
};
/* LENGTH-TABLE-END */

/* REFIT-BEGIN */
/* each allophone's length and level refitted on every recording of both
 * AtariVoxes (VoxRecord's sounds alone and its connected groups, the fourth
 * VoxProbe, Man Goes Down), the library given each recording's own background
 * and both measured by the same rule: length, level over time, the
 * time-frequency picture after alignment, timing inside the sound, sideways
 * shifts of the spectrum, the whole-sound spectrum and pitch, each against how
 * closely the two units agree with each other. each sound's fit was kept only
 * where no kind of recording came out worse.
 *
 * refit_length multiplies the length above. final_length multiplies it again
 * when the sound is the last before a pause or the end and isn't repeated: on
 * the chip a vowel played last is longer than the same vowel inside a phrase
 * (IY 1.25, AW 1.25, EH 1.18 times) and TT and FF shorter (0.64, 0.79), which
 * no one length fitted both ways (IY alone wanted 1.125 times, Man Goes Down
 * and the VoxProbe groups shorter). refit_gain_db is in dB on the sound's
 * level, fitted where sounds sit next to each other */
static const double refit_length[] = {
    1, 1, 1.4, 1.05, 1.4875, 1.4, 1, 1.2375, 1,
    1.2, 1.2, 1, 1, 0.95, 1.0375, 0.8875, 0.9625, 1,
    1.0625, 1.1, 1.1375, 1, 1.0875, 1, 1.025, 1.0625, 1,
    1.1, 1, 1, 1.05, 1.0625, 1, 1.05, 1, 1.0125,
    1.025, 1.025, 0.9875, 0.9375, 1.175, 1, 1, 0.975, 1.075,
    1, 0.9625, 1, 0.8375, 0.8875, 1.0875, 1, 1, 0.8125,
    1.1625, 1, 1, 1, 1, 0.875, 0.825, 1, 0.725,
    1, 0.9625, 0.825, 1.0375, 0.775, 1.0375, 1.1, 1.0125, 1.0375,
};
static const double final_length[] = {
    1.25, 1.175, 1, 1.175, 1, 1, 1, 1, 1.25,
    1, 1, 1.15, 1, 1, 0.9, 1, 1, 1,
    1, 1.05, 0.9875, 1.1375, 1, 1, 1, 0.9125, 1,
    1, 1.0375, 1.2375, 0.925, 0.975, 1.0375, 1, 1.025, 1.0625,
    1, 1, 1.25, 1, 1, 1, 1, 1, 1,
    1, 0.975, 0.8875, 1.2625, 1.2, 1, 1.075, 1.2125, 1,
    1, 1, 1, 1, 0.7875, 0.9625, 1, 1, 1,
    0.6375, 1.05, 1, 1, 1.3, 1, 0.8, 1, 0.925,
};
static const double refit_gain_db[] = {
    2.4375, 0, 0, 0, 0, 0, 0, 3.1875, 0,
    0, 0, 0, -2.0625, -6.375, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 4.3125, 0, 0, 0, 0, 0,
    0, -3, 0, 0, 0, 0, 0, 0, 0,
    0, 5.8125, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -4.875, 0, 0, 0,
};
/* REFIT-END */

/* how much louder (in dB) the chip plays each allophone than the oscillator
 * volumes here give, against AW. fitted to the VoxRecord recording's third
 * take, whose vowels barely clip: each allophone's level (where it is
 * within 20dB of its loudest) was matched to the recording's, measured the
 * same way, relative to AW. the vowels needed little; the nasals, liquids and
 * hiss were too loud here (the hiss levels were set from an earlier, clipped
 * recording, which made the vowels read quiet) and BE, BO, PE and PO too
 * quiet */
static const double allophone_gain_db[] = {
    1.1, 2.0, 2.0, 1.2, 2.0, 1.7, 1.3, 0.8, 0.0,
    -1.8, -1.9, -1.3, -3.9, -4.6, -4.5, -4.5, -4.7, -4.2,
    -3.2, -4.2, 0.4, 0.3, 0.8, 0.2, -0.5, -1.1, 1.2,
    0.5, -0.7, 0.9, 0.4, -0.9, -1.0, -0.2, -1.6, -0.6,
    -3.0, -11.3, -2.1, 1.7, -2.2, -3.3, 5.9, 6.3, -2.6,
    2.7, 2.9, 3.0, -4.6, -8.6, -0.4, -0.5, 1.7, 2.1,
    -15.5, -32.3, -31.1, -29.8, -26.3, -15.9, -17.6, -16.6, -26.8,
    -2.1, -3.3, -26.4, 1.5, -3.0, 2.5, -3.8, 6.4, 5.9,
};

/* VF-BEGIN */
/* some voiced sounds are played as the chip plays them, frame by frame. at
 * the default pitch each voicing pulse of the VoxRecord recording stands
 * alone (the fade is over long before the next pulse), so each pulse of every
 * voiced sound alone was fitted as three formant sines with the chip's fade
 * and output filter (see voice_fade), on both units, and the two units'
 * pulses paired and averaged. each point is a pulse: its time after the one
 * before, the glide to it, the three frequencies and their volumes (an
 * absolute level: AW's first formant is 24). a formant too weak to measure
 * takes the frequency it has where it is strongest. these are the sounds
 * whose recordings this brings closer on both units: the diphthongs, which
 * move between their formants in steps rather than one straight line, the
 * voiced fricatives, which on the chip are mostly voicing, and voiced stops
 * such as BO and GE, which are a voice bar and then a short vowel-like
 * release with no noise burst to speak of. their length is the one measured,
 * not stretched (see allophone_length) */
typedef struct vf_point {
  float ms, glide;
  float freq[3];
  float vol[3];
} vf_point;

static const vf_point vf_points[] = {
    {1.00, 1.00, {352, 1750, 2524}, {10.64, 7.00, 4.52}}, /* IH */
    {11.18, 11.18, {351, 1767, 2486}, {21.55, 16.20, 9.46}}, /* IH */
    {11.54, 11.54, {347, 1772, 2475}, {23.65, 18.11, 10.93}}, /* IH */
    {11.46, 11.46, {348, 1771, 2471}, {23.66, 18.01, 11.14}}, /* IH */
    {11.44, 11.44, {347, 1771, 2471}, {23.64, 18.06, 11.16}}, /* IH */
    {11.46, 11.46, {347, 1766, 2483}, {23.65, 18.16, 10.79}}, /* IH */
    {11.41, 11.41, {347, 1772, 2474}, {23.59, 18.04, 10.94}}, /* IH */
    {11.46, 11.46, {346, 1770, 2471}, {22.77, 17.49, 10.08}}, /* IH */
    {11.56, 11.56, {346, 1772, 2477}, {15.41, 12.37, 6.42}}, /* IH */
    {12.69, 12.69, {348, 1751, 2497}, {5.84, 5.68, 2.93}}, /* IH */
    {11.50, 11.50, {348, 1751, 2497}, {5.84, 5.68, 2.93}}, /* IH */
    {1.00, 1.00, {251, 1149, 2596}, {0.81, 0.23, 0.25}}, /* NO */
    {12.39, 12.39, {254, 1116, 2604}, {7.53, 1.69, 3.23}}, /* NO */
    {11.41, 11.41, {251, 1142, 2601}, {9.26, 2.04, 4.02}}, /* NO */
    {11.44, 11.44, {253, 1154, 2596}, {9.10, 2.01, 4.29}}, /* NO */
    {11.44, 11.44, {251, 1146, 2620}, {9.21, 2.01, 4.02}}, /* NO */
    {11.44, 11.44, {253, 1149, 2590}, {9.21, 2.08, 3.99}}, /* NO */
    {11.46, 11.46, {253, 1154, 2599}, {9.20, 2.04, 4.05}}, /* NO */
    {11.52, 11.52, {260, 1157, 2555}, {6.99, 1.31, 2.92}}, /* NO */
    {11.50, 11.50, {260, 1157, 2555}, {6.99, 1.31, 2.92}}, /* NO */
    {1.00, 1.00, {442, 875, 2394}, {4.79, 5.02, 0.42}}, /* LO */
    {12.53, 12.53, {446, 875, 2465}, {9.33, 10.33, 1.45}}, /* LO */
    {11.46, 11.46, {446, 875, 2458}, {9.32, 10.31, 1.50}}, /* LO */
    {11.44, 11.44, {447, 876, 2459}, {9.34, 10.37, 1.51}}, /* LO */
    {11.44, 11.44, {447, 875, 2463}, {9.28, 10.33, 1.46}}, /* LO */
    {11.44, 11.44, {447, 876, 2394}, {9.30, 10.39, 1.70}}, /* LO */
    {11.46, 11.46, {447, 874, 2390}, {9.30, 10.28, 1.61}}, /* LO */
    {11.39, 11.39, {443, 869, 2450}, {5.54, 6.05, 0.56}}, /* LO */
    {11.50, 11.50, {443, 869, 2450}, {5.54, 6.05, 0.56}}, /* LO */
    {1.00, 1.00, {358, 1336, 2487}, {3.97, 2.40, 2.83}}, /* IYRR */
    {12.15, 12.15, {234, 2208, 2552}, {12.00, 8.04, 6.25}}, /* IYRR */
    {11.56, 11.56, {248, 2199, 2522}, {14.79, 10.39, 8.21}}, /* IYRR */
    {11.50, 11.50, {251, 2123, 2494}, {15.21, 9.36, 8.26}}, /* IYRR */
    {11.50, 11.50, {262, 2061, 2441}, {15.28, 8.68, 8.73}}, /* IYRR */
    {11.48, 11.48, {256, 2048, 2441}, {16.38, 8.91, 7.61}}, /* IYRR */
    {11.07, 11.07, {279, 1936, 2295}, {16.90, 9.59, 8.69}}, /* IYRR */
    {11.47, 11.47, {289, 1906, 2259}, {16.36, 10.86, 6.08}}, /* IYRR */
    {11.49, 11.49, {311, 1860, 2226}, {16.03, 10.48, 6.62}}, /* IYRR */
    {11.56, 11.56, {324, 1790, 2259}, {15.30, 11.65, 3.39}}, /* IYRR */
    {10.02, 10.02, {351, 1712, 2441}, {16.56, 12.69, 2.11}}, /* IYRR */
    {11.46, 11.46, {358, 1692, 2412}, {18.58, 14.54, 2.78}}, /* IYRR */
    {12.40, 12.40, {369, 1646, 2441}, {16.97, 13.38, 1.59}}, /* IYRR */
    {11.48, 11.48, {375, 1591, 2441}, {16.78, 13.13, 1.71}}, /* IYRR */
    {11.60, 11.60, {396, 1510, 2441}, {16.18, 12.84, 1.04}}, /* IYRR */
    {12.36, 12.36, {398, 1471, 2503}, {13.98, 11.22, 2.27}}, /* IYRR */
    {11.62, 11.62, {417, 1396, 2334}, {11.07, 8.89, 2.40}}, /* IYRR */
    {11.55, 11.55, {439, 1330, 2441}, {10.25, 9.06, 1.20}}, /* IYRR */
    {9.13, 9.13, {425, 1336, 2441}, {17.90, 15.13, 1.72}}, /* IYRR */
    {11.73, 11.73, {429, 1322, 2441}, {10.24, 9.55, 1.29}}, /* IYRR */
    {11.50, 11.50, {429, 1322, 2441}, {10.24, 9.55, 1.29}}, /* IYRR */
    {1.00, 1.00, {532, 1502, 2368}, {12.05, 7.97, 5.86}}, /* AXRR */
    {11.89, 11.89, {530, 1501, 2374}, {23.40, 15.20, 10.74}}, /* AXRR */
    {11.78, 11.78, {525, 1500, 2347}, {26.63, 16.67, 12.24}}, /* AXRR */
    {11.48, 11.48, {520, 1490, 2295}, {26.49, 16.89, 12.04}}, /* AXRR */
    {11.40, 11.40, {514, 1474, 2250}, {26.24, 16.54, 10.32}}, /* AXRR */
    {11.49, 11.49, {507, 1468, 2426}, {25.00, 16.61, 6.10}}, /* AXRR */
    {11.54, 11.54, {502, 1461, 2230}, {24.25, 16.01, 10.16}}, /* AXRR */
    {11.45, 11.45, {499, 1446, 2138}, {23.77, 16.42, 8.41}}, /* AXRR */
    {11.48, 11.48, {493, 1437, 2084}, {23.02, 16.27, 7.66}}, /* AXRR */
    {11.45, 11.45, {486, 1427, 2042}, {23.00, 16.12, 6.56}}, /* AXRR */
    {11.46, 11.46, {483, 1414, 1959}, {21.75, 15.30, 6.74}}, /* AXRR */
    {11.48, 11.48, {476, 1408, 1933}, {21.41, 15.09, 6.00}}, /* AXRR */
    {11.46, 11.46, {468, 1384, 1943}, {21.36, 14.98, 5.50}}, /* AXRR */
    {11.50, 11.50, {463, 1377, 2347}, {19.81, 15.16, 2.10}}, /* AXRR */
    {11.46, 11.46, {456, 1373, 2347}, {19.87, 15.25, 2.11}}, /* AXRR */
    {11.44, 11.44, {449, 1358, 2347}, {19.37, 14.87, 2.31}}, /* AXRR */
    {11.44, 11.44, {447, 1355, 2347}, {18.55, 15.28, 2.04}}, /* AXRR */
    {10.89, 10.89, {435, 1340, 2347}, {19.49, 16.07, 0.90}}, /* AXRR */
    {11.48, 11.48, {433, 1334, 2347}, {15.41, 13.03, 0.82}}, /* AXRR */
    {12.06, 12.06, {436, 1324, 2347}, {7.40, 6.72, 0.73}}, /* AXRR */
    {11.50, 11.50, {436, 1324, 2347}, {7.40, 6.72, 0.73}}, /* AXRR */
    {1.00, 1.00, {726, 930, 2530}, {7.32, 6.79, 2.55}}, /* AWRR */
    {12.36, 12.36, {708, 964, 2486}, {19.98, 17.80, 5.78}}, /* AWRR */
    {11.40, 11.40, {694, 981, 2460}, {22.75, 21.26, 6.22}}, /* AWRR */
    {11.48, 11.48, {676, 1003, 2397}, {22.60, 20.86, 6.42}}, /* AWRR */
    {11.42, 11.42, {662, 1025, 2350}, {22.83, 20.99, 6.31}}, /* AWRR */
    {11.42, 11.42, {646, 1047, 2345}, {22.62, 21.16, 6.17}}, /* AWRR */
    {11.40, 11.40, {631, 1066, 2247}, {23.13, 20.87, 6.14}}, /* AWRR */
    {11.54, 11.54, {615, 1090, 2207}, {22.91, 20.36, 5.74}}, /* AWRR */
    {11.36, 11.36, {599, 1110, 2164}, {23.02, 20.37, 5.53}}, /* AWRR */
    {12.73, 12.73, {590, 1127, 2130}, {20.81, 17.55, 5.24}}, /* AWRR */
    {11.42, 11.42, {582, 1139, 2148}, {17.76, 15.72, 4.41}}, /* AWRR */
    {11.44, 11.44, {563, 1178, 2094}, {17.64, 15.89, 3.77}}, /* AWRR */
    {11.44, 11.44, {547, 1195, 1971}, {17.72, 15.93, 4.21}}, /* AWRR */
    {11.41, 11.41, {529, 1233, 2229}, {18.15, 15.80, 4.08}}, /* AWRR */
    {11.44, 11.44, {511, 1247, 2153}, {18.10, 15.97, 2.93}}, /* AWRR */
    {11.48, 11.48, {492, 1283, 2168}, {18.16, 15.73, 4.00}}, /* AWRR */
    {11.39, 11.39, {470, 1305, 2388}, {18.57, 15.85, 2.19}}, /* AWRR */
    {12.54, 12.54, {453, 1308, 2382}, {13.33, 10.73, 4.80}}, /* AWRR */
    {11.42, 11.42, {448, 1321, 2405}, {11.59, 9.75, 2.80}}, /* AWRR */
    {10.25, 10.25, {446, 1318, 2397}, {9.51, 9.62, 0.97}}, /* AWRR */
    {11.50, 11.50, {446, 1318, 2397}, {9.51, 9.62, 0.97}}, /* AWRR */
    {1.00, 1.00, {519, 910, 2400}, {1.06, 0.89, 0.85}}, /* OWRR */
    {12.89, 12.89, {562, 865, 2400}, {6.87, 7.72, 0.86}}, /* OWRR */
    {9.68, 9.68, {529, 883, 2427}, {16.52, 20.16, 4.58}}, /* OWRR */
    {11.48, 11.48, {522, 910, 2391}, {18.00, 21.92, 5.28}}, /* OWRR */
    {11.49, 11.49, {519, 937, 2315}, {18.26, 21.88, 5.42}}, /* OWRR */
    {13.48, 13.48, {524, 950, 2400}, {11.75, 13.56, 7.44}}, /* OWRR */
    {10.74, 10.74, {520, 993, 2313}, {14.28, 16.08, 2.91}}, /* OWRR */
    {11.34, 11.34, {517, 1024, 2400}, {14.71, 16.57, 1.08}}, /* OWRR */
    {11.43, 11.43, {508, 1050, 2511}, {14.71, 16.30, 1.96}}, /* OWRR */
    {11.44, 11.44, {497, 1083, 2555}, {14.91, 15.93, 3.20}}, /* OWRR */
    {11.29, 11.29, {486, 1113, 2619}, {15.74, 16.09, 3.04}}, /* OWRR */
    {11.38, 11.38, {484, 1147, 2584}, {17.47, 16.34, 3.30}}, /* OWRR */
    {11.56, 11.56, {479, 1170, 2583}, {17.39, 16.17, 2.47}}, /* OWRR */
    {11.37, 11.37, {473, 1205, 2528}, {17.05, 15.93, 1.88}}, /* OWRR */
    {11.44, 11.44, {464, 1236, 2379}, {17.32, 15.97, 2.06}}, /* OWRR */
    {11.49, 11.49, {456, 1269, 2518}, {17.10, 15.56, 2.35}}, /* OWRR */
    {11.25, 11.25, {451, 1305, 2583}, {17.41, 15.90, 1.92}}, /* OWRR */
    {12.62, 12.62, {450, 1322, 2482}, {12.43, 11.20, 4.01}}, /* OWRR */
    {9.59, 9.59, {449, 1316, 2400}, {11.05, 9.98, 1.70}}, /* OWRR */
    {13.29, 13.29, {450, 910, 2400}, {5.79, 5.13, 0.38}}, /* OWRR */
    {11.50, 11.50, {450, 910, 2400}, {5.79, 5.13, 0.38}}, /* OWRR */
    {1.00, 1.00, {367, 1006, 2461}, {0.43, 0.48, 0.48}}, /* OWIY */
    {13.15, 13.15, {551, 871, 2461}, {5.79, 6.16, 1.14}}, /* OWIY */
    {12.23, 12.23, {552, 904, 2543}, {10.17, 12.25, 4.25}}, /* OWIY */
    {11.38, 11.38, {534, 917, 2503}, {11.71, 13.00, 7.05}}, /* OWIY */
    {10.49, 10.49, {518, 1006, 2427}, {14.37, 16.32, 4.82}}, /* OWIY */
    {11.50, 11.50, {498, 1081, 2428}, {14.59, 15.80, 5.49}}, /* OWIY */
    {11.38, 11.38, {478, 1156, 2393}, {15.07, 15.69, 5.67}}, /* OWIY */
    {11.38, 11.38, {457, 1231, 2424}, {16.43, 15.58, 3.73}}, /* OWIY */
    {10.81, 10.81, {436, 1308, 2429}, {14.99, 13.52, 7.32}}, /* OWIY */
    {11.43, 11.43, {418, 1380, 2456}, {18.27, 16.19, 7.37}}, /* OWIY */
    {12.54, 12.54, {417, 1443, 2457}, {14.70, 12.58, 5.85}}, /* OWIY */
    {11.27, 11.27, {390, 1537, 2517}, {15.50, 12.45, 6.37}}, /* OWIY */
    {11.38, 11.38, {380, 1597, 2595}, {16.72, 12.66, 4.93}}, /* OWIY */
    {10.52, 10.52, {367, 1665, 2473}, {18.91, 15.11, 8.64}}, /* OWIY */
    {11.42, 11.42, {357, 1724, 2461}, {18.40, 14.76, 9.17}}, /* OWIY */
    {12.86, 12.86, {336, 1809, 2522}, {16.10, 13.08, 7.15}}, /* OWIY */
    {11.21, 11.21, {311, 1893, 2554}, {16.15, 10.91, 6.30}}, /* OWIY */
    {11.31, 11.31, {294, 1946, 2548}, {16.52, 10.38, 7.09}}, /* OWIY */
    {11.87, 11.87, {260, 2042, 2536}, {16.66, 9.22, 7.40}}, /* OWIY */
    {11.43, 11.43, {243, 2092, 2561}, {16.16, 8.68, 8.00}}, /* OWIY */
    {11.39, 11.39, {245, 2198, 2536}, {15.07, 9.97, 8.38}}, /* OWIY */
    {11.52, 11.52, {232, 2231, 2561}, {14.13, 8.52, 7.00}}, /* OWIY */
    {12.68, 12.68, {249, 2232, 2545}, {7.17, 5.18, 3.47}}, /* OWIY */
    {11.50, 11.50, {249, 2232, 2545}, {7.17, 5.18, 3.47}}, /* OWIY */
    {1.00, 1.00, {595, 1732, 2592}, {1.13, 0.41, 0.39}}, /* IYEH */
    {13.69, 13.69, {595, 2137, 2553}, {5.36, 3.89, 3.71}}, /* IYEH */
    {10.56, 10.56, {263, 2190, 2496}, {13.73, 10.61, 7.87}}, /* IYEH */
    {12.67, 12.67, {255, 2169, 2526}, {16.77, 10.04, 8.61}}, /* IYEH */
    {11.47, 11.47, {262, 2146, 2592}, {17.51, 10.52, 8.76}}, /* IYEH */
    {11.04, 11.04, {310, 2081, 2510}, {17.74, 10.01, 9.51}}, /* IYEH */
    {11.46, 11.46, {347, 2055, 2518}, {19.26, 10.71, 10.32}}, /* IYEH */
    {11.41, 11.41, {377, 1999, 2592}, {19.86, 9.44, 11.70}}, /* IYEH */
    {11.46, 11.46, {410, 1944, 2592}, {19.88, 9.37, 10.56}}, /* IYEH */
    {11.05, 11.05, {433, 1944, 2461}, {22.43, 12.88, 9.16}}, /* IYEH */
    {11.50, 11.50, {464, 1901, 2444}, {23.21, 13.24, 8.47}}, /* IYEH */
    {11.41, 11.41, {494, 1840, 2477}, {23.16, 12.84, 8.85}}, /* IYEH */
    {11.46, 11.46, {524, 1793, 2512}, {22.79, 12.85, 7.46}}, /* IYEH */
    {11.38, 11.38, {549, 1771, 2577}, {24.62, 11.59, 9.68}}, /* IYEH */
    {11.47, 11.47, {575, 1726, 2484}, {24.11, 11.74, 9.96}}, /* IYEH */
    {11.12, 11.12, {595, 1732, 2392}, {26.14, 15.40, 11.58}}, /* IYEH */
    {11.33, 11.33, {613, 1687, 2356}, {25.88, 15.32, 10.01}}, /* IYEH */
    {11.97, 11.97, {613, 1696, 2335}, {18.88, 11.07, 7.06}}, /* IYEH */
    {13.22, 13.22, {610, 1692, 2342}, {9.04, 5.22, 3.41}}, /* IYEH */
    {11.50, 11.50, {610, 1692, 2342}, {9.04, 5.22, 3.41}}, /* IYEH */
    {1.00, 1.00, {617, 1681, 2416}, {0.57, 0.38, 0.19}}, /* EHLL */
    {10.47, 10.47, {617, 1681, 2416}, {4.82, 3.19, 2.38}}, /* EHLL */
    {11.83, 11.83, {618, 1688, 2357}, {17.73, 10.48, 7.59}}, /* EHLL */
    {12.37, 12.37, {617, 1681, 2353}, {25.21, 14.61, 10.75}}, /* EHLL */
    {11.51, 11.51, {601, 1607, 2408}, {24.98, 14.31, 10.23}}, /* EHLL */
    {11.47, 11.47, {587, 1550, 2416}, {24.03, 13.82, 10.94}}, /* EHLL */
    {12.49, 12.49, {579, 1469, 2454}, {19.73, 12.32, 7.48}}, /* EHLL */
    {10.43, 10.43, {558, 1414, 2503}, {21.30, 13.43, 8.51}}, /* EHLL */
    {11.58, 11.58, {545, 1361, 2551}, {19.05, 13.16, 6.49}}, /* EHLL */
    {11.56, 11.56, {532, 1288, 2466}, {17.32, 12.10, 3.41}}, /* EHLL */
    {11.43, 11.43, {515, 1227, 2485}, {15.96, 12.00, 4.87}}, /* EHLL */
    {11.25, 11.25, {498, 1158, 2630}, {14.83, 11.29, 3.62}}, /* EHLL */
    {11.70, 11.70, {481, 1096, 2580}, {12.90, 11.26, 3.74}}, /* EHLL */
    {11.59, 11.59, {471, 1037, 2633}, {11.05, 10.59, 3.05}}, /* EHLL */
    {11.53, 11.53, {457, 971, 2416}, {9.68, 10.34, 1.19}}, /* EHLL */
    {11.47, 11.47, {617, 965, 2416}, {5.21, 6.13, 0.49}}, /* EHLL */
    {11.50, 11.50, {617, 965, 2416}, {5.21, 6.13, 0.49}}, /* EHLL */
    {1.00, 1.00, {214, 1118, 2504}, {4.23, 2.83, 2.45}}, /* IYUW */
    {12.44, 12.44, {208, 2176, 2548}, {14.05, 7.71, 5.76}}, /* IYUW */
    {11.08, 11.08, {214, 2208, 2507}, {18.49, 9.82, 8.01}}, /* IYUW */
    {11.49, 11.49, {242, 2081, 2548}, {15.07, 9.33, 7.62}}, /* IYUW */
    {11.49, 11.49, {260, 2045, 2552}, {14.67, 8.20, 8.25}}, /* IYUW */
    {11.02, 11.02, {256, 1926, 2439}, {16.79, 10.54, 7.65}}, /* IYUW */
    {11.49, 11.49, {265, 1837, 2504}, {15.25, 11.07, 6.06}}, /* IYUW */
    {10.23, 10.23, {305, 1767, 2368}, {15.78, 14.49, 7.55}}, /* IYUW */
    {11.51, 11.51, {303, 1680, 2360}, {16.44, 13.51, 7.94}}, /* IYUW */
    {13.04, 13.04, {289, 1598, 2419}, {13.28, 9.14, 3.37}}, /* IYUW */
    {9.99, 9.99, {311, 1523, 2345}, {15.92, 14.61, 5.33}}, /* IYUW */
    {11.47, 11.47, {319, 1447, 2269}, {15.83, 14.54, 5.28}}, /* IYUW */
    {12.52, 12.52, {311, 1370, 2225}, {15.07, 12.61, 5.51}}, /* IYUW */
    {11.55, 11.55, {320, 1273, 2336}, {12.66, 10.78, 2.81}}, /* IYUW */
    {10.26, 10.26, {333, 1205, 2198}, {13.66, 12.58, 3.89}}, /* IYUW */
    {11.54, 11.54, {333, 1118, 2321}, {15.55, 14.63, 3.30}}, /* IYUW */
    {11.46, 11.46, {336, 1064, 2342}, {14.98, 14.53, 3.10}}, /* IYUW */
    {12.12, 12.12, {326, 1062, 2552}, {8.86, 8.49, 0.65}}, /* IYUW */
    {11.50, 11.50, {326, 1062, 2552}, {8.86, 8.49, 0.65}}, /* IYUW */
    {1.00, 1.00, {675, 1207, 2378}, {2.26, 2.07, 1.53}}, /* AXUW */
    {10.99, 10.99, {531, 1504, 2358}, {13.51, 8.71, 6.50}}, /* AXUW */
    {11.64, 11.64, {530, 1500, 2376}, {24.66, 15.78, 11.67}}, /* AXUW */
    {12.77, 12.77, {546, 1465, 2378}, {22.59, 14.49, 11.76}}, /* AXUW */
    {11.49, 11.49, {570, 1437, 2385}, {22.55, 15.03, 11.33}}, /* AXUW */
    {11.47, 11.47, {589, 1397, 2409}, {22.13, 14.82, 10.42}}, /* AXUW */
    {10.37, 10.37, {605, 1357, 2379}, {25.60, 16.82, 11.24}}, /* AXUW */
    {11.46, 11.46, {624, 1314, 2389}, {24.82, 16.60, 10.32}}, /* AXUW */
    {11.47, 11.47, {645, 1280, 2407}, {24.58, 16.61, 10.70}}, /* AXUW */
    {11.51, 11.51, {663, 1238, 2411}, {23.91, 16.86, 10.89}}, /* AXUW */
    {11.48, 11.48, {675, 1207, 2415}, {26.24, 18.88, 11.19}}, /* AXUW */
    {11.88, 11.88, {699, 1180, 2450}, {20.31, 15.66, 8.64}}, /* AXUW */
    {11.51, 11.51, {683, 1137, 2432}, {23.12, 17.14, 8.71}}, /* AXUW */
    {11.64, 11.64, {594, 1120, 2423}, {18.48, 14.54, 5.84}}, /* AXUW */
    {10.82, 10.82, {515, 1090, 2341}, {18.61, 15.28, 3.74}}, /* AXUW */
    {11.56, 11.56, {407, 1080, 2511}, {16.19, 13.59, 3.08}}, /* AXUW */
    {11.54, 11.54, {343, 1061, 2378}, {14.28, 13.74, 1.20}}, /* AXUW */
    {11.58, 11.58, {317, 1059, 2378}, {8.03, 7.35, 0.48}}, /* AXUW */
    {11.50, 11.50, {317, 1059, 2378}, {8.03, 7.35, 0.48}}, /* AXUW */
    {1.00, 1.00, {351, 1664, 2435}, {4.35, 2.15, 1.70}}, /* IHWW */
    {11.70, 11.70, {354, 1762, 2499}, {14.62, 10.95, 6.09}}, /* IHWW */
    {11.81, 11.81, {348, 1732, 2484}, {19.41, 14.16, 8.04}}, /* IHWW */
    {11.44, 11.44, {351, 1713, 2435}, {22.44, 17.04, 9.58}}, /* IHWW */
    {11.46, 11.46, {355, 1664, 2434}, {21.72, 17.43, 9.12}}, /* IHWW */
    {11.42, 11.42, {358, 1595, 2422}, {21.35, 16.35, 9.22}}, /* IHWW */
    {11.43, 11.43, {362, 1542, 2378}, {20.12, 16.58, 8.63}}, /* IHWW */
    {12.44, 12.44, {369, 1463, 2484}, {17.12, 13.04, 3.95}}, /* IHWW */
    {11.50, 11.50, {376, 1416, 2416}, {15.99, 12.82, 4.15}}, /* IHWW */
    {11.50, 11.50, {381, 1354, 2439}, {15.07, 12.86, 2.93}}, /* IHWW */
    {11.55, 11.55, {386, 1290, 2404}, {14.17, 11.95, 3.29}}, /* IHWW */
    {10.43, 10.43, {380, 1255, 2283}, {16.49, 15.57, 5.75}}, /* IHWW */
    {12.06, 12.06, {383, 1187, 2443}, {15.61, 14.27, 3.15}}, /* IHWW */
    {11.45, 11.45, {393, 1129, 2435}, {14.04, 13.58, 2.39}}, /* IHWW */
    {11.51, 11.51, {398, 1064, 2228}, {13.16, 13.37, 3.76}}, /* IHWW */
    {11.47, 11.47, {408, 1013, 2435}, {12.30, 13.21, 1.68}}, /* IHWW */
    {11.57, 11.57, {413, 953, 2435}, {11.25, 13.31, 1.71}}, /* IHWW */
    {11.54, 11.54, {426, 898, 2435}, {10.46, 12.38, 1.98}}, /* IHWW */
    {12.33, 12.33, {415, 844, 2448}, {8.18, 10.63, 2.46}}, /* IHWW */
    {11.49, 11.49, {430, 811, 2435}, {6.70, 9.13, 2.27}}, /* IHWW */
    {12.16, 12.16, {351, 802, 2435}, {3.25, 4.83, 1.14}}, /* IHWW */
    {11.50, 11.50, {351, 802, 2435}, {3.25, 4.83, 1.14}}, /* IHWW */
    {1.00, 1.00, {679, 1567, 2175}, {1.02, 0.71, 0.70}}, /* AYWW */
    {11.71, 11.71, {716, 1647, 2175}, {10.26, 4.78, 1.39}}, /* AYWW */
    {12.58, 12.58, {712, 1671, 2607}, {19.40, 9.73, 2.08}}, /* AYWW */
    {11.67, 11.67, {707, 1657, 2377}, {26.18, 12.68, 4.52}}, /* AYWW */
    {11.55, 11.55, {698, 1609, 2251}, {26.21, 13.45, 6.23}}, /* AYWW */
    {10.67, 10.67, {679, 1567, 2483}, {28.31, 15.13, 5.04}}, /* AYWW */
    {11.44, 11.44, {661, 1517, 2467}, {26.91, 14.71, 5.48}}, /* AYWW */
    {11.47, 11.47, {649, 1452, 2464}, {25.75, 14.36, 4.56}}, /* AYWW */
    {11.44, 11.44, {633, 1397, 2110}, {24.43, 14.07, 4.74}}, /* AYWW */
    {11.55, 11.55, {618, 1350, 2115}, {23.02, 14.13, 5.69}}, /* AYWW */
    {11.46, 11.46, {602, 1293, 2175}, {21.54, 13.92, 6.41}}, /* AYWW */
    {11.51, 11.51, {589, 1234, 2157}, {19.90, 14.05, 4.93}}, /* AYWW */
    {11.46, 11.46, {573, 1175, 2170}, {19.05, 12.90, 5.33}}, /* AYWW */
    {11.54, 11.54, {560, 1116, 2228}, {17.16, 13.35, 4.35}}, /* AYWW */
    {11.47, 11.47, {539, 1067, 2705}, {16.03, 13.33, 2.33}}, /* AYWW */
    {11.58, 11.58, {527, 1015, 2710}, {14.00, 13.41, 1.93}}, /* AYWW */
    {11.55, 11.55, {515, 952, 2620}, {12.43, 12.51, 2.33}}, /* AYWW */
    {12.22, 12.22, {490, 924, 2524}, {9.27, 10.25, 4.21}}, /* AYWW */
    {11.63, 11.63, {470, 870, 2538}, {7.84, 10.14, 3.79}}, /* AYWW */
    {11.53, 11.53, {459, 814, 2459}, {7.25, 9.89, 2.54}}, /* AYWW */
    {11.77, 11.77, {679, 791, 2175}, {4.58, 6.03, 1.26}}, /* AYWW */
    {11.50, 11.50, {679, 791, 2175}, {4.58, 6.03, 1.26}}, /* AYWW */
    {1.00, 1.00, {564, 862, 2501}, {6.51, 7.02, 0.82}}, /* OWWW */
    {11.98, 11.98, {549, 905, 2529}, {10.07, 11.94, 4.16}}, /* OWWW */
    {11.48, 11.48, {561, 891, 2482}, {10.26, 13.15, 2.52}}, /* OWWW */
    {11.46, 11.46, {542, 892, 2501}, {10.05, 12.72, 4.18}}, /* OWWW */
    {11.47, 11.47, {548, 866, 2387}, {9.95, 12.89, 2.26}}, /* OWWW */
    {11.43, 11.43, {519, 869, 2602}, {8.82, 12.02, 2.70}}, /* OWWW */
    {11.46, 11.46, {518, 844, 2556}, {9.07, 11.88, 3.30}}, /* OWWW */
    {11.46, 11.46, {520, 832, 2506}, {9.18, 11.78, 3.43}}, /* OWWW */
    {11.46, 11.46, {485, 827, 2543}, {8.68, 11.40, 2.06}}, /* OWWW */
    {11.48, 11.48, {501, 837, 2529}, {8.22, 11.09, 1.07}}, /* OWWW */
    {11.46, 11.46, {487, 824, 2595}, {8.10, 11.03, 3.34}}, /* OWWW */
    {11.46, 11.46, {476, 823, 2586}, {8.17, 11.09, 3.09}}, /* OWWW */
    {11.48, 11.48, {468, 818, 2535}, {7.15, 10.09, 2.54}}, /* OWWW */
    {11.47, 11.47, {466, 826, 2584}, {7.05, 9.64, 3.75}}, /* OWWW */
    {11.44, 11.44, {457, 811, 2496}, {7.20, 9.73, 2.52}}, /* OWWW */
    {11.43, 11.43, {452, 793, 2494}, {6.41, 8.73, 2.24}}, /* OWWW */
    {12.41, 12.41, {561, 789, 2501}, {2.56, 3.98, 1.02}}, /* OWWW */
    {11.50, 11.50, {561, 789, 2501}, {2.56, 3.98, 1.02}}, /* OWWW */
    {1.00, 1.00, {271, 1499, 2383}, {0.40, 0.24, 0.21}}, /* ZZ */
    {11.45, 11.45, {298, 1491, 2394}, {4.81, 3.49, 3.70}}, /* ZZ */
    {11.39, 11.39, {274, 1499, 2392}, {10.03, 6.86, 7.25}}, /* ZZ */
    {11.26, 11.26, {270, 1498, 2397}, {10.21, 6.59, 7.12}}, /* ZZ */
    {11.17, 11.17, {271, 1495, 2391}, {10.54, 6.61, 7.17}}, /* ZZ */
    {11.14, 11.14, {273, 1500, 2394}, {10.23, 6.74, 7.19}}, /* ZZ */
    {11.23, 11.23, {270, 1499, 2383}, {10.36, 6.60, 7.31}}, /* ZZ */
    {11.21, 11.21, {274, 1498, 2392}, {10.26, 6.74, 7.06}}, /* ZZ */
    {11.42, 11.42, {276, 1508, 2395}, {8.23, 5.19, 5.43}}, /* ZZ */
    {11.50, 11.50, {276, 1508, 2395}, {8.23, 5.19, 5.43}}, /* ZZ */
    {1.00, 1.00, {434, 1676, 2563}, {6.69, 4.59, 1.88}}, /* ZH */
    {10.94, 10.94, {441, 1673, 2540}, {8.29, 5.79, 2.75}}, /* ZH */
    {10.87, 10.87, {435, 1682, 2447}, {8.14, 5.29, 3.14}}, /* ZH */
    {11.05, 11.05, {437, 1671, 2488}, {7.92, 5.50, 2.21}}, /* ZH */
    {11.80, 11.80, {434, 1662, 2551}, {6.39, 3.57, 2.98}}, /* ZH */
    {10.94, 10.94, {439, 1677, 2500}, {6.58, 4.44, 2.37}}, /* ZH */
    {11.50, 11.50, {439, 1677, 2500}, {6.58, 4.44, 2.37}}, /* ZH */
    {1.00, 1.00, {282, 1589, 2352}, {3.90, 3.82, 1.28}}, /* DH */
    {11.36, 11.36, {294, 1581, 2464}, {6.18, 5.96, 2.72}}, /* DH */
    {11.67, 11.67, {272, 1578, 2420}, {6.64, 5.96, 2.79}}, /* DH */
    {10.94, 10.94, {269, 1586, 2397}, {6.64, 5.89, 2.13}}, /* DH */
    {11.87, 11.87, {280, 1592, 2379}, {5.99, 5.85, 1.88}}, /* DH */
    {10.96, 10.96, {281, 1590, 2372}, {6.12, 5.78, 2.37}}, /* DH */
    {10.91, 10.91, {265, 1590, 2493}, {5.74, 5.06, 1.14}}, /* DH */
    {11.12, 11.12, {246, 1598, 2331}, {2.44, 2.02, 0.76}}, /* DH */
    {11.50, 11.50, {246, 1598, 2331}, {2.44, 2.02, 0.76}}, /* DH */
    {1.00, 1.00, {624, 1406, 2392}, {1.19, 0.41, 0.17}}, /* BO */
    {11.12, 11.12, {274, 1406, 2392}, {6.89, 0.33, 0.61}}, /* BO */
    {11.37, 11.37, {271, 1406, 2392}, {7.40, 0.29, 0.72}}, /* BO */
    {11.30, 11.30, {410, 1406, 2392}, {8.53, 1.49, 1.65}}, /* BO */
    {11.87, 11.87, {624, 1406, 2392}, {20.14, 14.98, 7.74}}, /* BO */
    {11.44, 11.44, {624, 1405, 2394}, {17.24, 12.95, 6.59}}, /* BO */
    {12.09, 12.09, {622, 1400, 2395}, {8.18, 6.33, 3.09}}, /* BO */
    {11.50, 11.50, {622, 1400, 2395}, {8.18, 6.33, 3.09}}, /* BO */
    {1.00, 1.00, {440, 1486, 2485}, {0.35, 0.29, 0.43}}, /* DO */
    {11.33, 11.33, {263, 1486, 2485}, {4.58, 0.28, 0.59}}, /* DO */
    {10.16, 10.16, {272, 1486, 2485}, {7.37, 0.36, 0.55}}, /* DO */
    {11.47, 11.47, {274, 1486, 2485}, {7.52, 0.38, 0.72}}, /* DO */
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
    {11.47, 11.47, {282, 1718, 2551}, {2.94, 0.62, 0.37}}, /* GO */
    {11.50, 11.50, {282, 1718, 2551}, {2.94, 0.62, 0.37}}, /* GO */
    {1.00, 1.00, {264, 2117, 2480}, {8.05, 5.10, 1.17}}, /* EG */
    {11.45, 11.45, {258, 2124, 2467}, {5.51, 3.38, 0.63}}, /* EG */
    {11.50, 11.50, {258, 2124, 2467}, {5.51, 3.38, 0.63}}, /* EG */
    {1.00, 1.00, {279, 1314, 2503}, {1.06, 0.51, 0.38}}, /* OG */
    {10.62, 10.62, {279, 1314, 2423}, {7.49, 6.23, 0.95}}, /* OG */
    {11.44, 11.44, {262, 1319, 2480}, {5.45, 3.96, 0.82}}, /* OG */
    {11.50, 11.50, {262, 1319, 2480}, {5.45, 3.96, 0.82}}, /* OG */
};

static const struct {
  short first, count;
} vf_index[] = {
    {0, 0},
    {0, 11},
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
    {11, 9},
    {0, 0},
    {0, 0},
    {0, 0},
    {20, 9},
    {0, 0},
    {0, 0},
    {29, 21},
    {0, 0},
    {50, 21},
    {71, 21},
    {92, 21},
    {0, 0},
    {0, 0},
    {113, 24},
    {0, 0},
    {137, 20},
    {157, 17},
    {174, 19},
    {193, 19},
    {212, 22},
    {234, 22},
    {256, 18},
    {0, 0},
    {0, 0},
    {274, 10},
    {284, 7},
    {291, 9},
    {0, 0},
    {300, 8},
    {0, 0},
    {0, 0},
    {0, 0},
    {308, 8},
    {316, 4},
    {320, 4},
    {324, 5},
    {329, 5},
    {334, 3},
    {337, 4},
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
/* VF-END */

/* the hiss of ZZ and ZH. their frames are the voicing alone, measured where
 * nothing above 4kHz was compared, but both units have hiss at 3.5 to 4.5kHz
 * 20 to 24dB under the voicing's loudest band, 7 to 8dB more than the voicing
 * gives (ZZ and ZH at pitch 40 on the fourth VoxProbe recording, and ZZ in Man
 * Goes Down). here oscillators 4 and 5 give it, half enveloped by the voicing
 * and following each frame's level; their frequencies, width and level are
 * fitted to the fourth VoxProbe recording, where ZZ goes from 3.8 to 1.9dB
 * and ZH 5.2 to 2.4dB (full band). left out, the VoxRecord recording's ZH goes
 * from 3.9 to 1.8dB, and Man Goes Down's ZZ is within 1dB at 3.5 and 4.5kHz */
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
    s->length = 1.0;
    for (int i = 0; i < vf_index[code - 128].count; i++)
      add_part(s, p[i].ms, p[i].glide,
               voiced_frame(FM(p[i].freq[0], p[i].freq[1], p[i].freq[2]), p[i].vol[0], p[i].vol[1], p[i].vol[2]));
    for (int i = 0; i < s->nparts; i++)
      s->parts[i].target.onset_ms = onset_ms(code);
    framed_noise(s, code);
    return;
  }
  switch (code) {
  /* the vowels' and liquids' three volumes are the chip's: at pitch 40 on the
   * third VoxProbe recording each voicing pulse is the three formant sines
   * alone, and fitting them through the chip's fade and output filter (see
   * voice_fade) gives each one's amplitude, the mean of both units. some of
   * these sounds are instead played from the chip's own frames, pulse by
   * pulse (see vf_points) */
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
  /* NGE and NGO had no measurement (VoxProbe 3 doesn't play them), but on
   * the VoxRecord recording both units agree on every harmonic of them to
   * within 0.5dB RMS. their frequencies, volumes and phases are fitted to
   * those harmonics, played here: NGE 87.0% to 97.0% and 3.93 to 1.11dB,
   * NGO 93.1% to 96.2% and 2.98 to 1.83dB. on the chip NGE's second and
   * third formants are one peak near 2.3kHz, where they had been 2.1 and
   * 2.7kHz */
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
    /* on both units (the VoxRecord recording) TS is hiss that starts at its
     * loudest, peaked at 3.5kHz, and dies away: 6dB down 40ms on, 14dB by
     * 120ms, spreading out to a flat spectrum with a little left at 3.5kHz.
     * the fit as a stop had it the other way round, a long broad burst into a
     * short hiss at the end. the hiss here is that fit's, then the same
     * oscillators spread wide (distortion) and moved up, gliding down to 20dB
     * under it. against both units this matches 37% (13% before) where they
     * match each other 35%, its spectrum 3.7dB off (3.5 before, 3.5 between
     * the two) */
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
  for (int i = 0; i < s->nparts; i++) {
    for (int j = 0; j < 5; j++)
      s->parts[i].target.vol[j] *= g;
    s->parts[i].target.onset_ms = onset_ms(code);
  }
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
/* a DTMF tone, its two frequencies in FREQ_STEP_HZ steps: the chip's,
 * measured on the second VoxProbe recording, which are a few hertz from the
 * standard ones (1477Hz is 1468.75) */
static void dtmf(sound *s, int low, int high) {
  const frame t = fade(tone(low * FREQ_STEP_HZ, high * FREQ_STEP_HZ), 2.4);
  hold(s, 150, 3, t);
  hold(s, 3, 3, quiet(t));
}

/* the manual describes the sound effects only by name and length. the others
 * here are traced from the third take of the VoxRecord recording (see
 * msa_speed_factor()), which plays each on its own and clips little: every
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
 * the output filter's loss at each frequency added back (see CHIP_RATE), as
 * the filter here takes it off again. many effects are nothing like their
 * names suggest, and many differ in length from the manual: A1 and A2 last
 * about a second, and M0 is a 1.2 second tone around 1.8kHz that dies away.
 * the points at the start and end of an effect that were no louder than the
 * recording's background (within 6dB of the first point) are silent: the
 * trace had taken that background for part of the effect, making each one
 * 20 to 50ms too long. the tone effects are traced again in fx2_points.
 *
 * A1 is traced again: its trace had taken the strongest peaks, which jump
 * between its parts. on both units of the VoxRecord and fourth VoxProbe
 * recordings, read from the peaks of its harmonics every 25ms (within 10Hz
 * of each other), it is three formants: one steady at 207Hz, one rising from
 * 720 to 1645Hz and one from about 120 to 2870Hz, faster, crossing the other
 * near 300ms. the same at pitch 60 and 150 and at speeds 90 and 127, and at
 * bend 15 all three 440Hz higher. the levels were fitted to the harmonics'
 * peaks at the default pitch: on the rest, left out (both units of the fourth
 * VoxProbe recording at pitch 60 and 150, bend 15 and speeds 90 and 127), 30
 * to 93% and 7.4 to 1.3dB, and on the first VoxProbe recording, which nothing
 * here was fitted to, 36 to 91% and 7.3 to 1.1dB. at bend 0 the chip plays
 * something else: the second formant moves 197Hz down as it should, but the
 * third is mirrored, falling as 2.5kHz less its rise, and a line holds near
 * 2.4kHz. both are a frequency Bend took under nothing (see msa_expand()):
 * the third formant starts at 120Hz, and the steady line is the first, which
 * read 207Hz off the harmonics but goes under at bend 0 too, so it is put
 * at 175Hz, the lowest that does: every A1 item 91.0 to 94.5% and 1.63 to
 * 1.33dB, bend 0 59 to 82% and 3.8 to 2.3dB */
enum { FX_TONE, FX_NOISE, FX_BUZZ };

typedef struct fx_point {
  short ms, kind, pitch;
  double db;
  short freq[3];
  double rel[3];
} fx_point;

static const fx_point fx_points[] = {
    /* R0 */
    {5, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {20, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
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
    {115, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {120, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {125, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
    {135, 0, 0, -60.0, {799, 511, 0}, {0, -15.1, -60.0}},
    /* R1 */
    {5, 0, 0, -60.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    {20, 0, 0, -60.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    {25, 0, 0, -14.8, {1100, 731, 0}, {0, -19.3, -60.0}},
    {30, 0, 0, -5.2, {1100, 731, 0}, {0, -19.3, -60.0}},
    {35, 0, 0, -1.6, {1100, 731, 0}, {0, -19.3, -60.0}},
    {80, 0, 0, 1.9, {1100, 731, 0}, {0, -19.3, -60.0}},
    {100, 0, 0, -4.4, {1100, 731, 0}, {0, -19.3, -60.0}},
    {105, 0, 0, -10.8, {1100, 731, 0}, {0, -19.3, -60.0}},
    {110, 0, 0, -20.4, {1100, 731, 0}, {0, -19.3, -60.0}},
    {115, 0, 0, -60.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    {130, 0, 0, -60.0, {1100, 731, 0}, {0, -19.3, -60.0}},
    /* R2 */
    {5, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    {20, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    {25, 0, 0, -13.7, {1299, 531, 0}, {0, -21.0, -60.0}},
    {30, 0, 0, -5.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {35, 0, 0, -2.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {60, 0, 0, 0.1, {1299, 531, 0}, {0, -21.0, -60.0}},
    {65, 0, 0, -0.2, {1299, 531, 0}, {0, -21.0, -60.0}},
    {95, 0, 0, -2.9, {1299, 531, 0}, {0, -21.0, -60.0}},
    {100, 0, 0, -5.5, {1299, 531, 0}, {0, -21.0, -60.0}},
    {105, 0, 0, -11.7, {1299, 531, 0}, {0, -21.0, -60.0}},
    {110, 0, 0, -16.4, {1299, 531, 0}, {0, -21.0, -60.0}},
    {115, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    {120, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    {125, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    {135, 0, 0, -60.0, {1299, 531, 0}, {0, -21.0, -60.0}},
    /* R3 */
    {5, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {10, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {15, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {20, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
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
    {120, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    {130, 0, 0, -60.0, {1599, 726, 360}, {0, -14.1, -15.2}},
    /* R4 */
    {5, 0, 0, -60.0, {1898, 305, 561}, {0, -18.5, -23.3}},
    {20, 0, 0, -60.0, {1898, 305, 561}, {0, -18.5, -23.3}},
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
    {120, 0, 0, -60.0, {1898, 305, 561}, {0, -18.5, -23.3}},
    {130, 0, 0, -60.0, {1898, 305, 561}, {0, -18.5, -23.3}},
    /* R5 */
    {5, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {10, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {15, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {20, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {25, 0, 0, -13.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {30, 0, 0, -7.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {45, 0, 0, -2.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {90, 0, 0, -3.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {95, 0, 0, -5.2, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {100, 0, 0, -12.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {105, 0, 0, -16.5, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {110, 0, 0, -22.1, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {115, 0, 0, -27.2, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {120, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {125, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    {130, 0, 0, -60.0, {2198, 381, 2667}, {0, -14.3, -18.3}},
    /* R6 */
    {5, 0, 0, -60.0, {2498, 314, 546}, {0, -19.0, -22.2}},
    {10, 0, 0, -60.0, {2498, 314, 546}, {0, -19.0, -22.2}},
    {15, 0, 0, -60.0, {2498, 314, 546}, {0, -19.0, -22.2}},
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
    {120, 0, 0, -60.0, {2498, 314, 546}, {0, -19.0, -22.2}},
    {125, 0, 0, -60.0, {2498, 314, 546}, {0, -19.0, -22.2}},
    /* R7 */
    {5, 0, 0, -60.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {10, 0, 0, -60.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {15, 0, 0, -60.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
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
    {120, 0, 0, -60.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    {125, 0, 0, -60.0, {2797, 253, 1055}, {0, -17.8, -24.6}},
    /* R8 */
    {5, 0, 0, -60.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {15, 0, 0, -60.0, {643, 3097, 321}, {0, 10.7, -16.6}},
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
    {120, 0, 0, -60.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {125, 0, 0, -60.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    {130, 0, 0, -60.0, {643, 3097, 321}, {0, 10.7, -16.6}},
    /* R9 */
    {5, 0, 0, -60.0, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {15, 0, 0, -60.0, {640, 3397, 1592}, {0, 10.1, -14.2}},
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
    {120, 0, 0, -60.0, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {125, 0, 0, -60.0, {640, 3397, 1592}, {0, 10.1, -14.2}},
    {130, 0, 0, -60.0, {640, 3397, 1592}, {0, 10.1, -14.2}},
    /* A0 */
    {5, 0, 0, -60.0, {239, 1009, 472}, {0, -60.0, -60.0}},
    {20, 0, 0, -60.0, {239, 1009, 472}, {0, -60.0, -60.0}},
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
    {375, 0, 0, -60.0, {367, 636, 157}, {0, -12.5, -60.0}},
    {380, 0, 0, -60.0, {362, 991, 157}, {0, -13.3, -60.0}},
    {385, 0, 0, -60.0, {245, 991, 157}, {0, -13.7, -60.0}},
    {395, 0, 0, -60.0, {239, 991, 157}, {0, -13.0, -60.0}},
    /* A1 */
    {15, 2, 87, -60.0, {175, 720, 120}, {0, 15.0, -3.0}},
    {35, 2, 87, -16.1, {175, 720, 120}, {0, 15.0, -3.0}},
    {60, 2, 87, 4.7, {175, 750, 172}, {0, -5.3, -3.0}},
    {85, 2, 87, 18.2, {175, 780, 224}, {0, -16.7, -3.0}},
    {110, 2, 87, 13.5, {175, 809, 293}, {0, -9.0, 0.7}},
    {135, 2, 87, 7.9, {175, 838, 362}, {0, -9.1, 0.4}},
    {160, 2, 87, 7.9, {175, 868, 428}, {0, -4.3, 3.6}},
    {185, 2, 87, 5.7, {175, 898, 495}, {0, -2.9, -1.0}},
    {210, 2, 87, 7.6, {175, 929, 567}, {0, -10.8, -6.4}},
    {235, 2, 87, 6.7, {175, 960, 640}, {0, -1.2, -6.9}},
    {260, 2, 87, 8.4, {175, 992, 709}, {0, -11.9, -3.9}},
    {285, 2, 87, 8.5, {175, 1024, 778}, {0, -3.0, -3.0}},
    {310, 2, 87, 7.4, {175, 1048, 861}, {0, -3.0, -3.0}},
    {335, 2, 87, 7.7, {175, 1072, 945}, {0, -3.0, -3.0}},
    {360, 2, 87, 7.3, {175, 1096, 1028}, {0, -3.0, -3.0}},
    {385, 2, 87, 6.6, {175, 1120, 1111}, {0, -3.0, -3.0}},
    {410, 2, 87, 7.6, {175, 1144, 1194}, {0, -3.0, -3.0}},
    {435, 2, 87, 6.9, {175, 1168, 1278}, {0, -3.0, -3.0}},
    {460, 2, 87, 7.1, {175, 1191, 1361}, {0, -3.0, -3.0}},
    {485, 2, 87, 2.9, {175, 1215, 1445}, {0, -3.0, -3.0}},
    {510, 2, 87, 2.3, {175, 1239, 1525}, {0, -3.4, -7.7}},
    {535, 2, 87, 1.5, {175, 1263, 1605}, {0, -2.6, -1.4}},
    {560, 2, 87, 2.7, {175, 1295, 1689}, {0, -7.5, -6.1}},
    {585, 2, 87, 3.9, {175, 1327, 1773}, {0, -3.5, -7.6}},
    {610, 2, 87, 2.0, {175, 1357, 1858}, {0, -3.6, -0.3}},
    {635, 2, 87, 4.8, {175, 1388, 1943}, {0, -9.4, -11.3}},
    {660, 2, 87, 2.6, {175, 1418, 2027}, {0, -1.1, -1.2}},
    {685, 2, 87, 1.7, {175, 1449, 2112}, {0, -5.7, -4.7}},
    {710, 2, 87, 3.3, {175, 1473, 2192}, {0, -5.5, -8.1}},
    {735, 2, 87, 2.6, {175, 1498, 2272}, {0, -5.5, -1.1}},
    {760, 2, 87, 5.4, {175, 1516, 2347}, {0, -7.2, -12.6}},
    {785, 2, 87, 2.8, {175, 1535, 2422}, {0, -3.9, 2.8}},
    {810, 2, 87, 3.5, {175, 1554, 2497}, {0, -6.5, -9.0}},
    {835, 2, 87, 2.8, {175, 1573, 2573}, {0, -4.2, -2.2}},
    {860, 2, 87, 2.1, {175, 1591, 2648}, {0, -5.8, -2.7}},
    {885, 2, 87, 5.2, {175, 1610, 2723}, {0, -6.8, -12.4}},
    {910, 2, 87, 7.9, {175, 1627, 2797}, {0, -10.4, -7.8}},
    {975, 2, 87, 6.0, {175, 1668, 2984}, {0, -3.0, -3.0}},
    /* A2 */
    {5, 2, 128, -60.0, {165, 1996, 1996}, {0, -0.0, -35.8}},
    {10, 2, 127, -60.0, {165, 1996, 1996}, {0, -0.1, -36.0}},
    {15, 2, 153, -60.0, {165, 1996, 1996}, {0, 0.3, -35.9}},
    {20, 2, 183, -60.0, {165, 1698, 1455}, {0, 36.6, 28.5}},
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
    {990, 2, 181, -60.0, {162, 250, 731}, {0, -0.0, 50.7}},
    {995, 2, 71, -60.0, {160, 250, 731}, {0, -39.9, -12.7}},
    {1000, 2, 127, -60.0, {149, 250, 731}, {0, -39.7, -10.4}},
    /* A3. its buzz was traced at 470 to 490Hz, but on both units of the
     * fourth VoxProbe recording it repeats at the voicing frequency (60.2,
     * 87.1 and 150Hz at pitch 60, 88 and 150), so its buzz is at the default
     * voicing pitch here and follows Pitch: its items match both units
     * better (79 to 80%, 80 to 82%) */
    {5, 2, 88, -60.0, {576, 977, 1996}, {0, 27.2, 36.5}},
    {10, 2, 88, -60.0, {576, 980, 2002}, {0, 27.3, 37.5}},
    {15, 2, 88, -60.0, {576, 990, 2010}, {0, 26.3, 38.0}},
    {20, 2, 88, -60.0, {576, 990, 2010}, {0, 26.3, 38.0}},
    {25, 2, 88, -6.5, {577, 990, 2386}, {0, -35.0, -12.1}},
    {30, 2, 88, -5.5, {577, 978, 2386}, {0, -1.6, -13.1}},
    {35, 2, 88, -3.9, {577, 978, 2386}, {0, -1.0, -13.9}},
    {80, 2, 88, -4.6, {578, 1065, 2476}, {0, 1.0, -14.0}},
    {120, 2, 88, -3.5, {626, 1164, 2491}, {0, 0.2, -15.2}},
    {125, 2, 88, -5.5, {632, 1177, 1852}, {0, 0.1, 3.6}},
    {135, 2, 88, -4.9, {645, 1199, 1852}, {0, -0.1, 2.7}},
    {365, 2, 88, -9.1, {1121, 1915, 2760}, {0, 3.0, 6.1}},
    {520, 2, 88, -8.7, {1452, 2357, 3417}, {0, 4.1, 6.0}},
    {525, 2, 88, -10.9, {1462, 2370, 3440}, {0, 4.3, 6.1}},
    {545, 2, 88, -14.2, {1465, 2382, 3447}, {0, 5.2, 6.6}},
    {550, 2, 88, -26.0, {1459, 2382, 3431}, {0, 4.0, 6.5}},
    {555, 2, 88, -60.0, {1304, 2379, 3423}, {0, -21.7, 6.5}},
    {560, 2, 88, -60.0, {1283, 2184, 3423}, {0, 1.5, -11.8}},
    {565, 2, 88, -60.0, {1067, 1987, 2980}, {0, 7.8, 15.7}},
    {570, 2, 88, -60.0, {1011, 1986, 2975}, {0, 9.0, 16.6}},
    /* A4 */
    {5, 0, 0, -60.0, {239, 467, 420}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 467, 420}, {0, -13.9, -60.0}},
    {15, 0, 0, -60.0, {239, 991, 420}, {0, -0.1, -60.0}},
    {20, 0, 0, -60.0, {239, 467, 420}, {0, -15.0, -60.0}},
    {25, 0, 0, -60.0, {239, 467, 420}, {0, -13.9, -60.0}},
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
    {275, 0, 0, -60.0, {490, 3901, 706}, {0, 16.0, -1.9}},
    {280, 0, 0, -60.0, {379, 3901, 647}, {0, 11.0, -8.3}},
    {285, 0, 0, -60.0, {239, 3901, 420}, {0, 7.8, -11.7}},
    {290, 0, 0, -60.0, {239, 997, 420}, {0, -14.8, -60.0}},
    {295, 0, 0, -60.0, {239, 991, 420}, {0, -14.5, -60.0}},
    /* A5 */
    {5, 0, 0, -60.0, {239, 484, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 484, 0}, {0, -60.0, 0.0}},
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
    {575, 0, 0, -60.0, {577, 490, 0}, {0, -60.0, 0.0}},
    {580, 0, 0, -60.0, {245, 490, 0}, {0, -60.0, 0.0}},
    {585, 0, 0, -60.0, {239, 490, 0}, {0, -60.0, 0.0}},
    /* A6 */
    {5, 0, 0, -60.0, {239, 991, 0}, {0, -60.0, 0.0}},
    {10, 0, 0, -60.0, {239, 991, 0}, {0, -13.1, 0.0}},
    {20, 0, 0, -60.0, {239, 991, 0}, {0, -14.7, 0.0}},
    {25, 0, 0, -60.0, {239, 700, 0}, {0, -11.5, 0.0}},
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
    {215, 0, 0, -60.0, {799, 2000, 0}, {0, -60.0, 0.0}},
    {220, 0, 0, -60.0, {245, 2000, 0}, {0, -60.0, 0.0}},
    {230, 0, 0, -60.0, {227, 2000, 0}, {0, -60.0, 0.0}},
    /* A7 */
    {5, 0, 0, -60.0, {239, 373, 770}, {0, -60.0, -60.0}},
    {20, 0, 0, -60.0, {239, 373, 770}, {0, -60.0, -60.0}},
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
    {600, 0, 0, -60.0, {1860, 927, 1528}, {0, -60.0, -60.0}},
    {605, 0, 0, -60.0, {1249, 927, 1528}, {0, -60.0, -60.0}},
    {610, 0, 0, -60.0, {1090, 927, 1528}, {0, -60.0, -60.0}},
    {615, 0, 0, -60.0, {245, 927, 1528}, {0, -60.0, -60.0}},
    {625, 0, 0, -60.0, {239, 927, 1528}, {0, -60.0, -60.0}},
    /* A8 */
    {5, 0, 0, -60.0, {239, 997, 0}, {0, -13.8, 0.0}},
    {20, 0, 0, -60.0, {245, 997, 0}, {0, -13.6, 0.0}},
    {25, 0, 0, -60.0, {245, 455, 0}, {0, -5.4, 0.0}},
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
    {295, 0, 0, -60.0, {490, 863, 0}, {0, -60.0, 0.0}},
    {300, 0, 0, -60.0, {251, 863, 0}, {0, -60.0, 0.0}},
    {310, 0, 0, -60.0, {239, 863, 0}, {0, -60.0, 0.0}},
    /* A9 */
    {5, 0, 0, -60.0, {233, 2443, 828}, {0, -60.0, -60.0}},
    {15, 0, 0, -60.0, {239, 2443, 828}, {0, -60.0, -60.0}},
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
    {335, 0, 0, -60.0, {257, 490, 863}, {0, -3.3, -5.5}},
    {340, 0, 0, -60.0, {239, 490, 863}, {0, -10.0, -11.6}},
    {345, 0, 0, -60.0, {239, 478, 682}, {0, -13.2, -0.1}},
    {350, 0, 0, -60.0, {233, 478, 682}, {0, -14.8, -60.0}},
    /* B0 */
    {5, 0, 0, -60.0, {239, 227, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 227, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -20.1, {245, 227, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -7.3, {2583, 227, 0}, {0, -60.0, 0.0}},
    {40, 0, 0, -2.7, {2595, 227, 0}, {0, -60.0, 0.0}},
    {135, 0, 0, -2.9, {2595, 227, 0}, {0, -60.0, 0.0}},
    {185, 0, 0, -15.6, {2595, 227, 0}, {0, -60.0, 0.0}},
    {190, 0, 0, -18.3, {2595, 227, 0}, {0, -13.9, 0.0}},
    {195, 0, 0, -20.8, {2595, 455, 0}, {0, -11.8, 0.0}},
    {200, 0, 0, -60.0, {245, 461, 0}, {0, -11.9, 0.0}},
    {210, 0, 0, -60.0, {233, 455, 0}, {0, -12.9, 0.0}},
    /* B1 */
    {5, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    {20, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    {25, 0, 0, -6.0, {1376, 0, 0}, {0, 0.0, 0.0}},
    {40, 0, 0, 0.7, {1516, 0, 0}, {0, 0.0, 0.0}},
    {95, 0, 0, -1.1, {2473, 0, 0}, {0, 0.0, 0.0}},
    {170, 0, 0, -2.5, {3767, 0, 0}, {0, 0.0, 0.0}},
    {185, 0, 0, -2.4, {3709, 0, 0}, {0, 0.0, 0.0}},
    {230, 0, 0, -1.4, {2752, 0, 0}, {0, 0.0, 0.0}},
    {260, 0, 0, -0.8, {2111, 0, 0}, {0, 0.0, 0.0}},
    {295, 0, 0, 1.5, {1411, 0, 0}, {0, 0.0, 0.0}},
    {375, 0, 0, -7.6, {1400, 0, 0}, {0, 0.0, 0.0}},
    {410, 0, 0, -60.0, {1400, 0, 0}, {0, 0.0, 0.0}},
    {415, 0, 0, -60.0, {257, 0, 0}, {0, 0.0, 0.0}},
    {430, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    /* B2 */
    {5, 0, 0, -60.0, {239, 198, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 198, 0}, {0, -60.0, 0.0}},
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
    {415, 0, 0, -60.0, {1703, 472, 0}, {0, -14.9, 0.0}},
    {420, 0, 0, -60.0, {1703, 478, 0}, {0, -3.3, 0.0}},
    {425, 0, 0, -60.0, {356, 478, 0}, {0, -10.6, 0.0}},
    {430, 0, 0, -60.0, {239, 478, 0}, {0, -4.3, 0.0}},
    {435, 0, 0, -60.0, {233, 478, 0}, {0, -60.0, 0.0}},
    /* B3 */
    {5, 0, 0, -60.0, {239, 490, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 490, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -20.8, {239, 490, 0}, {0, -60.0, 0.0}},
    {25, 0, 0, -13.8, {595, 490, 0}, {0, 0.0, 0.0}},
    {30, 0, 0, -11.3, {595, 3096, 0}, {0, 9.4, 0.0}},
    {130, 0, 0, -7.1, {595, 3096, 0}, {0, 8.2, 0.0}},
    {135, 0, 0, -4.4, {595, 1423, 0}, {0, -5.2, 0.0}},
    {140, 0, 0, -2.4, {1394, 601, 0}, {0, -5.9, 0.0}},
    {255, 0, 0, -0.9, {1394, 601, 0}, {0, -9.9, 0.0}},
    {310, 0, 0, -12.2, {1400, 601, 0}, {0, -11.6, 0.0}},
    {325, 0, 0, -21.2, {1394, 589, 0}, {0, -15.1, 0.0}},
    {330, 0, 0, -60.0, {1394, 589, 0}, {0, -6.9, 0.0}},
    {335, 0, 0, -60.0, {478, 991, 0}, {0, -4.5, 0.0}},
    {340, 0, 0, -60.0, {245, 991, 0}, {0, -12.9, 0.0}},
    {345, 0, 0, -60.0, {239, 991, 0}, {0, -13.8, 0.0}},
    /* B4 */
    {5, 0, 0, -60.0, {239, 1551, 0}, {0, -60.0, 0.0}},
    {20, 0, 0, -60.0, {239, 1551, 0}, {0, -60.0, 0.0}},
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
    {400, 0, 0, -60.0, {1825, 991, 0}, {0, -2.9, 0.0}},
    {405, 0, 0, -60.0, {239, 991, 0}, {0, -14.2, 0.0}},
    {415, 0, 0, -60.0, {239, 991, 0}, {0, -13.8, 0.0}},
    /* B5 */
    {5, 0, 0, -60.0, {233, 490, 204}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 490, 204}, {0, -60.0, -60.0}},
    {15, 0, 0, -60.0, {239, 490, 204}, {0, -0.0, -60.0}},
    {20, 0, 0, -60.0, {239, 490, 204}, {0, -0.0, -60.0}},
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
    {195, 0, 0, -60.0, {239, 986, 204}, {0, -0.1, -60.0}},
    {200, 0, 0, -60.0, {239, 531, 204}, {0, -13.0, -60.0}},
    {205, 0, 0, -60.0, {233, 531, 204}, {0, -60.0, -60.0}},
    /* B6 */
    {5, 0, 0, -60.0, {239, 367, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 367, 0}, {0, -60.0, 0.0}},
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
    {190, 0, 0, -60.0, {251, 467, 0}, {0, -2.1, 0.0}},
    {195, 0, 0, -60.0, {239, 991, 0}, {0, -13.9, 0.0}},
    {200, 0, 0, -60.0, {239, 991, 0}, {0, -14.1, 0.0}},
    {205, 0, 0, -60.0, {239, 467, 0}, {0, -14.9, 0.0}},
    /* B7 */
    {5, 0, 0, -60.0, {239, 268, 0}, {0, -60.0, 0.0}},
    {15, 0, 0, -60.0, {239, 268, 0}, {0, -60.0, 0.0}},
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
    {180, 0, 0, -60.0, {391, 997, 0}, {0, -0.4, 0.0}},
    {185, 0, 0, -60.0, {373, 997, 0}, {0, -4.3, 0.0}},
    {190, 0, 0, -60.0, {274, 1003, 0}, {0, -10.1, 0.0}},
    {195, 0, 0, -60.0, {239, 997, 0}, {0, -0.1, 0.0}},
    {200, 0, 0, -60.0, {239, 997, 0}, {0, -60.0, 0.0}},
    /* B8 */
    {5, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    {15, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    {20, 0, 0, -17.3, {245, 0, 0}, {0, 0.0, 0.0}},
    {25, 0, 0, -9.5, {2053, 0, 0}, {0, 0.0, 0.0}},
    {35, 0, 0, -5.9, {2053, 0, 0}, {0, 0.0, 0.0}},
    {150, 0, 0, -4.2, {1481, 0, 0}, {0, 0.0, 0.0}},
    {220, 0, 0, -3.7, {1324, 0, 0}, {0, 0.0, 0.0}},
    {290, 0, 0, -5.5, {1300, 0, 0}, {0, 0.0, 0.0}},
    {320, 0, 0, -13.1, {1300, 0, 0}, {0, 0.0, 0.0}},
    {340, 0, 0, -60.0, {1271, 0, 0}, {0, 0.0, 0.0}},
    {345, 0, 0, -60.0, {239, 0, 0}, {0, 0.0, 0.0}},
    {355, 0, 0, -60.0, {227, 0, 0}, {0, 0.0, 0.0}},
    /* B9 */
    {5, 0, 0, -60.0, {233, 898, 2700}, {0, -60.0, -60.0}},
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
    {320, 0, 0, -60.0, {245, 467, 881}, {0, -13.3, -60.0}},
    {325, 0, 0, -60.0, {239, 467, 881}, {0, -60.0, -60.0}},
    {330, 0, 0, -60.0, {239, 467, 881}, {0, -60.0, -60.0}},
    /* C0 */
    {5, 0, 0, -60.0, {239, 991, 1545}, {0, -14.9, -60.0}},
    {15, 0, 0, -60.0, {239, 991, 1545}, {0, -14.9, -60.0}},
    {20, 0, 0, -60.0, {239, 991, 1545}, {0, -0.1, -60.0}},
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
    {225, 0, 0, -60.0, {268, 490, 881}, {0, -10.7, -60.0}},
    {240, 0, 0, -60.0, {239, 461, 881}, {0, -0.0, -60.0}},
    {245, 0, 0, -60.0, {239, 461, 881}, {0, -60.0, -60.0}},
    /* C1 */
    {5, 0, 0, -60.0, {239, 449, 426}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 449, 426}, {0, -13.5, -60.0}},
    {15, 0, 0, -60.0, {245, 449, 426}, {0, -14.1, -60.0}},
    {20, 0, 0, -60.0, {653, 478, 426}, {0, -1.8, -14.5}},
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
    {305, 0, 0, -60.0, {519, 222, 490}, {0, 0.0, 0.0}},
    {310, 0, 0, -60.0, {490, 280, 799}, {0, -3.2, -4.5}},
    {315, 0, 0, -60.0, {245, 472, 886}, {0, -2.0, -4.9}},
    {325, 0, 0, -60.0, {239, 472, 799}, {0, -11.2, -12.6}},
    {330, 0, 0, -60.0, {227, 472, 799}, {0, -14.7, -60.0}},
    /* C2 */
    {5, 0, 0, -60.0, {233, 216, 974}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 216, 974}, {0, -60.0, -60.0}},
    {15, 0, 0, -60.0, {239, 216, 974}, {0, 0.0, -60.0}},
    {25, 0, 0, -60.0, {455, 216, 974}, {0, -2.1, -60.0}},
    {30, 0, 0, -60.0, {478, 910, 974}, {0, -5.1, -0.1}},
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
    {240, 0, 0, -60.0, {245, 472, 484}, {0, -14.6, -0.0}},
    {245, 0, 0, -60.0, {239, 472, 484}, {0, -12.0, -60.0}},
    {250, 0, 0, -60.0, {233, 472, 484}, {0, -60.0, -60.0}},
    /* C3 */
    {5, 0, 0, -60.0, {222, 490, 414}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 490, 414}, {0, -12.0, -60.0}},
    {15, 0, 0, -60.0, {239, 490, 414}, {0, -13.2, -60.0}},
    {20, 0, 0, -60.0, {268, 490, 414}, {0, -7.4, -60.0}},
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
    {240, 0, 0, -60.0, {239, 997, 571}, {0, -13.4, -60.0}},
    {250, 0, 0, -60.0, {239, 991, 571}, {0, -13.5, -60.0}},
    /* C4 */
    {5, 0, 0, -60.0, {239, 991, 0}, {0, -14.7, 0.0}},
    {20, 0, 0, -60.0, {239, 991, 0}, {0, -14.9, 0.0}},
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
    {245, 0, 0, -60.0, {239, 997, 0}, {0, -12.9, 0.0}},
    {255, 0, 0, -60.0, {239, 997, 0}, {0, -15.0, 0.0}},
    {265, 0, 0, -60.0, {239, 478, 0}, {0, -12.0, 0.0}},
    /* C5 */
    {5, 0, 0, -60.0, {239, 467, 297}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 467, 297}, {0, -0.0, -60.0}},
    {15, 0, 0, -60.0, {239, 665, 297}, {0, -14.5, -60.0}},
    {20, 0, 0, -60.0, {239, 665, 297}, {0, -2.1, -60.0}},
    {25, 0, 0, -60.0, {309, 764, 297}, {0, -10.9, -60.0}},
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
    {360, 0, 0, -60.0, {187, 612, 892}, {0, -11.8, -60.0}},
    {365, 0, 0, -60.0, {198, 612, 892}, {0, -9.6, -60.0}},
    {370, 0, 0, -60.0, {239, 991, 892}, {0, -5.0, -60.0}},
    {375, 0, 0, -60.0, {239, 991, 892}, {0, -14.5, -60.0}},
    {380, 0, 0, -60.0, {239, 991, 892}, {0, -13.8, -60.0}},
    /* C6 */
    {5, 0, 0, -60.0, {233, 478, 583}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {233, 478, 583}, {0, -0.0, -60.0}},
    {15, 0, 0, -60.0, {239, 980, 583}, {0, -13.4, -0.1}},
    {20, 0, 0, -60.0, {239, 980, 583}, {0, -11.9, -12.9}},
    {25, 0, 0, -60.0, {2012, 991, 595}, {0, -14.7, -15.1}},
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
    {285, 0, 0, -60.0, {239, 571, 531}, {0, -8.0, -10.3}},
    {295, 0, 0, -60.0, {239, 764, 566}, {0, -14.6, -0.1}},
    {300, 0, 0, -60.0, {239, 764, 566}, {0, -10.4, -60.0}},
    {305, 0, 0, -60.0, {239, 478, 566}, {0, -13.0, -60.0}},
    /* C7 */
    {5, 0, 0, -60.0, {239, 472, 332}, {0, -60.0, -60.0}},
    {10, 0, 0, -60.0, {239, 472, 332}, {0, -12.2, -60.0}},
    {20, 0, 0, -60.0, {245, 472, 332}, {0, -12.2, -60.0}},
    {25, 0, 0, -60.0, {490, 262, 332}, {0, -4.9, -60.0}},
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
    {405, 0, 0, -60.0, {484, 227, 157}, {0, -4.2, -60.0}},
    {410, 0, 0, -60.0, {239, 227, 157}, {0, -13.8, -60.0}},
    {420, 0, 0, -60.0, {239, 227, 157}, {0, -60.0, -60.0}},
    /* M0 */
    {5, 0, 0, -60.0, {239, 986, 1627}, {0, -13.9, -60.0}},
    {15, 0, 0, -60.0, {239, 991, 1627}, {0, -14.8, -60.0}},
    {25, 0, 0, -2.4, {1755, 1609, 1627}, {0, -11.4, -60.0}},
    {35, 0, 0, 1.1, {1790, 1609, 1627}, {0, -12.9, -60.0}},
    {60, 0, 0, 0.8, {1814, 1574, 1627}, {0, -60.0, -60.0}},
    {180, 0, 0, -2.1, {1819, 1598, 1627}, {0, -60.0, -60.0}},
    {185, 0, 0, -2.1, {1819, 1598, 1627}, {0, -1.0, -60.0}},
    {210, 0, 0, -2.9, {1784, 1574, 1627}, {0, -0.9, -60.0}},
    {215, 0, 0, -3.1, {1784, 1574, 1627}, {0, -60.0, -60.0}},
    {930, 0, 0, -24.3, {1796, 1580, 1627}, {0, -14.7, -60.0}},
    {935, 0, 0, -60.0, {1796, 251, 1627}, {0, -16.4, -60.0}},
    {950, 0, 0, -60.0, {1819, 222, 1627}, {0, -15.6, -60.0}},
    {980, 0, 0, -60.0, {1773, 251, 1627}, {0, -60.0, -60.0}},
    {985, 0, 0, -60.0, {1796, 251, 1627}, {0, -11.5, -60.0}},
    {990, 0, 0, -60.0, {1825, 251, 1627}, {0, -12.3, -0.9}},
    {995, 0, 0, -60.0, {1825, 1475, 1627}, {0, -13.8, -13.4}},
    {1000, 0, 0, -60.0, {1825, 1475, 1627}, {0, -11.2, -15.2}},
    {1005, 0, 0, -60.0, {1814, 251, 1627}, {0, -14.1, -13.7}},
    {1010, 0, 0, -60.0, {1805, 245, 478}, {0, -12.5, -14.4}},
    {1030, 0, 0, -60.0, {1800, 245, 478}, {0, -60.0, -60.0}},
    {1035, 0, 0, -60.0, {1795, 245, 478}, {0, -1.7, -60.0}},
    {1060, 0, 0, -60.0, {1808, 233, 583}, {0, -10.6, -14.1}},
    {1075, 0, 0, -60.0, {1831, 227, 1679}, {0, -11.5, -12.2}},
    {1085, 0, 0, -60.0, {1819, 513, 1679}, {0, -13.5, -15.3}},
    {1110, 0, 0, -60.0, {1796, 292, 443}, {0, -14.4, -16.5}},
    {1130, 0, 0, -60.0, {1779, 612, 840}, {0, -8.9, -12.9}},
    {1135, 2, 128, -60.0, {957, 1781, 2639}, {0, 26.7, 11.7}},
    {1160, 2, 128, -60.0, {1014, 1784, 2636}, {0, 24.6, 10.0}},
    {1165, 0, 0, -60.0, {1790, 239, 1586}, {0, -9.2, -13.0}},
    {1210, 0, 0, -60.0, {1802, 251, 461}, {0, -4.6, -9.0}},
    {1215, 0, 0, -60.0, {239, 496, 443}, {0, -5.8, -7.1}},
    {1220, 0, 0, -60.0, {239, 991, 443}, {0, -12.5, -14.5}},
    {1230, 0, 0, -60.0, {233, 496, 397}, {0, -2.2, -60.0}},
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
    {0, 16}, {16, 11}, {27, 15}, {42, 18}, {60, 15}, {75, 16}, {91, 18},
    {109, 19}, {128, 18}, {146, 16}, {162, 38}, {200, 38}, {238, 38}, {276, 20},
    {296, 33}, {329, 27}, {356, 18}, {374, 26}, {400, 33}, {433, 32}, {465, 11},
    {476, 14}, {490, 24}, {514, 15}, {529, 31}, {560, 27}, {587, 18}, {605, 19},
    {624, 12}, {636, 38}, {674, 40}, {714, 38}, {752, 40}, {792, 25}, {817, 40},
    {857, 38}, {895, 36}, {931, 37}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {968, 33}, {0, 0}, {1001, 16},
};

/* the effects the chip gates: their tones pulse on and off at a steady rate,
 * which the chip's envelope oscillator does. on the VoxRecord recording's
 * third take, measured from each effect's envelope a millisecond at a time:
 * C0 and C4 pulse at the default speech pitch (85 and 87Hz), C2 at 66Hz, C3
 * at 59Hz, C6 at 125Hz, on for a quarter to two fifths of each cycle (B9
 * pulses at 87Hz too, but plays closer to the recording ungated). C8 and C9
 * are gated noise, built in effect() (see c_noise()). the envelope shape for each is the one that brought it closest to
 * the recording. 0 is not gated. for codes 200 to 254. C0, C2, C3, C4 and C6
 * have since been found to pulse with the voicing instead (see fx_pulse),
 * which takes the place of their gates here */
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

/* the fourth VoxProbe recording plays the gated effects at pitch 60 and 150
 * and at bend 0 and 15, on both units. their gates follow the Pitch control,
 * a steady distance from the voicing frequency at all three pitches: C0 4.7Hz
 * under it, C2 20.5Hz under, C3 27.9Hz under, C4 on it and C6 42.7Hz over.
 * and each pulse has the same length whatever the rate: folded at the gate's
 * period, the envelope starts each pulse at full level and fades to nothing
 * in a fixed time, as the voicing does (see voice_fade): C4 as the voicing,
 * C0 about 1.3 times as long, C3 and C6 about 1.6 times, and C2 as long but
 * in two humps (its tones beating, restarted together each pulse). at low
 * pitch the rest of each period is silent; at high pitch the next pulse cuts
 * the fade short.
 *
 * C2, C4 and C6 play that way here: voiced at the voicing frequency plus
 * their distance, along voice_fade stretched as measured, with their noise
 * (see fx_noise) refitted to it. fitted on both recordings at the default
 * pitch, at pitch 60 and at bend 0, and checked at pitch 150, at speeds 90 and
 * 127 and at bend 15, which were left out: C2 73 to 77% and 2.5 to 2.2dB on
 * those, C4 65 to 71% and 4.0 to 3.6dB, C6 65 to 67% and 3.0 to 2.1dB. C0
 * and C3 came out worse played this way (on the 4kHz comparison of the
 * time); C3 pulses at a moving distance, and C0, measured full band, closer
 * and with a shorter fade (below).
 *
 * R8 and R9 pulse too. their low tone, near 650Hz, is restarted at half its
 * frequency: on both units of the fourth VoxProbe recording its lines are
 * 306, 333 and 380Hz apart at pitch 60, 88 and 150, the rate falls with the
 * tone (R9 333 to 296Hz over its first 40ms as the tone falls 667 to 609Hz),
 * and the rate doesn't move with Bend while the tone does, which is what put
 * the strongest line near 330Hz at bend 0 and near 1kHz at bend 15 (the low
 * tone read 126Hz lower there than on the VoxRecord recording). their high
 * tone (3.1 and 3.4kHz) is steady, so it moves to oscillator 4, and their
 * noise goes. here the restart rate is half the loudest tone of oscillators
 * 1 to 3, moved with the Pitch control as the others are; 10Hz either side
 * of half fitted 3 to 5 points worse. the fade, 0.7 of the voicing's, and
 * the tones' levels were fitted at pitch 60, at bend 0 and on the VoxRecord
 * recording: on pitch 150, bend 15 and the speeds, left out, R8 61 to 77% and
 * 4.9 to 2.8dB, R9 34 to 67% and 7.1 to 3.2dB. a rate rising 0.86Hz to each
 * hertz of the voicing, which pitch 150 suggests, made R8 worse.
 *
 * C3 pulses too, but its pulses slow down as it plays: on every C3 of both
 * units of the fourth VoxProbe recording and of the VoxRecord recording
 * they come 15, 15, 16, 16, 17, 19, 19, 22 and 22ms apart at the default
 * pitch, 5 to 6ms long each at every pitch. at speed 90 the slowing is
 * drawn out and at speed 127 squeezed, so it follows the trace, not the
 * clock, and at pitch 60 and 150 the gaps (24 to 50ms and 8 to 12ms) are
 * the voicing less a distance growing from about 20 to 42Hz along it, as at
 * the default pitch. so its distance falls by drift hertz for each
 * millisecond of the trace. fitted on every C3: 66.9 to 77.2% and 4.06 to
 * 2.67dB, pitch 60 50 to 72%, speed 127 65 to 79%, its old steady 59Hz gate
 * at pitch 60 having pulsed nearly twice as fast as the chip.
 *
 * C0 pulses as well. its gate is the voicing less 4 to 6Hz at all three
 * pitches (every 18.1, 12.3 and 7.0ms at pitch 60, 88 and 150, on both
 * units), where it had a steady 85Hz gate. a steady distance on that gate
 * made it worse, its square pulses too long at low pitch; pulses fading at
 * 0.66 of the voicing's fade, with its noise (see fx_noise) 6dB quieter,
 * fitted on every C0 of both recordings: 76.2 to 79.8% and 2.15 to 2.43dB,
 * pitch 60 73 to 80%, bend 0 76 to 83%, bend 15 72 to 78%, pitch 150 82 to
 * 79% */
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


/* the noise under some of the traced effects. on both units of the VoxRecord
 * recording C0, C1, C2, C4, C6 and C7 have a broad noise under their tones
 * that the traces, which take the strongest peaks as tones, leave out (C1 is
 * as noisy as it is tonal). here oscillators 4 and 5 add it, at a level
 * against the tones' and gated as deep as fitted, along with their
 * frequencies and width, to both units at once. C0 70 to 78% and 2.7 to
 * 2.3dB, C1 42 to 67% and 5.7 to 1.7dB, C2 74 to 79% and 5.1 to 3.3dB, C4 68
 * to 75% and 4.8 to 4.3dB, C6 66 to 72% and 3.8 to 2.7dB, C7 58 to 65% and
 * 6.3 to 1.9dB. C3's made it match one unit worse, so it has none. C2's, C4's
 * and C6's levels and depths were fitted again for their pulses (see
 * fx_pulse): 3dB quieter each, and 0.4, 1 and 0 deep.
 *
 * the tone effects (see fx2_points) have it too: on both units a pure tone
 * sits on a skirt of noise and scattered lines 15 to 30dB under it, across
 * the band, 15 to 30dB over the recording's background, where these had
 * clean sines with nothing. fitted the same way (depth 0: they aren't gated)
 * and kept where neither unit's spectrogram match fell more than half a
 * point: M0 67 to 97% and 5.6 to 0.5dB, A0 66 to 86% and 7.9 to 2.0dB, B6
 * 6.1 to 0.7dB, R2 3.3 to 0.6dB, R9 5.5 to 1.8dB, A6 3.7 to 1.4dB. B7's made
 * its spectrogram worse, as did R3's, R7's, R8's, A5's, B3's and B4's. R9's
 * went again when it was found to pulse (see fx_pulse) */
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
    {214, 2232.5, 3476.6, 176.3, 0, -13.45},
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

/* contour plays a traced effect: a quick start on the first point, then a
 * glide to each of the others in turn, then 3ms to silence. a tone doesn't
 * move while it is silent: it fades out where it was and fades in where it
 * is going. the traces give the silent points whatever the background held
 * (C3's first and last are 227Hz), and gliding from those swept each tone in
 * from far below, which at bend 15 put a strong sweep near 660Hz into C3 that
 * neither unit of the fourth VoxProbe recording has (10.6 to 10.1dB there) */
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

/* C8 and C9 are noise from about 400Hz to 2.8kHz, flat within a few dB, the
 * same on both units of the VoxRecord recording and nearly the same as each
 * other, gated at 52Hz 13 to 16dB deep. their traces had taken the noise's
 * strongest peaks for three tones, falling away above 1.5kHz. here they are
 * oscillators 4 and 5 spread wide, gated by a saw (see noise_depth), every
 * 5ms at the level both units have (the mean of the two, in dB against the
 * loudest); the frequencies, width and depth are fitted to both units at
 * once, and the depth comes out at 0.83 (-15dB), as measured. C8 and C9
 * 34% to 58% and 8.4 to 2.9dB.
 *
 * the fourth VoxProbe recording moves them with Bend (see msa_expand()), and
 * at bend 15 both units' noise has a sharp lower edge, nothing under 560Hz
 * within 20dB, which a new random frequency every sample, spreading the
 * noise in long skirts around the oscillator, can't give. holding each random
 * value 0.75ms makes a band with edges instead; with the spread, the
 * frequencies and the hold fitted to both recordings at default and bend 0
 * and the pitches, and scored over four noise sequences rather than one
 * (the noise carries on from the sounds before), C8 and C9 go from 49 to 55%
 * and 2.8 to 2.3dB there, 52 to 56% and 2.8 to 2.5dB on the VoxRecord
 * recording, and at bend 15, left out of the fit, from 37 to 62% and 8.3 to
 * 4.4dB.
 *
 * the gate keeps time with Speed, not Pitch: on both units of the fourth
 * VoxProbe recording C8's gate comes every 19.1ms at the default speed, 32.7
 * at speed 90 and 11.2 at speed 127 (1.71 and 0.59 times, where Speed
 * stretches the sound 1.75 and 0.59 times), and every 18.9 to 19.2ms at
 * pitch 60 and 150 and both bends. C8 at speed 90 62 to 68%, at speed 127 85
 * to 86% */
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
/* the sound effects that are pure tones, traced again from both units'
 * VoxRecord recordings: the tones were tracked every millisecond (12ms
 * windows), each tone followed while it lasts and split where its level
 * jumps, the two units' tracks paired and averaged (and a strong one heard on
 * only one unit kept), and each reduced to the points where it bends (10Hz,
 * 2dB). each tone gets an oscillator of its own, which takes its frequency
 * while silent before the tone starts, so tones start and stop in place
 * rather than sweeping in from another frequency as the first traces did. the
 * levels are the recording's with the output filter's loss added back,
 * scaled so that the DTMF tones play at their measured level. effects whose
 * recordings are mostly not tones (buzzes and noise) keep fx_points. each
 * point is its time after the one before (glided to in a straight line), the
 * five oscillators' frequencies and their volumes. A4's trace stopped at
 * 1315Hz, where its volume reached the oscillators' limit; on both units it
 * goes on, faster, to 3.9kHz by 190ms, holds there and dies away by 245ms.
 * from 1083Hz on it is tracked again every 4ms (the two units within 50Hz
 * and 0.5dB of each other) with this library's output filter loss added
 * back, against its level at 1083Hz. the volumes had been cut off at the
 * oscillators' limit (31), which flattened the loudest part of 30 effects
 * (A0's swell, B6's and B7's fall, the DTMF tones' whole body); each effect
 * is now scaled so that its loudest point is at the limit instead, which
 * keeps its shape below it: effects 82.9 to 83.5% and 2.91 to 2.45dB, B2 4.9
 * to 0.6dB, R0 3.1 to 1.0dB. A4, M0 and R6 keep their cut-off volumes, which
 * matched both units better */
typedef struct fx2_point {
  float ms;
  float freq[5];
  float vol[5];
} fx2_point;

static const fx2_point fx2_points[] = {
    {1.0, {794, 409, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {794, 409, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {794, 409, 0, 0, 0}, {2.52, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {798, 409, 0, 0, 0}, {16.51, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {803, 409, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {803, 409, 0, 0, 0}, {30.98, 4.08, 0.00, 0.00, 0.00}},
    {11.0, {803, 450, 0, 0, 0}, {30.78, 7.00, 0.00, 0.00, 0.00}},
    {24.0, {802, 539, 0, 0, 0}, {30.33, 8.44, 0.00, 0.00, 0.00}},
    {5.0, {802, 570, 0, 0, 0}, {30.24, 10.56, 0.00, 0.00, 0.00}},
    {1.0, {802, 570, 0, 0, 0}, {30.22, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {802, 570, 0, 0, 0}, {30.11, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {804, 570, 0, 0, 0}, {19.17, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {809, 570, 0, 0, 0}, {6.41, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {809, 570, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {809, 570, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {635, 1105, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {635, 1105, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {635, 1105, 0, 0, 0}, {0.37, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {681, 1105, 0, 0, 0}, {0.79, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {681, 1105, 0, 0, 0}, {1.01, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {681, 1105, 0, 0, 0}, {1.31, 2.09, 0.00, 0.00, 0.00}},
    {1.0, {681, 1105, 0, 0, 0}, {1.69, 3.31, 0.00, 0.00, 0.00}},
    {3.0, {685, 1105, 0, 0, 0}, {1.72, 13.19, 0.00, 0.00, 0.00}},
    {1.0, {686, 1105, 0, 0, 0}, {1.74, 15.42, 0.00, 0.00, 0.00}},
    {4.0, {660, 1105, 0, 0, 0}, {2.47, 28.86, 0.00, 0.00, 0.00}},
    {4.0, {699, 1105, 0, 0, 0}, {3.24, 29.03, 0.00, 0.00, 0.00}},
    {8.0, {698, 1105, 0, 0, 0}, {3.47, 29.37, 0.00, 0.00, 0.00}},
    {7.0, {733, 1105, 0, 0, 0}, {3.41, 29.67, 0.00, 0.00, 0.00}},
    {15.0, {735, 1105, 0, 0, 0}, {3.35, 30.33, 0.00, 0.00, 0.00}},
    {8.0, {776, 1105, 0, 0, 0}, {3.42, 30.69, 0.00, 0.00, 0.00}},
    {1.0, {766, 1105, 0, 0, 0}, {3.42, 30.73, 0.00, 0.00, 0.00}},
    {2.0, {777, 1105, 0, 0, 0}, {3.40, 30.82, 0.00, 0.00, 0.00}},
    {1.0, {765, 1105, 0, 0, 0}, {3.35, 30.86, 0.00, 0.00, 0.00}},
    {3.0, {766, 1105, 0, 0, 0}, {3.31, 31.00, 0.00, 0.00, 0.00}},
    {2.0, {767, 1105, 0, 0, 0}, {3.27, 27.70, 0.00, 0.00, 0.00}},
    {2.0, {715, 1105, 0, 0, 0}, {2.66, 24.75, 0.00, 0.00, 0.00}},
    {1.0, {710, 1104, 0, 0, 0}, {2.55, 23.40, 0.00, 0.00, 0.00}},
    {2.0, {701, 1105, 0, 0, 0}, {2.33, 16.96, 0.00, 0.00, 0.00}},
    {3.0, {733, 1107, 0, 0, 0}, {2.51, 10.46, 0.00, 0.00, 0.00}},
    {1.0, {744, 1107, 0, 0, 0}, {2.57, 7.34, 0.00, 0.00, 0.00}},
    {2.0, {768, 1107, 0, 0, 0}, {1.45, 3.62, 0.00, 0.00, 0.00}},
    {1.0, {768, 1107, 0, 0, 0}, {0.00, 2.54, 0.00, 0.00, 0.00}},
    {3.0, {768, 1107, 0, 0, 0}, {0.00, 0.37, 0.00, 0.00, 0.00}},
    {1.0, {768, 1107, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {768, 1107, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1300, 733, 1697, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1300, 733, 1697, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1300, 733, 1697, 0, 0}, {0.18, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1300, 733, 1697, 0, 0}, {1.01, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1300, 733, 1697, 0, 0}, {2.44, 1.03, 0.00, 0.00, 0.00}},
    {1.0, {1300, 653, 1697, 0, 0}, {4.18, 0.75, 0.00, 0.00, 0.00}},
    {1.0, {1300, 655, 1697, 0, 0}, {7.15, 1.01, 0.00, 0.00, 0.00}},
    {3.0, {1301, 659, 1697, 0, 0}, {18.65, 2.41, 0.00, 0.00, 0.00}},
    {3.0, {1303, 678, 1697, 0, 0}, {27.16, 2.89, 0.00, 0.00, 0.00}},
    {1.0, {1304, 685, 1697, 0, 0}, {30.78, 3.08, 3.67, 0.00, 0.00}},
    {4.0, {1304, 662, 1667, 0, 0}, {30.80, 3.11, 5.18, 0.00, 0.00}},
    {4.0, {1304, 639, 1606, 0, 0}, {30.81, 3.14, 6.90, 0.00, 0.00}},
    {1.0, {1304, 633, 1606, 0, 0}, {30.82, 3.15, 0.00, 0.00, 0.00}},
    {42.0, {1304, 392, 1606, 0, 0}, {31.00, 3.52, 0.00, 0.00, 0.00}},
    {3.0, {1303, 375, 1606, 0, 0}, {24.71, 3.55, 0.00, 0.00, 0.00}},
    {2.0, {1302, 376, 1606, 0, 0}, {21.24, 3.34, 0.00, 0.00, 0.00}},
    {1.0, {1303, 377, 1606, 0, 0}, {17.46, 3.24, 0.00, 0.00, 0.00}},
    {2.0, {1304, 415, 1606, 0, 0}, {11.80, 2.59, 0.00, 0.00, 0.00}},
    {1.0, {1305, 418, 1606, 0, 0}, {9.70, 2.39, 0.00, 0.00, 0.00}},
    {3.0, {1306, 386, 1606, 0, 0}, {5.39, 2.52, 0.00, 0.00, 0.00}},
    {1.0, {1305, 376, 1606, 0, 0}, {7.11, 2.57, 0.00, 0.00, 0.00}},
    {1.0, {1305, 388, 1606, 0, 0}, {0.00, 2.45, 0.00, 0.00, 0.00}},
    {2.0, {1305, 412, 1606, 0, 0}, {0.00, 2.23, 0.00, 0.00, 0.00}},
    {7.0, {1305, 421, 1606, 0, 0}, {0.00, 0.24, 0.00, 0.00, 0.00}},
    {1.0, {1305, 420, 1606, 0, 0}, {0.00, 0.60, 0.00, 0.00, 0.00}},
    {1.0, {1305, 420, 1606, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1305, 420, 1606, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1605, 636, 345, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1605, 636, 345, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1605, 636, 345, 0, 0}, {0.95, 0.87, 0.00, 0.00, 0.00}},
    {3.0, {1605, 636, 345, 0, 0}, {4.51, 2.96, 0.00, 0.00, 0.00}},
    {3.0, {1604, 645, 345, 0, 0}, {13.51, 3.97, 0.00, 0.00, 0.00}},
    {3.0, {1605, 653, 345, 0, 0}, {24.38, 5.32, 0.00, 0.00, 0.00}},
    {3.0, {1606, 662, 345, 0, 0}, {29.19, 7.12, 0.00, 0.00, 0.00}},
    {1.0, {1607, 670, 345, 0, 0}, {31.00, 7.18, 0.00, 0.00, 0.00}},
    {1.0, {1607, 678, 345, 0, 0}, {30.94, 7.24, 1.21, 0.00, 0.00}},
    {1.0, {1607, 686, 360, 0, 0}, {30.88, 7.30, 1.70, 0.00, 0.00}},
    {1.0, {1607, 694, 357, 0, 0}, {30.83, 7.36, 1.70, 0.00, 0.00}},
    {4.0, {1607, 697, 345, 0, 0}, {30.60, 7.44, 1.69, 0.00, 0.00}},
    {4.0, {1607, 699, 351, 0, 0}, {30.37, 7.52, 2.28, 0.00, 0.00}},
    {6.0, {1607, 728, 360, 0, 0}, {30.03, 6.62, 3.54, 0.00, 0.00}},
    {1.0, {1606, 729, 361, 0, 0}, {29.97, 6.43, 3.81, 0.00, 0.00}},
    {15.0, {1606, 737, 364, 0, 0}, {29.14, 4.21, 4.08, 0.00, 0.00}},
    {7.0, {1606, 771, 365, 0, 0}, {28.76, 3.20, 4.21, 0.00, 0.00}},
    {8.0, {1606, 769, 367, 0, 0}, {28.34, 3.57, 4.36, 0.00, 0.00}},
    {1.0, {1606, 758, 357, 0, 0}, {28.28, 3.49, 4.14, 0.00, 0.00}},
    {4.0, {1606, 717, 318, 0, 0}, {17.71, 3.18, 3.36, 0.00, 0.00}},
    {3.0, {1607, 726, 296, 0, 0}, {12.47, 3.04, 1.77, 0.00, 0.00}},
    {1.0, {1607, 728, 296, 0, 0}, {11.09, 3.00, 1.43, 0.00, 0.00}},
    {1.0, {1607, 731, 296, 0, 0}, {8.30, 2.95, 1.16, 0.00, 0.00}},
    {1.0, {1607, 731, 296, 0, 0}, {6.22, 2.53, 0.94, 0.00, 0.00}},
    {1.0, {1607, 731, 296, 0, 0}, {4.65, 2.17, 0.88, 0.00, 0.00}},
    {3.0, {1607, 731, 298, 0, 0}, {1.09, 1.36, 0.71, 0.00, 0.00}},
    {1.0, {1606, 731, 298, 0, 0}, {1.35, 1.17, 0.66, 0.00, 0.00}},
    {1.0, {1606, 731, 299, 0, 0}, {0.00, 1.15, 0.62, 0.00, 0.00}},
    {1.0, {1606, 731, 299, 0, 0}, {0.00, 1.14, 0.00, 0.00, 0.00}},
    {4.0, {1606, 731, 299, 0, 0}, {0.00, 1.08, 0.00, 0.00, 0.00}},
    {3.0, {1606, 731, 299, 0, 0}, {0.00, 0.35, 0.00, 0.00, 0.00}},
    {1.0, {1606, 731, 299, 0, 0}, {0.00, 0.19, 0.00, 0.00, 0.00}},
    {1.0, {1606, 706, 299, 0, 0}, {0.00, 0.07, 0.00, 0.00, 0.00}},
    {1.0, {1606, 706, 299, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1606, 706, 299, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1901, 629, 462, 282, 1476}, {0.13, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1901, 629, 462, 282, 1476}, {0.75, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1901, 629, 462, 282, 1476}, {1.83, 0.59, 0.00, 0.00, 0.00}},
    {1.0, {1901, 608, 462, 282, 1476}, {3.34, 0.55, 0.00, 0.00, 0.00}},
    {1.0, {1901, 603, 462, 282, 1476}, {6.08, 0.85, 0.00, 0.00, 0.00}},
    {3.0, {1904, 588, 462, 282, 1476}, {15.30, 3.12, 0.00, 0.00, 0.00}},
    {1.0, {1905, 572, 462, 282, 1476}, {20.81, 3.49, 0.00, 0.00, 0.00}},
    {1.0, {1905, 555, 462, 282, 1476}, {22.99, 3.90, 0.00, 0.00, 0.00}},
    {3.0, {1906, 549, 462, 282, 1476}, {31.00, 4.25, 0.00, 0.00, 0.00}},
    {3.0, {1906, 629, 462, 282, 1476}, {30.89, 4.75, 0.00, 0.00, 0.00}},
    {2.0, {1907, 635, 462, 282, 1476}, {30.82, 4.69, 0.00, 0.00, 0.00}},
    {2.0, {1907, 671, 462, 282, 1476}, {30.75, 4.21, 0.00, 0.00, 0.00}},
    {1.0, {1907, 682, 462, 282, 1476}, {30.71, 4.10, 5.21, 0.00, 0.00}},
    {1.0, {1907, 693, 463, 282, 1476}, {30.67, 4.00, 4.02, 0.00, 0.00}},
    {7.0, {1907, 770, 505, 282, 1476}, {30.42, 3.33, 4.32, 0.00, 0.00}},
    {3.0, {1907, 790, 522, 282, 1476}, {30.32, 3.18, 4.46, 0.00, 0.00}},
    {1.0, {1907, 797, 531, 282, 1476}, {30.28, 3.13, 4.36, 1.97, 0.00}},
    {1.0, {1907, 804, 540, 281, 1476}, {30.25, 3.08, 4.25, 2.05, 0.00}},
    {2.0, {1907, 833, 558, 279, 1476}, {30.17, 2.58, 4.04, 2.22, 0.00}},
    {1.0, {1907, 848, 567, 282, 1476}, {30.14, 2.37, 3.94, 2.41, 1.77}},
    {1.0, {1907, 863, 576, 286, 1472}, {30.10, 2.17, 3.85, 2.62, 1.75}},
    {1.0, {1907, 863, 585, 289, 1468}, {30.07, 2.07, 3.75, 2.84, 1.74}},
    {4.0, {1907, 863, 621, 304, 1514}, {29.93, 1.71, 3.40, 3.94, 2.59}},
    {1.0, {1907, 863, 630, 307, 1535}, {29.89, 1.63, 3.32, 4.27, 2.70}},
    {2.0, {1907, 863, 641, 314, 1578}, {29.82, 1.48, 3.30, 5.03, 2.94}},
    {1.0, {1907, 863, 646, 318, 1581}, {29.79, 1.41, 3.30, 5.00, 2.80}},
    {1.0, {1907, 863, 651, 321, 1618}, {29.75, 1.35, 3.29, 4.96, 2.86}},
    {1.0, {1907, 863, 657, 324, 1634}, {29.72, 1.28, 3.28, 4.93, 2.98}},
    {1.0, {1907, 863, 662, 327, 1634}, {29.68, 1.22, 3.27, 4.90, 0.00}},
    {3.0, {1907, 863, 678, 337, 1634}, {29.58, 1.06, 3.25, 4.80, 0.00}},
    {1.0, {1907, 861, 683, 340, 1634}, {29.54, 0.87, 3.24, 4.77, 0.00}},
    {3.0, {1907, 857, 710, 350, 1634}, {29.44, 0.48, 3.50, 4.68, 0.00}},
    {1.0, {1907, 857, 719, 354, 1634}, {29.41, 0.00, 3.58, 4.64, 0.00}},
    {4.0, {1907, 857, 734, 367, 1634}, {29.27, 0.00, 3.87, 4.52, 0.00}},
    {1.0, {1907, 857, 737, 366, 1634}, {29.23, 0.00, 3.94, 4.43, 0.00}},
    {1.0, {1907, 857, 729, 366, 1634}, {29.20, 0.00, 3.70, 4.34, 0.00}},
    {3.0, {1907, 857, 702, 365, 1634}, {20.17, 0.00, 3.07, 4.08, 0.00}},
    {1.0, {1907, 857, 709, 373, 1634}, {17.83, 0.00, 3.03, 3.55, 0.00}},
    {1.0, {1907, 857, 715, 441, 1634}, {15.76, 0.00, 3.00, 3.73, 0.00}},
    {1.0, {1907, 857, 722, 441, 1634}, {13.94, 0.00, 2.96, 0.00, 0.00}},
    {1.0, {1907, 857, 728, 441, 1634}, {12.32, 0.00, 2.92, 0.00, 0.00}},
    {1.0, {1906, 857, 728, 441, 1634}, {10.89, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1905, 857, 728, 441, 1634}, {1.30, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1905, 857, 728, 441, 1634}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1905, 857, 728, 441, 1634}, {0, 0, 0, 0, 0}},
    {1.0, {347, 2206, 810, 0, 0}, {0.12, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {347, 2206, 810, 0, 0}, {0.19, 0.12, 0.00, 0.00, 0.00}},
    {1.0, {347, 2206, 810, 0, 0}, {0.30, 0.43, 0.00, 0.00, 0.00}},
    {2.0, {347, 2206, 810, 0, 0}, {0.75, 2.36, 0.00, 0.00, 0.00}},
    {2.0, {347, 2206, 810, 0, 0}, {1.85, 6.25, 0.00, 0.00, 0.00}},
    {1.0, {334, 2206, 810, 0, 0}, {2.07, 10.17, 0.00, 0.00, 0.00}},
    {1.0, {321, 2206, 810, 0, 0}, {2.31, 12.71, 0.00, 0.00, 0.00}},
    {1.0, {307, 2207, 810, 0, 0}, {2.58, 15.89, 3.46, 0.00, 0.00}},
    {2.0, {302, 2207, 813, 0, 0}, {2.91, 24.83, 3.25, 0.00, 0.00}},
    {1.0, {310, 2208, 804, 0, 0}, {3.06, 25.95, 3.41, 0.00, 0.00}},
    {2.0, {359, 2208, 786, 0, 0}, {3.72, 28.36, 3.76, 0.00, 0.00}},
    {2.0, {379, 2208, 769, 0, 0}, {4.26, 31.00, 4.14, 0.00, 0.00}},
    {45.0, {381, 2208, 768, 0, 0}, {4.19, 25.60, 4.24, 0.00, 0.00}},
    {2.0, {382, 2208, 757, 0, 0}, {4.19, 25.38, 4.16, 0.00, 0.00}},
    {3.0, {362, 2208, 740, 0, 0}, {4.07, 25.06, 4.04, 0.00, 0.00}},
    {1.0, {356, 2208, 812, 0, 0}, {4.03, 21.18, 0.00, 0.00, 0.00}},
    {1.0, {349, 2208, 812, 0, 0}, {3.99, 17.90, 1.71, 0.00, 0.00}},
    {1.0, {361, 2208, 833, 0, 0}, {4.02, 15.13, 2.11, 0.00, 0.00}},
    {2.0, {384, 2207, 831, 0, 0}, {4.07, 10.81, 2.12, 0.00, 0.00}},
    {3.0, {419, 2207, 798, 0, 0}, {4.16, 6.52, 2.17, 0.00, 0.00}},
    {1.0, {431, 2207, 787, 0, 0}, {4.19, 4.20, 2.18, 0.00, 0.00}},
    {1.0, {446, 2207, 776, 0, 0}, {3.51, 2.71, 2.20, 0.00, 0.00}},
    {1.0, {462, 2207, 756, 0, 0}, {2.93, 1.74, 1.96, 0.00, 0.00}},
    {1.0, {477, 2208, 736, 0, 0}, {2.46, 0.97, 1.75, 0.00, 0.00}},
    {1.0, {492, 2208, 736, 0, 0}, {2.06, 0.54, 0.00, 0.00, 0.00}},
    {2.0, {492, 2209, 736, 0, 0}, {1.34, 0.17, 0.00, 0.00, 0.00}},
    {1.0, {492, 2209, 736, 0, 0}, {1.08, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {492, 2209, 736, 0, 0}, {0.88, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {496, 2209, 736, 0, 0}, {1.03, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {496, 2209, 736, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {496, 2209, 736, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1668, 477, 2511, 760, 878}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1668, 477, 2511, 760, 878}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1668, 477, 2511, 760, 878}, {0.26, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1668, 477, 2511, 760, 878}, {0.58, 1.39, 0.00, 0.00, 0.00}},
    {1.0, {1668, 475, 2511, 760, 878}, {1.27, 2.00, 0.75, 0.00, 0.00}},
    {2.0, {1654, 471, 2511, 760, 878}, {2.21, 4.14, 3.26, 0.00, 0.00}},
    {1.0, {1646, 469, 2511, 760, 878}, {2.91, 4.40, 5.29, 0.00, 0.00}},
    {2.0, {1597, 464, 2511, 760, 878}, {2.79, 4.96, 13.94, 0.00, 0.00}},
    {1.0, {1573, 462, 2510, 760, 878}, {2.74, 5.26, 18.16, 0.00, 0.00}},
    {1.0, {1548, 434, 2510, 760, 878}, {2.68, 5.42, 23.67, 0.00, 0.00}},
    {1.0, {1489, 405, 2510, 760, 878}, {2.17, 5.60, 30.84, 0.00, 0.00}},
    {1.0, {1707, 377, 2509, 760, 878}, {0.00, 5.78, 31.00, 3.20, 0.00}},
    {1.0, {1707, 367, 2509, 713, 878}, {0.00, 6.33, 31.00, 4.66, 0.00}},
    {1.0, {1707, 358, 2509, 719, 878}, {0.00, 6.95, 31.00, 4.95, 0.00}},
    {3.0, {1707, 366, 2509, 739, 878}, {0.00, 7.07, 31.00, 5.92, 0.00}},
    {3.0, {1707, 374, 2509, 735, 878}, {0.00, 7.20, 31.00, 6.05, 0.00}},
    {3.0, {1707, 363, 2509, 712, 878}, {0.00, 7.11, 31.00, 6.06, 0.00}},
    {1.0, {1707, 359, 2509, 704, 878}, {2.00, 7.08, 31.00, 6.07, 1.06}},
    {3.0, {1691, 348, 2509, 682, 877}, {3.38, 6.99, 31.00, 6.08, 1.29}},
    {5.0, {1643, 329, 2509, 644, 874}, {4.26, 6.85, 31.00, 6.10, 1.77}},
    {6.0, {1535, 307, 2509, 598, 871}, {3.33, 6.69, 31.00, 6.14, 2.58}},
    {3.0, {1482, 302, 2509, 575, 869}, {2.95, 4.94, 31.00, 6.15, 3.12}},
    {1.0, {1482, 301, 2509, 568, 869}, {3.01, 4.47, 31.00, 6.16, 3.32}},
    {2.0, {1482, 297, 2509, 553, 840}, {3.12, 3.65, 31.00, 6.17, 3.86}},
    {1.0, {542, 296, 2509, 549, 826}, {0.00, 3.30, 31.00, 6.34, 4.17}},
    {2.0, {542, 292, 2509, 541, 798}, {0.00, 2.70, 31.00, 6.69, 4.84}},
    {3.0, {542, 287, 2509, 529, 772}, {0.00, 1.99, 31.00, 7.25, 5.20}},
    {1.0, {542, 1622, 2509, 522, 763}, {0.00, 0.00, 31.00, 7.32, 5.33}},
    {8.0, {542, 1622, 2510, 466, 692}, {0.00, 0.00, 31.00, 7.94, 6.45}},
    {1.0, {542, 1622, 2510, 466, 683}, {0.00, 0.00, 31.00, 0.00, 6.60}},
    {2.0, {542, 1622, 2510, 466, 643}, {0.00, 0.00, 31.00, 0.00, 6.82}},
    {4.0, {542, 1622, 2509, 466, 625}, {0.00, 0.00, 31.00, 0.00, 7.46}},
    {1.0, {542, 1622, 2509, 466, 625}, {5.79, 0.00, 31.00, 0.00, 0.00}},
    {1.0, {538, 1622, 2509, 466, 625}, {5.62, 3.46, 31.00, 0.00, 0.00}},
    {2.0, {530, 1600, 2509, 466, 625}, {5.29, 2.72, 26.30, 0.00, 0.00}},
    {1.0, {526, 1607, 2509, 466, 625}, {5.13, 2.69, 22.60, 0.00, 0.00}},
    {2.0, {518, 1668, 2508, 466, 625}, {4.84, 2.84, 16.68, 0.00, 0.00}},
    {1.0, {502, 1670, 2508, 466, 625}, {4.03, 2.63, 14.33, 0.00, 0.00}},
    {2.0, {469, 1675, 2508, 466, 625}, {2.79, 2.25, 6.77, 0.00, 0.00}},
    {2.0, {451, 1680, 2508, 466, 625}, {3.64, 2.48, 3.20, 0.00, 0.00}},
    {1.0, {442, 1683, 2508, 466, 625}, {4.16, 2.61, 1.72, 0.00, 0.00}},
    {2.0, {473, 1688, 2506, 466, 625}, {3.73, 2.87, 0.50, 0.00, 0.00}},
    {1.0, {488, 1685, 2506, 466, 625}, {3.53, 2.34, 0.00, 0.00, 0.00}},
    {1.0, {504, 1682, 2506, 466, 625}, {3.35, 1.91, 0.00, 0.00, 0.00}},
    {1.0, {504, 1681, 2506, 466, 625}, {0.00, 1.19, 0.00, 0.00, 0.00}},
    {3.0, {504, 1678, 2506, 466, 625}, {0.00, 0.29, 0.00, 0.00, 0.00}},
    {1.0, {504, 1678, 2506, 466, 625}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {504, 1678, 2506, 466, 625}, {0, 0, 0, 0, 0}},
    {1.0, {2805, 593, 639, 424, 901}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2805, 593, 639, 424, 901}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2805, 593, 639, 424, 901}, {0.73, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2805, 593, 639, 424, 901}, {1.60, 1.12, 0.00, 0.00, 0.00}},
    {1.0, {2805, 600, 639, 424, 901}, {3.51, 2.17, 0.00, 0.00, 0.00}},
    {2.0, {2805, 565, 639, 424, 901}, {8.33, 3.45, 0.00, 0.00, 0.00}},
    {1.0, {2805, 529, 639, 424, 901}, {12.82, 3.40, 0.00, 0.00, 0.00}},
    {1.0, {2806, 492, 639, 424, 901}, {15.84, 3.34, 0.00, 0.00, 0.00}},
    {1.0, {2808, 480, 639, 424, 901}, {19.56, 3.37, 0.00, 0.00, 0.00}},
    {1.0, {2809, 477, 639, 424, 901}, {24.16, 3.51, 2.99, 0.00, 0.00}},
    {1.0, {2809, 475, 625, 424, 901}, {25.72, 3.67, 3.43, 0.00, 0.00}},
    {1.0, {2810, 493, 611, 424, 901}, {27.37, 4.86, 3.94, 0.00, 0.00}},
    {1.0, {2810, 1002, 597, 424, 901}, {29.13, 0.00, 4.52, 0.00, 0.00}},
    {1.0, {2811, 1002, 597, 424, 901}, {31.00, 0.00, 4.78, 0.00, 0.00}},
    {1.0, {2811, 1002, 596, 424, 901}, {30.97, 3.33, 5.06, 0.00, 0.00}},
    {2.0, {2811, 1021, 596, 424, 901}, {30.91, 3.13, 5.66, 0.00, 0.00}},
    {2.0, {2811, 1041, 614, 424, 901}, {30.85, 2.94, 5.33, 0.00, 0.00}},
    {1.0, {2811, 1051, 623, 424, 901}, {30.82, 2.85, 5.17, 4.19, 0.00}},
    {2.0, {2810, 1070, 641, 430, 901}, {30.77, 2.67, 4.86, 4.44, 0.00}},
    {1.0, {2810, 1083, 650, 433, 901}, {30.74, 2.36, 4.72, 4.57, 0.00}},
    {3.0, {2810, 1120, 664, 442, 901}, {30.65, 1.62, 4.26, 4.98, 0.00}},
    {1.0, {2810, 1133, 669, 445, 901}, {30.62, 1.43, 4.11, 5.12, 2.83}},
    {1.0, {2810, 260, 674, 448, 907}, {30.59, 0.00, 3.97, 5.27, 2.93}},
    {2.0, {2810, 260, 683, 455, 919}, {30.53, 0.00, 3.71, 5.59, 3.13}},
    {2.0, {2810, 260, 693, 466, 931}, {30.47, 0.00, 3.46, 5.47, 3.35}},
    {1.0, {2810, 260, 697, 471, 943}, {30.45, 0.00, 3.34, 5.41, 3.44}},
    {1.0, {2810, 260, 761, 477, 956}, {30.42, 0.00, 0.00, 5.36, 3.54}},
    {3.0, {2810, 260, 761, 494, 993}, {30.33, 0.00, 0.00, 5.19, 3.84}},
    {1.0, {2810, 260, 761, 499, 1005}, {30.30, 1.88, 0.00, 5.14, 3.95}},
    {1.0, {2810, 261, 761, 505, 1018}, {30.27, 1.98, 0.00, 5.09, 4.06}},
    {1.0, {2810, 262, 761, 507, 1023}, {30.24, 2.09, 1.83, 4.99, 4.02}},
    {1.0, {2810, 263, 764, 510, 1028}, {30.22, 2.20, 2.23, 4.90, 3.97}},
    {7.0, {2810, 269, 778, 526, 1062}, {30.02, 3.18, 1.37, 4.29, 3.69}},
    {1.0, {2810, 270, 780, 529, 1073}, {29.99, 3.35, 1.28, 4.21, 3.70}},
    {1.0, {2810, 271, 1077, 534, 1083}, {29.96, 3.53, 0.00, 4.16, 3.70}},
    {4.0, {2810, 275, 1077, 557, 1125}, {29.84, 4.36, 0.00, 3.95, 3.73}},
    {6.0, {2809, 281, 1077, 591, 1188}, {29.68, 4.40, 0.00, 3.66, 3.77}},
    {3.0, {2809, 284, 1077, 595, 1196}, {29.59, 4.42, 0.00, 3.87, 3.83}},
    {2.0, {2809, 286, 1077, 579, 1204}, {23.89, 4.43, 0.00, 3.73, 3.14}},
    {1.0, {2809, 287, 1077, 571, 1231}, {21.47, 4.44, 0.00, 3.67, 2.81}},
    {1.0, {2809, 296, 1077, 563, 1258}, {19.29, 4.45, 2.55, 3.60, 2.51}},
    {1.0, {2809, 304, 1072, 566, 1266}, {17.34, 4.46, 2.63, 3.55, 1.87}},
    {1.0, {2809, 321, 1066, 569, 1273}, {15.58, 4.50, 2.71, 3.51, 1.39}},
    {1.0, {2809, 337, 1082, 572, 1273}, {14.00, 4.54, 2.81, 3.46, 0.00}},
    {1.0, {2809, 354, 1098, 572, 1273}, {12.58, 4.59, 2.90, 0.00, 0.00}},
    {1.0, {2809, 370, 1114, 572, 1273}, {9.15, 4.63, 3.00, 0.00, 0.00}},
    {2.0, {2810, 403, 1126, 572, 1273}, {4.85, 4.72, 2.91, 0.00, 0.00}},
    {2.0, {2811, 403, 1138, 572, 1273}, {2.57, 3.62, 2.82, 0.00, 0.00}},
    {1.0, {2811, 404, 1139, 572, 1273}, {1.87, 3.17, 2.14, 0.00, 0.00}},
    {1.0, {2811, 405, 1139, 572, 1273}, {0.00, 2.36, 1.62, 0.00, 0.00}},
    {3.0, {2811, 409, 1141, 572, 1273}, {0.00, 0.97, 0.71, 0.00, 0.00}},
    {6.0, {2811, 409, 1141, 572, 1273}, {0.00, 0.85, 0.78, 0.00, 0.00}},
    {2.0, {2811, 410, 1144, 572, 1273}, {0.00, 0.48, 0.35, 0.00, 0.00}},
    {1.0, {2811, 410, 1144, 572, 1273}, {0.00, 0.36, 0.00, 0.00, 0.00}},
    {2.0, {2811, 410, 1144, 572, 1273}, {0.00, 0.20, 0.00, 0.00, 0.00}},
    {1.0, {2811, 410, 1144, 572, 1273}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2811, 410, 1144, 572, 1273}, {0, 0, 0, 0, 0}},
    {1.0, {728, 0, 575, 3111, 2311}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {728, 0, 575, 3111, 2311}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {728, 0, 575, 3111, 2311}, {0.89, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {709, 0, 575, 3111, 2311}, {2.13, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {707, 0, 575, 3111, 2311}, {3.39, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {706, 0, 575, 3111, 2311}, {5.40, 0.00, 0.00, 1.75, 0.00}},
    {5.0, {748, 0, 575, 3111, 2311}, {10.50, 0.00, 0.00, 14.17, 0.00}},
    {1.0, {756, 0, 575, 3111, 2311}, {12.00, 0.00, 9.03, 16.31, 0.00}},
    {1.0, {2783, 0, 607, 3111, 2311}, {0.00, 0.00, 9.51, 18.79, 0.00}},
    {1.0, {2783, 0, 621, 3111, 2311}, {4.11, 0.00, 11.43, 21.63, 0.00}},
    {1.0, {2779, 0, 634, 3111, 2311}, {4.62, 0.00, 13.73, 24.91, 0.00}},
    {1.0, {2774, 0, 643, 3110, 2311}, {5.19, 0.00, 14.28, 28.68, 0.00}},
    {1.0, {2770, 0, 652, 3111, 2311}, {5.83, 0.00, 14.85, 28.73, 0.00}},
    {2.0, {2725, 0, 670, 3111, 2311}, {5.85, 0.00, 16.05, 28.83, 0.00}},
    {1.0, {2702, 0, 679, 3111, 2311}, {5.85, 0.00, 16.70, 28.87, 1.07}},
    {1.0, {2679, 0, 677, 3111, 2317}, {5.85, 0.00, 16.70, 28.92, 0.91}},
    {2.0, {2655, 0, 674, 3111, 2317}, {6.27, 0.00, 16.70, 29.02, 1.14}},
    {4.0, {2658, 0, 669, 3111, 2317}, {4.12, 0.00, 16.70, 29.21, 1.80}},
    {4.0, {2660, 0, 663, 3111, 2316}, {2.70, 0.00, 16.70, 29.40, 2.83}},
    {3.0, {2663, 0, 659, 3111, 2316}, {1.98, 0.00, 16.70, 29.55, 3.99}},
    {1.0, {2663, 0, 658, 3111, 2304}, {1.77, 0.00, 16.70, 29.60, 3.97}},
    {1.0, {2670, 0, 657, 3111, 2291}, {2.22, 0.00, 16.70, 29.65, 3.95}},
    {1.0, {1910, 0, 655, 3111, 2278}, {0.00, 0.00, 16.70, 29.70, 3.93}},
    {1.0, {1910, 0, 654, 3111, 2266}, {0.00, 0.00, 16.70, 29.75, 3.92}},
    {3.0, {1910, 0, 650, 3111, 2228}, {0.00, 0.00, 16.70, 29.89, 3.86}},
    {1.0, {1910, 0, 648, 3111, 2228}, {0.00, 0.00, 16.70, 29.94, 3.71}},
    {1.0, {1910, 0, 647, 3111, 2228}, {0.85, 0.00, 16.70, 29.99, 3.56}},
    {5.0, {1905, 0, 640, 3111, 2228}, {2.25, 0.00, 16.70, 30.24, 2.90}},
    {2.0, {1903, 0, 637, 3111, 2228}, {3.31, 0.00, 16.70, 30.34, 2.67}},
    {1.0, {1903, 0, 636, 3111, 2228}, {4.02, 0.00, 16.70, 30.39, 2.57}},
    {1.0, {1903, 0, 634, 3111, 2228}, {4.21, 0.00, 16.70, 30.44, 2.46}},
    {1.0, {1902, 0, 633, 3111, 2224}, {4.41, 0.00, 16.70, 30.49, 2.18}},
    {1.0, {1902, 0, 632, 3111, 2220}, {4.62, 0.00, 16.70, 30.54, 1.93}},
    {1.0, {1890, 0, 630, 3112, 2211}, {4.74, 0.00, 16.70, 30.59, 2.42}},
    {1.0, {1877, 0, 629, 3112, 2211}, {4.88, 0.00, 16.70, 30.64, 0.00}},
    {2.0, {1851, 0, 626, 3112, 2211}, {5.15, 0.00, 16.70, 30.75, 0.00}},
    {2.0, {1825, 0, 623, 3112, 2211}, {5.45, 0.00, 16.70, 30.85, 0.00}},
    {3.0, {1827, 0, 619, 3112, 2211}, {5.70, 0.00, 16.70, 31.00, 0.00}},
    {1.0, {1827, 0, 618, 3112, 2211}, {5.79, 0.00, 16.70, 30.05, 0.00}},
    {2.0, {1828, 0, 615, 3112, 2211}, {5.99, 0.00, 16.70, 28.23, 0.00}},
    {1.0, {1823, 0, 614, 3112, 2211}, {5.76, 0.00, 16.70, 27.37, 0.00}},
    {2.0, {1814, 0, 584, 3112, 2211}, {5.35, 0.00, 15.35, 25.71, 0.00}},
    {1.0, {1809, 0, 569, 3111, 2211}, {5.15, 0.00, 14.71, 22.89, 0.00}},
    {2.0, {1792, 0, 540, 3111, 2211}, {4.56, 0.00, 13.53, 18.14, 0.00}},
    {1.0, {1730, 0, 540, 3111, 2211}, {3.85, 0.00, 12.88, 16.15, 0.00}},
    {1.0, {689, 0, 540, 3111, 2211}, {0.00, 0.00, 12.27, 14.38, 0.00}},
    {0.7, {689, 0, 540, 3111, 2211}, {0.00, 0.00, 11.83, 11.91, 0.00}},
    {1.0, {689, 0, 541, 3111, 2211}, {7.73, 0.00, 11.27, 9.25, 0.00}},
    {0.5, {688, 0, 541, 3111, 2211}, {7.73, 0.00, 11.12, 8.67, 0.00}},
    {1.0, {688, 0, 574, 3111, 2211}, {7.71, 0.00, 17.49, 6.73, 0.00}},
    {0.7, {687, 0, 574, 3111, 2211}, {7.70, 0.00, 0.00, 5.58, 0.00}},
    {0.5, {682, 0, 574, 3111, 2211}, {7.68, 0.00, 0.00, 5.23, 0.00}},
    {0.7, {667, 0, 574, 3111, 2211}, {7.62, 0.00, 0.00, 3.59, 0.00}},
    {1.0, {624, 0, 574, 3111, 2211}, {7.98, 0.00, 0.00, 2.15, 0.00}},
    {1.0, {614, 0, 574, 3111, 2211}, {7.80, 0.00, 0.00, 1.29, 0.00}},
    {0.5, {614, 0, 574, 3111, 2211}, {7.12, 0.00, 0.00, 1.12, 0.00}},
    {1.0, {615, 0, 574, 3112, 2211}, {5.10, 0.00, 0.00, 1.26, 0.00}},
    {1.0, {616, 0, 574, 3112, 2211}, {3.65, 0.00, 0.00, 0.00, 0.00}},
    {3.7, {619, 0, 574, 3112, 2211}, {1.05, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {619, 0, 574, 3112, 2211}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {619, 0, 574, 3112, 2211}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {680, 1693, 0, 3412, 334}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {680, 1693, 0, 3412, 334}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {680, 1693, 0, 3412, 334}, {1.17, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {678, 1693, 0, 3412, 334}, {2.02, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {676, 1693, 0, 3412, 334}, {3.51, 1.30, 0.00, 1.50, 0.00}},
    {2.0, {682, 1710, 0, 3412, 334}, {6.22, 2.76, 0.00, 5.24, 0.00}},
    {2.0, {686, 1756, 0, 3412, 334}, {8.00, 3.23, 0.00, 11.87, 0.00}},
    {1.0, {689, 1763, 0, 3412, 334}, {9.06, 3.35, 0.00, 17.84, 0.00}},
    {1.0, {681, 1771, 0, 3413, 334}, {9.87, 3.48, 0.00, 21.95, 0.00}},
    {1.0, {673, 1800, 0, 3414, 334}, {10.74, 4.00, 0.00, 27.00, 0.00}},
    {1.0, {666, 1677, 0, 3414, 334}, {11.69, 0.00, 0.00, 33.22, 0.00}},
    {2.0, {651, 1677, 0, 3414, 334}, {13.85, 0.00, 0.00, 37.65, 0.00}},
    {1.0, {657, 1677, 0, 3413, 334}, {14.12, 0.00, 0.00, 40.08, 0.00}},
    {1.0, {664, 1677, 0, 3413, 334}, {14.41, 3.26, 0.00, 42.66, 0.00}},
    {1.0, {671, 1702, 0, 3413, 334}, {14.70, 4.08, 0.00, 42.62, 0.00}},
    {1.0, {678, 1695, 0, 3413, 334}, {14.98, 4.29, 0.00, 42.59, 2.60}},
    {2.0, {677, 1681, 0, 3413, 334}, {14.99, 4.75, 0.00, 42.49, 1.49}},
    {1.0, {676, 1674, 0, 3413, 350}, {14.99, 4.99, 0.00, 42.46, 1.43}},
    {1.0, {675, 1667, 0, 3413, 349}, {14.99, 5.26, 0.00, 42.42, 1.38}},
    {2.0, {674, 1667, 0, 3413, 348}, {15.00, 5.25, 0.00, 42.34, 1.29}},
    {1.0, {673, 1667, 0, 3413, 328}, {15.00, 5.24, 0.00, 42.30, 1.19}},
    {2.0, {671, 1667, 0, 3413, 347}, {15.00, 5.23, 0.00, 42.22, 1.07}},
    {1.0, {671, 1667, 0, 3413, 324}, {15.02, 5.21, 0.00, 42.18, 1.01}},
    {1.0, {670, 1667, 0, 3413, 335}, {15.02, 5.21, 0.00, 42.13, 0.97}},
    {1.0, {669, 1667, 0, 3413, 344}, {15.02, 5.20, 0.00, 42.11, 0.97}},
    {1.0, {668, 1668, 0, 3413, 327}, {15.02, 5.19, 0.00, 42.06, 0.98}},
    {2.0, {667, 1668, 0, 3413, 360}, {15.03, 5.18, 0.00, 41.98, 1.08}},
    {1.0, {663, 1657, 0, 3413, 331}, {14.97, 5.18, 0.00, 41.94, 1.00}},
    {3.0, {651, 1626, 0, 3413, 300}, {14.79, 5.16, 0.00, 41.82, 1.10}},
    {2.0, {643, 1605, 0, 3413, 308}, {14.69, 5.16, 0.00, 41.74, 1.34}},
    {1.0, {643, 1605, 0, 3413, 312}, {14.72, 5.14, 0.00, 41.70, 1.49}},
    {2.0, {643, 1605, 0, 3413, 340}, {14.76, 5.12, 0.00, 41.62, 1.84}},
    {1.0, {642, 1604, 0, 3413, 323}, {14.78, 5.09, 0.00, 41.59, 1.95}},
    {1.0, {642, 1604, 0, 3413, 688}, {14.82, 5.08, 0.00, 41.55, 0.00}},
    {7.0, {640, 1602, 0, 3413, 688}, {14.99, 4.97, 0.00, 41.28, 0.00}},
    {1.0, {640, 1594, 0, 3413, 688}, {15.02, 5.04, 0.00, 41.23, 0.00}},
    {7.0, {626, 1538, 0, 3413, 688}, {14.67, 5.47, 0.00, 40.96, 0.00}},
    {6.0, {614, 1542, 0, 3413, 688}, {14.40, 4.81, 0.00, 40.74, 0.00}},
    {1.0, {612, 1543, 0, 3413, 688}, {14.36, 4.70, 0.00, 36.91, 0.00}},
    {4.0, {534, 1546, 0, 3412, 688}, {11.69, 4.31, 0.00, 24.90, 0.00}},
    {2.0, {531, 1548, 0, 3412, 688}, {10.75, 4.12, 0.00, 20.46, 0.00}},
    {1.7, {528, 1517, 0, 3411, 688}, {9.98, 3.72, 0.00, 13.17, 0.00}},
    {0.5, {528, 1512, 0, 3411, 688}, {9.88, 3.66, 0.00, 12.36, 0.00}},
    {0.7, {526, 1519, 0, 3411, 688}, {9.57, 3.61, 0.00, 10.25, 6.21}},
    {0.5, {526, 1522, 0, 3411, 688}, {9.46, 3.60, 0.00, 9.60, 6.23}},
    {1.0, {545, 1531, 0, 3411, 688}, {13.72, 3.52, 0.00, 7.47, 6.31}},
    {0.7, {545, 1538, 0, 3410, 689}, {0.00, 3.48, 0.00, 6.20, 6.37}},
    {0.5, {545, 1540, 0, 3410, 684}, {0.00, 3.45, 0.00, 5.80, 6.36}},
    {0.7, {545, 1574, 0, 3410, 672}, {0.00, 3.29, 0.00, 4.81, 6.32}},
    {0.5, {545, 1587, 0, 3410, 661}, {0.00, 3.23, 0.00, 4.51, 6.39}},
    {0.7, {545, 1587, 0, 3410, 630}, {0.00, 0.00, 0.00, 0.00, 6.60}},
    {0.5, {545, 1587, 0, 3410, 627}, {0.00, 0.00, 0.00, 0.00, 6.59}},
    {0.7, {545, 1587, 0, 3410, 618}, {0.00, 0.00, 0.00, 0.00, 6.58}},
    {2.0, {545, 1587, 0, 3410, 623}, {0.00, 0.00, 0.00, 0.00, 4.44}},
    {4.0, {545, 1587, 0, 3410, 628}, {0.00, 0.00, 0.00, 0.00, 0.94}},
    {1.0, {545, 1587, 0, 3410, 628}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {545, 1587, 0, 3410, 628}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1615, 305, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {11.0, {1615, 305, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1615, 305, 0, 0, 0}, {5.71, 0.00, 0.00, 0.00, 0.00}},
    {15.0, {1695, 305, 0, 0, 0}, {7.41, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1718, 305, 0, 0, 0}, {6.47, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1724, 305, 0, 0, 0}, {6.25, 1.92, 0.00, 0.00, 0.00}},
    {1.0, {1729, 301, 0, 0, 0}, {6.04, 1.32, 0.00, 0.00, 0.00}},
    {1.0, {1735, 300, 0, 0, 0}, {5.84, 1.67, 0.00, 0.00, 0.00}},
    {5.0, {1763, 294, 0, 0, 0}, {5.89, 5.35, 0.00, 0.00, 0.00}},
    {7.0, {1803, 215, 0, 0, 0}, {5.97, 2.89, 0.00, 0.00, 0.00}},
    {2.0, {1814, 228, 0, 0, 0}, {6.00, 3.45, 0.00, 0.00, 0.00}},
    {3.0, {1831, 281, 0, 0, 0}, {6.04, 6.05, 0.00, 0.00, 0.00}},
    {7.0, {1870, 242, 0, 0, 0}, {6.13, 5.04, 0.00, 0.00, 0.00}},
    {6.0, {1904, 283, 0, 0, 0}, {6.21, 8.02, 0.00, 0.00, 0.00}},
    {5.0, {1932, 262, 0, 0, 0}, {6.29, 6.60, 0.00, 0.00, 0.00}},
    {5.0, {1961, 292, 0, 0, 0}, {6.37, 8.83, 0.00, 0.00, 0.00}},
    {5.0, {1989, 277, 0, 0, 0}, {6.45, 7.50, 0.00, 0.00, 0.00}},
    {5.0, {2017, 303, 0, 0, 0}, {6.53, 9.33, 0.00, 0.00, 0.00}},
    {5.0, {2045, 293, 0, 0, 0}, {6.62, 7.76, 0.00, 0.00, 0.00}},
    {5.0, {2073, 316, 0, 0, 0}, {6.71, 9.39, 0.00, 0.00, 0.00}},
    {4.0, {2096, 303, 0, 0, 0}, {6.79, 8.56, 0.00, 0.00, 0.00}},
    {5.0, {2124, 327, 0, 0, 0}, {6.89, 9.42, 0.00, 0.00, 0.00}},
    {4.0, {2147, 312, 0, 0, 0}, {6.97, 8.93, 0.00, 0.00, 0.00}},
    {4.0, {2169, 340, 0, 0, 0}, {7.05, 8.40, 0.00, 0.00, 0.00}},
    {5.0, {2197, 314, 0, 0, 0}, {7.16, 9.27, 0.00, 0.00, 0.00}},
    {4.0, {2220, 360, 0, 0, 0}, {7.25, 8.16, 0.00, 0.00, 0.00}},
    {3.0, {2237, 322, 0, 0, 0}, {7.31, 10.79, 0.00, 0.00, 0.00}},
    {3.0, {2254, 269, 0, 0, 0}, {7.38, 8.45, 0.00, 0.00, 0.00}},
    {1.0, {2259, 274, 0, 0, 0}, {7.40, 7.49, 0.00, 0.00, 0.00}},
    {1.0, {2265, 361, 0, 0, 0}, {7.43, 9.21, 0.00, 0.00, 0.00}},
    {1.0, {2271, 224, 0, 0, 0}, {7.45, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {2305, 224, 0, 0, 0}, {7.59, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2310, 224, 0, 0, 0}, {7.62, 7.84, 0.00, 0.00, 0.00}},
    {1.0, {2316, 266, 0, 0, 0}, {7.64, 8.66, 0.00, 0.00, 0.00}},
    {3.0, {2333, 291, 0, 0, 0}, {7.71, 12.42, 0.00, 0.00, 0.00}},
    {5.0, {2362, 236, 0, 0, 0}, {7.44, 11.42, 0.00, 0.00, 0.00}},
    {4.0, {2385, 280, 0, 0, 0}, {7.22, 15.38, 0.00, 0.00, 0.00}},
    {4.0, {2408, 247, 0, 0, 0}, {7.01, 14.21, 0.00, 0.00, 0.00}},
    {4.0, {2431, 279, 0, 0, 0}, {6.81, 17.69, 0.00, 0.00, 0.00}},
    {3.0, {2449, 258, 0, 0, 0}, {6.66, 16.99, 0.00, 0.00, 0.00}},
    {4.0, {2472, 280, 0, 0, 0}, {6.47, 19.24, 0.00, 0.00, 0.00}},
    {4.0, {2495, 263, 0, 0, 0}, {6.28, 18.30, 0.00, 0.00, 0.00}},
    {4.0, {2518, 285, 0, 0, 0}, {6.10, 20.93, 0.00, 0.00, 0.00}},
    {3.0, {2536, 273, 0, 0, 0}, {5.96, 20.68, 0.00, 0.00, 0.00}},
    {4.0, {2559, 289, 0, 0, 0}, {5.79, 22.30, 0.00, 0.00, 0.00}},
    {3.0, {2576, 288, 0, 0, 0}, {5.66, 22.38, 0.00, 0.00, 0.00}},
    {8.0, {2599, 285, 0, 0, 0}, {5.45, 22.62, 0.00, 0.00, 0.00}},
    {3.0, {2608, 299, 0, 0, 0}, {5.37, 24.10, 0.00, 0.00, 0.00}},
    {2.0, {2613, 301, 0, 0, 0}, {5.32, 24.22, 0.00, 0.00, 0.00}},
    {3.0, {2653, 304, 0, 0, 0}, {5.67, 24.41, 0.00, 0.00, 0.00}},
    {5.0, {2653, 308, 0, 0, 0}, {4.43, 24.72, 0.00, 0.00, 0.00}},
    {1.0, {2574, 309, 0, 0, 0}, {5.12, 24.78, 0.00, 0.00, 0.00}},
    {1.0, {415, 310, 0, 0, 0}, {0.00, 24.85, 0.00, 0.00, 0.00}},
    {87.0, {415, 384, 0, 0, 0}, {0.00, 31.00, 0.00, 0.00, 0.00}},
    {18.0, {415, 342, 0, 0, 0}, {0.00, 17.90, 0.00, 0.00, 0.00}},
    {4.0, {415, 308, 0, 0, 0}, {0.00, 13.56, 0.00, 0.00, 0.00}},
    {1.0, {415, 305, 0, 0, 0}, {0.00, 16.94, 0.00, 0.00, 0.00}},
    {1.0, {415, 305, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {415, 305, 0, 0, 0}, {10.70, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {437, 305, 0, 0, 0}, {12.05, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {388, 305, 0, 0, 0}, {10.02, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {390, 305, 0, 0, 0}, {8.95, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {385, 305, 0, 0, 0}, {6.16, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {371, 305, 0, 0, 0}, {4.87, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {327, 305, 0, 0, 0}, {6.31, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {327, 305, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {327, 305, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {247, 0, 0, 0, 0}, {0.37, 0.00, 0.00, 0.00, 0.00}},
    {10.0, {247, 0, 0, 0, 0}, {10.50, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {247, 0, 0, 0, 0}, {20.36, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {247, 0, 0, 0, 0}, {19.65, 0.00, 0.00, 0.00, 0.00}},
    {11.0, {261, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {12.0, {303, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {49.0, {496, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {534, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {45.0, {1083, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {3.1, {1122, 0, 0, 0, 0}, {27.99, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1257, 0, 0, 0, 0}, {23.91, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1408, 0, 0, 0, 0}, {22.51, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1559, 0, 0, 0, 0}, {21.35, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1712, 0, 0, 0, 0}, {20.41, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1863, 0, 0, 0, 0}, {20.10, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2015, 0, 0, 0, 0}, {19.50, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2169, 0, 0, 0, 0}, {19.68, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2320, 0, 0, 0, 0}, {19.73, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2464, 0, 0, 0, 0}, {18.74, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2689, 0, 0, 0, 0}, {14.39, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2998, 0, 0, 0, 0}, {13.33, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3298, 0, 0, 0, 0}, {11.45, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3578, 0, 0, 0, 0}, {10.41, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3878, 0, 0, 0, 0}, {18.59, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3911, 0, 0, 0, 0}, {22.95, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {21.53, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {19.52, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {17.75, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {16.17, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {14.74, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3912, 0, 0, 0, 0}, {13.01, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {11.02, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3914, 0, 0, 0, 0}, {9.23, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3912, 0, 0, 0, 0}, {7.60, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3914, 0, 0, 0, 0}, {6.08, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3913, 0, 0, 0, 0}, {4.17, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3913, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {3913, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {644, 333, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {644, 333, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {644, 333, 0, 0, 0}, {1.67, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {641, 333, 0, 0, 0}, {1.67, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {641, 333, 0, 0, 0}, {6.38, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {641, 333, 0, 0, 0}, {12.67, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {636, 333, 0, 0, 0}, {18.94, 0.00, 0.00, 0.00, 0.00}},
    {173.0, {420, 333, 0, 0, 0}, {19.05, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {405, 333, 0, 0, 0}, {19.13, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {417, 333, 0, 0, 0}, {18.90, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {389, 333, 0, 0, 0}, {19.80, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {413, 333, 0, 0, 0}, {18.27, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {358, 333, 0, 0, 0}, {21.81, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {409, 333, 0, 0, 0}, {17.66, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {409, 333, 0, 0, 0}, {17.47, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {345, 333, 0, 0, 0}, {24.17, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {338, 333, 0, 0, 0}, {25.84, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {341, 333, 0, 0, 0}, {24.61, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {404, 333, 0, 0, 0}, {16.39, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {409, 333, 0, 0, 0}, {15.78, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {341, 333, 0, 0, 0}, {23.88, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {336, 333, 0, 0, 0}, {24.87, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {368, 333, 0, 0, 0}, {15.99, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {377, 333, 0, 0, 0}, {15.81, 0.00, 0.00, 0.00, 0.00}},
    {0.8, {410, 333, 0, 0, 0}, {15.24, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {410, 333, 0, 0, 0}, {0.00, 10.62, 0.00, 0.00, 0.00}},
    {0.8, {410, 333, 0, 0, 0}, {0.00, 10.02, 0.00, 0.00, 0.00}},
    {1.2, {410, 333, 0, 0, 0}, {0.00, 9.13, 0.00, 0.00, 0.00}},
    {5.0, {410, 331, 0, 0, 0}, {0.00, 25.97, 0.00, 0.00, 0.00}},
    {5.0, {410, 331, 0, 0, 0}, {0.00, 31.00, 0.00, 0.00, 0.00}},
    {12.0, {410, 331, 0, 0, 0}, {0.00, 15.08, 0.00, 0.00, 0.00}},
    {5.0, {410, 333, 0, 0, 0}, {0.00, 6.31, 0.00, 0.00, 0.00}},
    {1.0, {410, 393, 0, 0, 0}, {0.00, 4.61, 0.00, 0.00, 0.00}},
    {1.0, {410, 409, 0, 0, 0}, {0.00, 4.21, 0.00, 0.00, 0.00}},
    {2.0, {410, 390, 0, 0, 0}, {0.00, 5.41, 0.00, 0.00, 0.00}},
    {1.0, {410, 333, 0, 0, 0}, {0.00, 7.96, 0.00, 0.00, 0.00}},
    {6.0, {410, 331, 0, 0, 0}, {0.00, 26.45, 0.00, 0.00, 0.00}},
    {7.0, {410, 332, 0, 0, 0}, {0.00, 21.68, 0.00, 0.00, 0.00}},
    {1.0, {410, 334, 0, 0, 0}, {0.00, 16.97, 0.00, 0.00, 0.00}},
    {1.0, {410, 384, 0, 0, 0}, {0.00, 12.30, 0.00, 0.00, 0.00}},
    {2.0, {410, 411, 0, 0, 0}, {0.00, 10.39, 0.00, 0.00, 0.00}},
    {1.0, {410, 401, 0, 0, 0}, {0.00, 11.37, 0.00, 0.00, 0.00}},
    {2.0, {410, 333, 0, 0, 0}, {0.00, 19.58, 0.00, 0.00, 0.00}},
    {4.0, {410, 333, 0, 0, 0}, {0.00, 24.72, 0.00, 0.00, 0.00}},
    {1.0, {410, 335, 0, 0, 0}, {0.00, 20.98, 0.00, 0.00, 0.00}},
    {2.0, {410, 405, 0, 0, 0}, {0.00, 13.64, 0.00, 0.00, 0.00}},
    {1.0, {410, 413, 0, 0, 0}, {0.00, 13.07, 0.00, 0.00, 0.00}},
    {2.0, {410, 384, 0, 0, 0}, {0.00, 15.35, 0.00, 0.00, 0.00}},
    {1.0, {410, 338, 0, 0, 0}, {0.00, 19.51, 0.00, 0.00, 0.00}},
    {2.0, {410, 335, 0, 0, 0}, {0.00, 22.71, 0.00, 0.00, 0.00}},
    {1.0, {410, 342, 0, 0, 0}, {0.00, 20.17, 0.00, 0.00, 0.00}},
    {1.0, {410, 386, 0, 0, 0}, {0.00, 16.49, 0.00, 0.00, 0.00}},
    {1.0, {410, 409, 0, 0, 0}, {0.00, 14.96, 0.00, 0.00, 0.00}},
    {1.0, {410, 414, 0, 0, 0}, {0.00, 14.60, 0.00, 0.00, 0.00}},
    {2.0, {410, 388, 0, 0, 0}, {0.00, 16.25, 0.00, 0.00, 0.00}},
    {1.0, {410, 350, 0, 0, 0}, {0.00, 18.84, 0.00, 0.00, 0.00}},
    {1.0, {410, 348, 0, 0, 0}, {0.00, 19.63, 0.00, 0.00, 0.00}},
    {2.0, {410, 403, 0, 0, 0}, {0.00, 16.18, 0.00, 0.00, 0.00}},
    {1.0, {410, 414, 0, 0, 0}, {0.00, 15.63, 0.00, 0.00, 0.00}},
    {2.0, {410, 408, 0, 0, 0}, {0.00, 15.85, 0.00, 0.00, 0.00}},
    {2.0, {410, 376, 0, 0, 0}, {0.00, 17.51, 0.00, 0.00, 0.00}},
    {3.0, {410, 418, 0, 0, 0}, {0.00, 16.15, 0.00, 0.00, 0.00}},
    {3.0, {410, 403, 0, 0, 0}, {0.00, 16.28, 0.00, 0.00, 0.00}},
    {4.0, {410, 423, 0, 0, 0}, {0.00, 16.21, 0.00, 0.00, 0.00}},
    {180.4, {410, 628, 0, 0, 0}, {0.00, 14.44, 0.00, 0.00, 0.00}},
    {27.9, {410, 628, 0, 0, 0}, {0.00, 7.07, 0.00, 0.00, 0.00}},
    {16.9, {410, 628, 0, 0, 0}, {0.00, 2.45, 0.00, 0.00, 0.00}},
    {6.0, {410, 628, 0, 0, 0}, {0.00, 0.64, 0.00, 0.00, 0.00}},
    {2.0, {410, 628, 0, 0, 0}, {0.00, 0.23, 0.00, 0.00, 0.00}},
    {2.0, {410, 628, 0, 0, 0}, {0.00, 0.05, 0.00, 0.00, 0.00}},
    {1.0, {410, 628, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {410, 628, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {308, 262, 0, 0, 0}, {0.22, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {308, 262, 0, 0, 0}, {1.71, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {308, 262, 0, 0, 0}, {12.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {368, 262, 0, 0, 0}, {22.68, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {457, 262, 0, 0, 0}, {28.15, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {475, 262, 0, 0, 0}, {30.38, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {503, 262, 0, 0, 0}, {29.91, 21.41, 0.00, 0.00, 0.00}},
    {2.0, {561, 263, 0, 0, 0}, {29.03, 20.34, 0.00, 0.00, 0.00}},
    {5.0, {704, 304, 0, 0, 0}, {27.04, 30.57, 0.00, 0.00, 0.00}},
    {33.0, {1649, 662, 0, 0, 0}, {18.59, 30.66, 0.00, 0.00, 0.00}},
    {9.0, {1649, 759, 0, 0, 0}, {14.88, 30.87, 0.00, 0.00, 0.00}},
    {5.0, {1649, 799, 0, 0, 0}, {13.15, 31.00, 0.00, 0.00, 0.00}},
    {37.0, {1649, 802, 0, 0, 0}, {5.27, 30.87, 0.00, 0.00, 0.00}},
    {11.0, {1649, 803, 0, 0, 0}, {2.52, 30.84, 0.00, 0.00, 0.00}},
    {8.0, {1649, 804, 0, 0, 0}, {0.72, 30.81, 0.00, 0.00, 0.00}},
    {1.0, {1649, 804, 0, 0, 0}, {0.00, 30.81, 0.00, 0.00, 0.00}},
    {6.0, {1649, 804, 0, 0, 0}, {0.00, 30.79, 0.00, 0.00, 0.00}},
    {31.0, {1649, 804, 0, 0, 0}, {0.00, 15.09, 0.00, 0.00, 0.00}},
    {12.0, {1649, 804, 0, 0, 0}, {0.00, 8.37, 0.00, 0.00, 0.00}},
    {8.0, {1649, 804, 0, 0, 0}, {0.00, 3.78, 0.00, 0.00, 0.00}},
    {5.0, {1649, 804, 0, 0, 0}, {0.00, 1.35, 0.00, 0.00, 0.00}},
    {3.0, {1649, 804, 0, 0, 0}, {0.00, 0.32, 0.00, 0.00, 0.00}},
    {1.0, {1649, 804, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1649, 804, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1145, 418, 876, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1145, 418, 876, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1145, 418, 876, 0, 0}, {0.67, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1142, 418, 876, 0, 0}, {4.17, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1142, 418, 876, 0, 0}, {11.24, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1153, 418, 876, 0, 0}, {19.99, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1156, 418, 876, 0, 0}, {23.09, 0.48, 0.00, 0.00, 0.00}},
    {2.0, {1161, 418, 876, 0, 0}, {22.83, 0.41, 0.00, 0.00, 0.00}},
    {1.0, {1163, 418, 876, 0, 0}, {22.70, 0.50, 0.48, 0.00, 0.00}},
    {3.0, {1169, 418, 875, 0, 0}, {22.31, 0.97, 0.74, 0.00, 0.00}},
    {6.0, {1182, 418, 873, 0, 0}, {21.55, 1.86, 1.78, 0.00, 0.00}},
    {1.0, {1184, 418, 873, 0, 0}, {21.43, 2.07, 1.87, 0.00, 0.00}},
    {8.0, {1202, 418, 873, 0, 0}, {20.46, 2.93, 2.81, 0.00, 0.00}},
    {8.0, {1219, 418, 873, 0, 0}, {19.55, 4.15, 3.36, 0.00, 0.00}},
    {44.0, {1314, 418, 874, 0, 0}, {15.21, 9.86, 9.01, 0.00, 0.00}},
    {9.0, {1333, 418, 894, 0, 0}, {14.45, 11.77, 9.54, 0.00, 0.00}},
    {60.0, {1462, 458, 1027, 0, 0}, {10.32, 17.55, 13.88, 0.00, 0.00}},
    {58.0, {1463, 497, 1156, 0, 0}, {5.28, 25.83, 20.00, 0.00, 0.00}},
    {27.0, {1463, 515, 1215, 0, 0}, {2.33, 30.93, 23.72, 0.00, 0.00}},
    {1.0, {1463, 516, 1218, 0, 0}, {2.26, 30.93, 23.88, 0.00, 0.00}},
    {5.0, {1463, 524, 1229, 0, 0}, {2.32, 30.93, 24.65, 0.00, 0.00}},
    {29.0, {1463, 567, 1293, 0, 0}, {2.67, 30.94, 24.17, 0.00, 0.00}},
    {16.0, {1463, 590, 1328, 0, 0}, {2.06, 30.95, 23.92, 0.00, 0.00}},
    {10.0, {1463, 605, 1350, 0, 0}, {1.11, 30.96, 23.77, 0.00, 0.00}},
    {22.0, {1463, 638, 1398, 0, 0}, {0.90, 30.99, 23.47, 0.00, 0.00}},
    {4.0, {1463, 644, 1407, 0, 0}, {1.08, 31.00, 23.41, 0.00, 0.00}},
    {3.0, {1463, 648, 1413, 0, 0}, {1.24, 30.95, 23.37, 0.00, 0.00}},
    {11.0, {1463, 664, 1438, 0, 0}, {0.88, 30.75, 23.23, 0.00, 0.00}},
    {6.0, {1463, 673, 1456, 0, 0}, {0.73, 30.64, 23.15, 0.00, 0.00}},
    {13.0, {1463, 692, 1494, 0, 0}, {1.08, 30.42, 22.99, 0.00, 0.00}},
    {1.0, {1463, 694, 1496, 0, 0}, {1.11, 30.40, 22.98, 0.00, 0.00}},
    {14.0, {1463, 715, 1524, 0, 0}, {0.70, 30.16, 22.84, 0.00, 0.00}},
    {26.0, {1463, 753, 1574, 0, 0}, {1.01, 29.72, 22.60, 0.00, 0.00}},
    {11.0, {1463, 770, 1596, 0, 0}, {0.65, 29.54, 22.51, 0.00, 0.00}},
    {1.0, {1456, 771, 1598, 0, 0}, {0.85, 29.53, 22.50, 0.00, 0.00}},
    {1.0, {1456, 772, 1600, 0, 0}, {0.00, 29.51, 22.49, 0.00, 0.00}},
    {4.0, {1456, 778, 1608, 0, 0}, {0.00, 29.44, 22.46, 0.00, 0.00}},
    {15.0, {1456, 801, 1658, 0, 0}, {0.00, 29.20, 22.24, 0.00, 0.00}},
    {9.0, {1456, 814, 1659, 0, 0}, {0.00, 29.06, 22.10, 0.00, 0.00}},
    {5.0, {1456, 821, 1690, 0, 0}, {0.00, 28.98, 22.01, 0.00, 0.00}},
    {9.0, {1456, 835, 1694, 0, 0}, {0.00, 28.84, 22.08, 0.00, 0.00}},
    {16.0, {1456, 858, 1748, 0, 0}, {0.00, 28.59, 21.93, 0.00, 0.00}},
    {6.0, {1456, 867, 1748, 0, 0}, {0.00, 28.50, 21.90, 0.00, 0.00}},
    {42.0, {1456, 919, 1816, 0, 0}, {0.00, 16.03, 13.46, 0.00, 0.00}},
    {3.0, {1456, 920, 1821, 0, 0}, {0.00, 14.48, 13.00, 0.00, 0.00}},
    {35.0, {1456, 928, 1821, 0, 0}, {0.00, 4.39, 5.13, 0.00, 0.00}},
    {8.0, {1456, 928, 1821, 0, 0}, {0.00, 2.03, 4.15, 0.00, 0.00}},
    {1.0, {1456, 928, 1821, 0, 0}, {0.00, 1.84, 3.91, 0.00, 0.00}},
    {8.0, {1456, 928, 1821, 0, 0}, {0.00, 0.33, 2.46, 0.00, 0.00}},
    {1.0, {1456, 928, 1821, 0, 0}, {0.00, 0.00, 2.32, 0.00, 0.00}},
    {4.0, {1456, 928, 1821, 0, 0}, {0.00, 0.00, 1.84, 0.00, 0.00}},
    {3.0, {1456, 928, 1821, 0, 0}, {0.00, 0.00, 1.01, 0.00, 0.00}},
    {4.0, {1456, 928, 1815, 0, 0}, {0.00, 0.00, 0.22, 0.00, 0.00}},
    {1.0, {1456, 928, 1815, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1456, 928, 1815, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {348, 289, 0, 0, 0}, {0.16, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {348, 289, 0, 0, 0}, {0.98, 0.00, 0.00, 0.00, 0.00}},
    {9.0, {348, 289, 0, 0, 0}, {6.76, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {348, 289, 0, 0, 0}, {10.80, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {389, 289, 0, 0, 0}, {11.75, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {423, 289, 0, 0, 0}, {14.24, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {484, 289, 0, 0, 0}, {15.53, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {500, 289, 0, 0, 0}, {15.87, 11.06, 0.00, 0.00, 0.00}},
    {5.0, {577, 290, 0, 0, 0}, {17.71, 13.80, 0.00, 0.00, 0.00}},
    {23.0, {931, 449, 0, 0, 0}, {29.84, 24.49, 0.00, 0.00, 0.00}},
    {6.0, {999, 491, 0, 0, 0}, {31.00, 28.49, 0.00, 0.00, 0.00}},
    {1.0, {999, 498, 0, 0, 0}, {30.38, 29.22, 0.00, 0.00, 0.00}},
    {22.0, {1003, 501, 0, 0, 0}, {19.48, 17.99, 0.00, 0.00, 0.00}},
    {3.0, {1003, 502, 0, 0, 0}, {17.19, 16.84, 0.00, 0.00, 0.00}},
    {21.0, {1003, 501, 0, 0, 0}, {7.18, 6.86, 0.00, 0.00, 0.00}},
    {6.0, {1003, 501, 0, 0, 0}, {5.59, 3.93, 0.00, 0.00, 0.00}},
    {6.0, {1003, 501, 0, 0, 0}, {2.54, 1.19, 0.00, 0.00, 0.00}},
    {1.0, {1003, 544, 0, 0, 0}, {2.23, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1003, 544, 0, 0, 0}, {1.50, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1003, 544, 0, 0, 0}, {0.33, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {284, 544, 0, 0, 0}, {0.00, 0.15, 0.00, 0.00, 0.00}},
    {3.0, {284, 544, 0, 0, 0}, {0.00, 0.06, 0.00, 0.00, 0.00}},
    {10.0, {284, 544, 0, 0, 0}, {0.00, 0.07, 0.00, 0.00, 0.00}},
    {2.0, {284, 544, 0, 0, 0}, {0.00, 0.11, 0.00, 0.00, 0.00}},
    {7.0, {284, 544, 0, 0, 0}, {0.00, 1.77, 0.00, 0.00, 0.00}},
    {8.0, {284, 544, 0, 0, 0}, {0.00, 5.53, 0.00, 0.00, 0.00}},
    {1.0, {284, 575, 0, 0, 0}, {0.00, 5.78, 0.00, 0.00, 0.00}},
    {1.0, {284, 606, 0, 0, 0}, {0.44, 6.06, 0.00, 0.00, 0.00}},
    {10.0, {280, 916, 0, 0, 0}, {4.00, 9.66, 0.00, 0.00, 0.00}},
    {7.0, {280, 1134, 0, 0, 0}, {11.72, 13.49, 0.00, 0.00, 0.00}},
    {2.0, {294, 1196, 0, 0, 0}, {13.52, 14.85, 0.00, 0.00, 0.00}},
    {6.0, {336, 1383, 0, 0, 0}, {20.75, 16.11, 0.00, 0.00, 0.00}},
    {17.0, {461, 1913, 0, 0, 0}, {27.33, 22.10, 0.00, 0.00, 0.00}},
    {2.0, {476, 1966, 0, 0, 0}, {28.24, 23.70, 0.00, 0.00, 0.00}},
    {2.0, {491, 1997, 0, 0, 0}, {29.18, 25.07, 0.00, 0.00, 0.00}},
    {1.0, {498, 2001, 0, 0, 0}, {29.66, 24.91, 0.00, 0.00, 0.00}},
    {2.0, {499, 2009, 0, 0, 0}, {27.84, 24.58, 0.00, 0.00, 0.00}},
    {26.0, {500, 2009, 0, 0, 0}, {12.22, 12.76, 0.00, 0.00, 0.00}},
    {19.0, {501, 2008, 0, 0, 0}, {6.70, 5.25, 0.00, 0.00, 0.00}},
    {1.0, {501, 2008, 0, 0, 0}, {5.85, 5.01, 0.00, 0.00, 0.00}},
    {7.0, {501, 2008, 0, 0, 0}, {2.26, 1.48, 0.00, 0.00, 0.00}},
    {1.0, {501, 2008, 0, 0, 0}, {1.59, 1.24, 0.00, 0.00, 0.00}},
    {2.0, {501, 2008, 0, 0, 0}, {0.80, 0.52, 0.00, 0.00, 0.00}},
    {1.0, {502, 2008, 0, 0, 0}, {0.39, 0.33, 0.00, 0.00, 0.00}},
    {1.0, {502, 2008, 0, 0, 0}, {0.19, 0.18, 0.00, 0.00, 0.00}},
    {1.0, {502, 2008, 0, 0, 0}, {0.00, 0.09, 0.00, 0.00, 0.00}},
    {1.0, {502, 2008, 0, 0, 0}, {0.00, 0.05, 0.00, 0.00, 0.00}},
    {1.0, {502, 2008, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {502, 2008, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1303, 2431, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1303, 2431, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1303, 2431, 0, 0, 0}, {0.88, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1302, 2431, 0, 0, 0}, {1.79, 1.29, 0.00, 0.00, 0.00}},
    {1.0, {1302, 2449, 0, 0, 0}, {3.62, 3.67, 0.00, 0.00, 0.00}},
    {1.0, {1301, 2449, 0, 0, 0}, {7.35, 5.76, 0.00, 0.00, 0.00}},
    {1.0, {1301, 2449, 0, 0, 0}, {9.31, 9.02, 0.00, 0.00, 0.00}},
    {3.0, {1301, 2449, 0, 0, 0}, {18.90, 14.36, 0.00, 0.00, 0.00}},
    {1.0, {1301, 2449, 0, 0, 0}, {18.93, 16.76, 0.00, 0.00, 0.00}},
    {45.0, {1295, 2449, 0, 0, 0}, {20.68, 18.19, 0.00, 0.00, 0.00}},
    {20.0, {1169, 2449, 0, 0, 0}, {21.57, 18.87, 0.00, 0.00, 0.00}},
    {5.0, {1137, 2431, 0, 0, 0}, {21.82, 19.13, 0.00, 0.00, 0.00}},
    {132.0, {304, 1531, 0, 0, 0}, {31.00, 21.28, 0.00, 0.00, 0.00}},
    {12.0, {252, 1449, 0, 0, 0}, {12.16, 21.91, 0.00, 0.00, 0.00}},
    {14.0, {251, 1354, 0, 0, 0}, {0.67, 22.76, 0.00, 0.00, 0.00}},
    {1.0, {251, 1347, 0, 0, 0}, {0.00, 22.83, 0.00, 0.00, 0.00}},
    {2.0, {251, 1334, 0, 0, 0}, {0.00, 22.96, 0.00, 0.00, 0.00}},
    {5.0, {251, 1308, 0, 0, 0}, {0.00, 10.28, 0.00, 0.00, 0.00}},
    {2.0, {251, 1308, 0, 0, 0}, {0.00, 3.93, 0.00, 0.00, 0.00}},
    {3.0, {251, 1308, 0, 0, 0}, {0.00, 0.46, 0.00, 0.00, 0.00}},
    {1.0, {251, 1308, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {251, 1308, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {2603, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2603, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2603, 0, 0, 0, 0}, {1.02, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2597, 0, 0, 0, 0}, {3.97, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2597, 0, 0, 0, 0}, {12.47, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2602, 0, 0, 0, 0}, {22.56, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2606, 0, 0, 0, 0}, {30.76, 0.00, 0.00, 0.00, 0.00}},
    {104.0, {2607, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {29.0, {2605, 0, 0, 0, 0}, {15.71, 0.00, 0.00, 0.00, 0.00}},
    {17.0, {2605, 0, 0, 0, 0}, {6.63, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {2605, 0, 0, 0, 0}, {2.08, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {2605, 0, 0, 0, 0}, {0.31, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2605, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2605, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1421, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1421, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1421, 0, 0, 0, 0}, {1.69, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1416, 0, 0, 0, 0}, {15.65, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1473, 0, 0, 0, 0}, {29.09, 0.00, 0.00, 0.00, 0.00}},
    {87.0, {2986, 0, 0, 0, 0}, {16.04, 0.00, 0.00, 0.00, 0.00}},
    {49.0, {3837, 0, 0, 0, 0}, {8.91, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {3868, 0, 0, 0, 0}, {9.53, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {3879, 0, 0, 0, 0}, {9.96, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3822, 0, 0, 0, 0}, {8.75, 0.00, 0.00, 0.00, 0.00}},
    {35.0, {3070, 0, 0, 0, 0}, {14.74, 0.00, 0.00, 0.00, 0.00}},
    {71.0, {1546, 0, 0, 0, 0}, {27.52, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1443, 0, 0, 0, 0}, {29.51, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1418, 0, 0, 0, 0}, {30.74, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1407, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {61.0, {1407, 0, 0, 0, 0}, {15.11, 0.00, 0.00, 0.00, 0.00}},
    {32.0, {1407, 0, 0, 0, 0}, {6.24, 0.00, 0.00, 0.00, 0.00}},
    {10.0, {1407, 0, 0, 0, 0}, {3.50, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1407, 0, 0, 0, 0}, {1.53, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1407, 0, 0, 0, 0}, {0.51, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1407, 0, 0, 0, 0}, {0.11, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1407, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1407, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1406, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1406, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1406, 260, 0, 0, 0}, {1.17, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1406, 260, 0, 0, 0}, {3.34, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1406, 260, 0, 0, 0}, {11.39, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1409, 260, 0, 0, 0}, {21.47, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1412, 260, 0, 0, 0}, {30.32, 0.00, 0.00, 0.00, 0.00}},
    {108.0, {1406, 260, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1414, 260, 0, 0, 0}, {30.18, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1443, 260, 0, 0, 0}, {26.56, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1471, 260, 0, 0, 0}, {23.98, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1562, 260, 0, 0, 0}, {16.52, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1869, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1869, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1869, 260, 0, 0, 0}, {24.59, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {2266, 260, 0, 0, 0}, {25.25, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2395, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2395, 260, 0, 0, 0}, {26.78, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2468, 260, 0, 0, 0}, {26.16, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2507, 260, 0, 0, 0}, {30.23, 0.00, 0.00, 0.00, 0.00}},
    {13.0, {2508, 260, 0, 0, 0}, {30.07, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2487, 260, 0, 0, 0}, {22.17, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2461, 260, 0, 0, 0}, {17.31, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1767, 260, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1767, 260, 0, 0, 0}, {16.98, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1719, 260, 0, 0, 0}, {21.73, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1707, 260, 0, 0, 0}, {29.06, 0.00, 0.00, 0.00, 0.00}},
    {33.0, {1707, 260, 0, 0, 0}, {29.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1707, 260, 0, 0, 0}, {29.00, 0.30, 0.00, 0.00, 0.00}},
    {2.0, {1707, 257, 0, 0, 0}, {29.00, 0.32, 0.00, 0.00, 0.00}},
    {9.0, {1707, 259, 0, 0, 0}, {28.98, 2.51, 0.00, 0.00, 0.00}},
    {7.0, {1707, 298, 0, 0, 0}, {28.97, 5.09, 0.00, 0.00, 0.00}},
    {33.0, {1707, 573, 0, 0, 0}, {28.91, 5.19, 0.00, 0.00, 0.00}},
    {5.0, {1707, 601, 0, 0, 0}, {28.90, 5.10, 0.00, 0.00, 0.00}},
    {21.0, {1707, 601, 0, 0, 0}, {28.87, 4.12, 0.00, 0.00, 0.00}},
    {68.0, {1708, 603, 0, 0, 0}, {12.02, 2.07, 0.00, 0.00, 0.00}},
    {3.0, {1708, 603, 0, 0, 0}, {11.03, 2.01, 0.00, 0.00, 0.00}},
    {15.0, {1708, 603, 0, 0, 0}, {7.19, 1.22, 0.00, 0.00, 0.00}},
    {5.0, {1708, 603, 0, 0, 0}, {6.23, 0.79, 0.00, 0.00, 0.00}},
    {2.0, {1708, 603, 0, 0, 0}, {5.52, 0.66, 0.00, 0.00, 0.00}},
    {7.0, {1708, 603, 0, 0, 0}, {3.59, 0.63, 0.00, 0.00, 0.00}},
    {3.0, {1708, 603, 0, 0, 0}, {2.99, 0.44, 0.00, 0.00, 0.00}},
    {2.0, {1708, 603, 0, 0, 0}, {2.65, 0.25, 0.00, 0.00, 0.00}},
    {2.0, {1708, 603, 0, 0, 0}, {1.91, 0.15, 0.00, 0.00, 0.00}},
    {2.0, {1708, 600, 0, 0, 0}, {1.38, 0.17, 0.00, 0.00, 0.00}},
    {1.0, {1708, 600, 0, 0, 0}, {1.18, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1708, 600, 0, 0, 0}, {0.85, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1709, 600, 0, 0, 0}, {0.10, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1709, 600, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1709, 600, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {3103, 615, 1416, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3103, 615, 1416, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3103, 615, 1416, 0, 0}, {1.30, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3102, 615, 1416, 0, 0}, {2.81, 1.30, 0.00, 0.00, 0.00}},
    {1.0, {3100, 612, 1416, 0, 0}, {6.04, 1.89, 0.00, 0.00, 0.00}},
    {2.0, {3103, 605, 1416, 0, 0}, {12.93, 3.95, 0.00, 0.00, 0.00}},
    {1.0, {3105, 602, 1416, 0, 0}, {14.29, 5.73, 0.00, 0.00, 0.00}},
    {4.0, {3114, 596, 1416, 0, 0}, {21.35, 8.15, 0.00, 0.00, 0.00}},
    {98.0, {3112, 599, 1416, 0, 0}, {20.72, 8.15, 0.00, 0.00, 0.00}},
    {1.0, {3104, 599, 1416, 0, 0}, {17.08, 8.15, 0.67, 0.00, 0.00}},
    {3.0, {3083, 599, 1422, 0, 0}, {9.57, 8.15, 6.19, 0.00, 0.00}},
    {1.0, {3076, 599, 1418, 0, 0}, {7.88, 8.15, 9.20, 0.00, 0.00}},
    {1.0, {3076, 599, 1415, 0, 0}, {5.01, 8.15, 13.68, 0.00, 0.00}},
    {1.0, {3075, 599, 1412, 0, 0}, {1.54, 8.15, 20.35, 0.00, 0.00}},
    {1.0, {3075, 599, 1409, 0, 0}, {0.00, 8.15, 23.14, 0.00, 0.00}},
    {2.0, {3075, 599, 1404, 0, 0}, {0.00, 8.15, 29.94, 0.00, 0.00}},
    {9.0, {3075, 599, 1404, 0, 0}, {0.00, 8.15, 31.00, 0.00, 0.00}},
    {99.0, {3075, 602, 1404, 0, 0}, {0.00, 8.15, 30.83, 0.00, 0.00}},
    {3.0, {3075, 602, 1404, 0, 0}, {0.00, 7.73, 30.82, 0.00, 0.00}},
    {31.0, {3075, 601, 1403, 0, 0}, {0.00, 4.46, 18.58, 0.00, 0.00}},
    {5.0, {3075, 600, 1403, 0, 0}, {0.00, 4.08, 15.24, 0.00, 0.00}},
    {19.0, {3075, 604, 1403, 0, 0}, {0.00, 1.89, 7.17, 0.00, 0.00}},
    {8.0, {3075, 604, 1403, 0, 0}, {0.00, 0.94, 5.22, 0.00, 0.00}},
    {2.0, {3075, 604, 1403, 0, 0}, {0.00, 0.79, 3.99, 0.00, 0.00}},
    {5.0, {3075, 602, 1403, 0, 0}, {0.00, 0.25, 2.03, 0.00, 0.00}},
    {1.0, {3075, 602, 1403, 0, 0}, {0.00, 0.00, 1.51, 0.00, 0.00}},
    {3.0, {3075, 602, 1403, 0, 0}, {0.00, 0.00, 0.63, 0.00, 0.00}},
    {1.0, {3075, 602, 1404, 0, 0}, {0.00, 0.00, 0.70, 0.00, 0.00}},
    {1.0, {3075, 602, 1404, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {3075, 602, 1404, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1828, 1620, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1828, 1620, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1828, 1620, 0, 0, 0}, {2.16, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1828, 1620, 0, 0, 0}, {8.65, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1828, 1620, 0, 0, 0}, {16.32, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1823, 1620, 0, 0, 0}, {24.15, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1855, 1620, 0, 0, 0}, {30.20, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1825, 1620, 0, 0, 0}, {19.76, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1835, 1620, 0, 0, 0}, {16.78, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1844, 1620, 0, 0, 0}, {18.82, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1805, 1620, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1841, 1620, 0, 0, 0}, {24.95, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1829, 1620, 0, 0, 0}, {25.50, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1829, 1620, 0, 0, 0}, {25.50, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1829, 1620, 0, 0, 0}, {25.50, 15.24, 0.00, 0.00, 0.00}},
    {106.0, {1834, 357, 0, 0, 0}, {25.31, 21.67, 0.00, 0.00, 0.00}},
    {5.0, {1834, 325, 0, 0, 0}, {25.30, 21.15, 0.00, 0.00, 0.00}},
    {4.0, {1834, 353, 0, 0, 0}, {25.29, 21.63, 0.00, 0.00, 0.00}},
    {104.0, {1839, 1574, 0, 0, 0}, {25.10, 14.87, 0.00, 0.00, 0.00}},
    {4.0, {1839, 1617, 0, 0, 0}, {25.09, 19.19, 0.00, 0.00, 0.00}},
    {1.0, {1839, 1617, 0, 0, 0}, {25.09, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1839, 1617, 0, 0, 0}, {25.09, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1805, 1617, 0, 0, 0}, {29.51, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1853, 1617, 0, 0, 0}, {16.82, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {1807, 1617, 0, 0, 0}, {14.98, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1858, 1617, 0, 0, 0}, {30.74, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1825, 1617, 0, 0, 0}, {25.39, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1838, 1617, 0, 0, 0}, {25.34, 0.00, 0.00, 0.00, 0.00}},
    {31.0, {1832, 1617, 0, 0, 0}, {15.28, 0.00, 0.00, 0.00, 0.00}},
    {28.0, {1834, 1617, 0, 0, 0}, {5.56, 0.00, 0.00, 0.00, 0.00}},
    {11.0, {1834, 1617, 0, 0, 0}, {1.66, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1834, 1617, 0, 0, 0}, {0.76, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {1834, 1617, 0, 0, 0}, {0.29, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1839, 1617, 0, 0, 0}, {0.27, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1839, 1617, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1839, 1617, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {982, 3508, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {982, 3508, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {982, 3508, 0, 0, 0}, {1.45, 0.56, 0.00, 0.00, 0.00}},
    {1.0, {981, 3502, 0, 0, 0}, {2.77, 2.12, 0.00, 0.00, 0.00}},
    {2.0, {979, 3502, 0, 0, 0}, {10.13, 6.67, 0.00, 0.00, 0.00}},
    {2.0, {977, 3506, 0, 0, 0}, {18.58, 11.02, 0.00, 0.00, 0.00}},
    {1.0, {973, 3508, 0, 0, 0}, {20.69, 14.16, 0.00, 0.00, 0.00}},
    {2.0, {965, 3510, 0, 0, 0}, {25.65, 15.72, 0.00, 0.00, 0.00}},
    {1.0, {956, 3511, 0, 0, 0}, {25.71, 16.56, 0.00, 0.00, 0.00}},
    {65.0, {320, 3513, 0, 0, 0}, {31.00, 16.18, 0.00, 0.00, 0.00}},
    {9.0, {260, 3514, 0, 0, 0}, {13.75, 16.12, 0.00, 0.00, 0.00}},
    {16.0, {258, 3514, 0, 0, 0}, {0.33, 16.03, 0.00, 0.00, 0.00}},
    {1.0, {258, 3514, 0, 0, 0}, {0.00, 16.03, 0.00, 0.00, 0.00}},
    {6.0, {258, 3514, 0, 0, 0}, {0.00, 15.99, 0.00, 0.00, 0.00}},
    {34.0, {258, 3516, 0, 0, 0}, {0.00, 6.12, 0.00, 0.00, 0.00}},
    {18.0, {258, 3517, 0, 0, 0}, {0.00, 1.32, 0.00, 0.00, 0.00}},
    {1.0, {258, 3517, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {258, 3517, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {3503, 1821, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3503, 1821, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3503, 1821, 0, 0, 0}, {1.41, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {3495, 1821, 0, 0, 0}, {7.11, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {3507, 1821, 0, 0, 0}, {22.97, 0.00, 0.00, 0.00, 0.00}},
    {14.0, {3506, 1821, 0, 0, 0}, {24.65, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3500, 1821, 0, 0, 0}, {20.49, 1.01, 0.00, 0.00, 0.00}},
    {2.0, {3486, 1821, 0, 0, 0}, {14.16, 4.47, 0.00, 0.00, 0.00}},
    {1.0, {3452, 1821, 0, 0, 0}, {13.05, 9.41, 0.00, 0.00, 0.00}},
    {1.0, {3493, 1817, 0, 0, 0}, {0.00, 12.47, 0.00, 0.00, 0.00}},
    {3.0, {3493, 1805, 0, 0, 0}, {0.00, 29.04, 0.00, 0.00, 0.00}},
    {15.0, {3493, 1804, 0, 0, 0}, {0.00, 30.86, 0.00, 0.00, 0.00}},
    {2.0, {3493, 1804, 0, 0, 0}, {0.00, 19.87, 0.00, 0.00, 0.00}},
    {1.0, {3493, 1803, 0, 0, 0}, {11.54, 15.94, 0.00, 0.00, 0.00}},
    {2.0, {3501, 1803, 0, 0, 0}, {17.05, 10.26, 0.00, 0.00, 0.00}},
    {1.0, {3506, 1803, 0, 0, 0}, {20.73, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3510, 1803, 0, 0, 0}, {25.20, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {3513, 1803, 0, 0, 0}, {26.47, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3512, 1803, 0, 0, 0}, {25.20, 0.47, 0.00, 0.00, 0.00}},
    {1.0, {3511, 1802, 0, 0, 0}, {23.99, 1.62, 0.00, 0.00, 0.00}},
    {1.0, {3509, 1802, 0, 0, 0}, {22.84, 3.61, 0.00, 0.00, 0.00}},
    {1.0, {3505, 1802, 0, 0, 0}, {18.34, 8.04, 0.00, 0.00, 0.00}},
    {2.0, {3495, 1802, 0, 0, 0}, {11.82, 18.57, 0.00, 0.00, 0.00}},
    {1.0, {3491, 1802, 0, 0, 0}, {9.49, 22.02, 0.00, 0.00, 0.00}},
    {1.0, {3469, 1803, 0, 0, 0}, {0.00, 26.11, 0.00, 0.00, 0.00}},
    {1.0, {3469, 1803, 0, 0, 0}, {0.00, 30.96, 0.00, 0.00, 0.00}},
    {10.0, {3469, 1809, 0, 0, 0}, {0.00, 25.78, 0.00, 0.00, 0.00}},
    {2.0, {3469, 1817, 0, 0, 0}, {0.00, 13.98, 0.00, 0.00, 0.00}},
    {1.0, {3469, 1821, 0, 0, 0}, {12.74, 10.30, 0.00, 0.00, 0.00}},
    {2.0, {3497, 1823, 0, 0, 0}, {18.34, 2.98, 0.00, 0.00, 0.00}},
    {1.0, {3500, 1824, 0, 0, 0}, {19.98, 1.61, 0.00, 0.00, 0.00}},
    {1.0, {3503, 1822, 0, 0, 0}, {21.77, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {3509, 1822, 0, 0, 0}, {25.86, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3503, 1822, 0, 0, 0}, {22.12, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3498, 1822, 0, 0, 0}, {18.93, 1.17, 0.00, 0.00, 0.00}},
    {2.0, {3488, 1822, 0, 0, 0}, {13.86, 4.96, 0.00, 0.00, 0.00}},
    {1.0, {3463, 1822, 0, 0, 0}, {13.13, 10.23, 0.00, 0.00, 0.00}},
    {1.0, {3463, 1817, 0, 0, 0}, {0.00, 13.90, 0.00, 0.00, 0.00}},
    {2.0, {3463, 1808, 0, 0, 0}, {0.00, 25.65, 0.00, 0.00, 0.00}},
    {15.0, {3463, 1804, 0, 0, 0}, {0.00, 31.00, 0.00, 0.00, 0.00}},
    {26.0, {3463, 1805, 0, 0, 0}, {0.00, 15.96, 0.00, 0.00, 0.00}},
    {16.0, {3463, 1805, 0, 0, 0}, {0.00, 6.83, 0.00, 0.00, 0.00}},
    {8.0, {3463, 1805, 0, 0, 0}, {0.00, 2.03, 0.00, 0.00, 0.00}},
    {5.0, {3463, 1805, 0, 0, 0}, {0.00, 0.33, 0.00, 0.00, 0.00}},
    {1.0, {3463, 1805, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {3463, 1805, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {2908, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2908, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2908, 0, 0, 0, 0}, {1.74, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2910, 0, 0, 0, 0}, {11.64, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {2914, 0, 0, 0, 0}, {29.00, 0.00, 0.00, 0.00, 0.00}},
    {31.0, {2912, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2910, 0, 0, 0, 0}, {25.56, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2907, 0, 0, 0, 0}, {12.20, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2907, 0, 0, 0, 0}, {4.34, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2907, 0, 0, 0, 0}, {0.70, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3884, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {9.0, {3884, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3884, 0, 0, 0, 0}, {1.15, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3903, 0, 0, 0, 0}, {4.60, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {3897, 0, 0, 0, 0}, {13.53, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {3918, 0, 0, 0, 0}, {28.13, 0.00, 0.00, 0.00, 0.00}},
    {33.0, {3916, 0, 0, 0, 0}, {27.42, 0.00, 0.00, 0.00, 0.00}},
    {29.0, {3918, 0, 0, 0, 0}, {13.85, 0.00, 0.00, 0.00, 0.00}},
    {15.0, {3916, 0, 0, 0, 0}, {6.25, 0.00, 0.00, 0.00, 0.00}},
    {9.0, {3917, 0, 0, 0, 0}, {2.02, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {3917, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {3917, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {2085, 3618, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2085, 3618, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2085, 3618, 0, 0, 0}, {0.84, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {2086, 3618, 0, 0, 0}, {2.40, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2086, 3618, 0, 0, 0}, {9.29, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {2086, 3618, 0, 0, 0}, {18.76, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {2082, 3618, 0, 0, 0}, {28.04, 0.00, 0.00, 0.00, 0.00}},
    {24.0, {1961, 3618, 0, 0, 0}, {28.72, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1956, 3618, 0, 0, 0}, {28.75, 4.55, 0.00, 0.00, 0.00}},
    {2.0, {1946, 3621, 0, 0, 0}, {28.82, 9.15, 0.00, 0.00, 0.00}},
    {3.0, {1925, 3621, 0, 0, 0}, {28.76, 14.01, 0.00, 0.00, 0.00}},
    {17.0, {1808, 3619, 0, 0, 0}, {28.55, 22.37, 0.00, 0.00, 0.00}},
    {8.0, {1752, 3501, 0, 0, 0}, {28.53, 23.14, 0.00, 0.00, 0.00}},
    {23.0, {1654, 3280, 0, 0, 0}, {29.03, 24.86, 0.00, 0.00, 0.00}},
    {35.0, {1505, 3005, 0, 0, 0}, {30.12, 28.94, 0.00, 0.00, 0.00}},
    {1.0, {1501, 3001, 0, 0, 0}, {30.15, 28.94, 0.00, 0.00, 0.00}},
    {76.0, {1330, 2709, 0, 0, 0}, {31.00, 28.72, 0.00, 0.00, 0.00}},
    {6.0, {1325, 2686, 0, 0, 0}, {29.86, 28.62, 0.00, 0.00, 0.00}},
    {23.0, {1305, 2611, 0, 0, 0}, {25.88, 24.51, 0.00, 0.00, 0.00}},
    {39.0, {1305, 2609, 0, 0, 0}, {24.36, 24.54, 0.00, 0.00, 0.00}},
    {5.0, {1305, 2610, 0, 0, 0}, {24.17, 22.01, 0.00, 0.00, 0.00}},
    {10.0, {1305, 2611, 0, 0, 0}, {15.99, 17.71, 0.00, 0.00, 0.00}},
    {21.0, {1305, 2612, 0, 0, 0}, {6.71, 6.24, 0.00, 0.00, 0.00}},
    {7.0, {1305, 2612, 0, 0, 0}, {2.79, 2.47, 0.00, 0.00, 0.00}},
    {3.0, {1305, 2612, 0, 0, 0}, {1.24, 0.97, 0.00, 0.00, 0.00}},
    {1.0, {1305, 2612, 0, 0, 0}, {0.77, 0.71, 0.00, 0.00, 0.00}},
    {1.0, {1305, 2613, 0, 0, 0}, {0.49, 0.82, 0.00, 0.00, 0.00}},
    {1.0, {1306, 2613, 0, 0, 0}, {0.49, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1306, 2613, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1306, 2613, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {716, 2705, 302, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {716, 2705, 302, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {716, 2705, 302, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {716, 2705, 302, 0, 0}, {1.28, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {716, 2705, 302, 0, 0}, {1.28, 8.05, 0.00, 0.00, 0.00}},
    {1.0, {716, 2708, 302, 0, 0}, {1.49, 3.89, 0.00, 0.00, 0.00}},
    {3.0, {716, 2710, 302, 0, 0}, {2.33, 9.25, 0.00, 0.00, 0.00}},
    {1.0, {716, 2710, 302, 0, 0}, {2.68, 9.99, 0.00, 0.00, 0.00}},
    {3.0, {716, 2710, 302, 0, 0}, {2.68, 12.78, 0.00, 0.00, 0.00}},
    {31.0, {716, 2709, 302, 0, 0}, {2.67, 12.87, 0.00, 0.00, 0.00}},
    {1.8, {716, 2701, 302, 0, 0}, {2.66, 12.40, 0.00, 0.00, 0.00}},
    {1.2, {716, 2696, 302, 0, 0}, {2.94, 12.11, 0.00, 0.00, 0.00}},
    {2.0, {716, 2668, 302, 0, 0}, {3.49, 10.62, 0.00, 0.00, 0.00}},
    {1.0, {716, 2708, 302, 0, 0}, {3.80, 15.95, 0.00, 0.00, 0.00}},
    {1.0, {716, 901, 302, 0, 0}, {4.13, 0.00, 0.00, 0.00, 0.00}},
    {9.8, {716, 901, 302, 0, 0}, {9.48, 0.00, 0.00, 0.00, 0.00}},
    {14.2, {640, 901, 302, 0, 0}, {16.96, 0.00, 0.00, 0.00, 0.00}},
    {0.7, {636, 901, 302, 0, 0}, {17.48, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {636, 901, 302, 0, 0}, {17.56, 13.06, 0.00, 0.00, 0.00}},
    {5.0, {620, 902, 302, 0, 0}, {19.12, 16.49, 0.00, 0.00, 0.00}},
    {1.0, {617, 902, 302, 0, 0}, {19.45, 17.28, 2.54, 0.00, 0.00}},
    {4.0, {605, 903, 301, 0, 0}, {20.82, 20.83, 3.77, 0.00, 0.00}},
    {2.7, {597, 903, 301, 0, 0}, {21.79, 20.25, 4.93, 0.00, 0.00}},
    {7.3, {596, 902, 299, 0, 0}, {21.75, 18.76, 10.20, 0.00, 0.00}},
    {4.0, {595, 902, 299, 0, 0}, {21.72, 17.99, 12.86, 0.00, 0.00}},
    {6.0, {594, 900, 299, 0, 0}, {21.69, 13.66, 18.22, 0.00, 0.00}},
    {3.0, {593, 900, 299, 0, 0}, {21.67, 11.90, 18.52, 0.00, 0.00}},
    {14.6, {590, 898, 300, 0, 0}, {21.59, 3.07, 20.04, 0.00, 0.00}},
    {1.4, {586, 898, 300, 0, 0}, {21.65, 2.68, 20.20, 0.00, 0.00}},
    {1.0, {583, 899, 300, 0, 0}, {21.70, 3.61, 20.31, 0.00, 0.00}},
    {1.0, {580, 281, 300, 0, 0}, {21.75, 0.00, 20.42, 0.00, 0.00}},
    {5.5, {563, 281, 300, 0, 0}, {22.02, 0.00, 21.05, 0.00, 0.00}},
    {10.5, {512, 281, 300, 0, 0}, {21.38, 0.00, 22.28, 0.00, 0.00}},
    {1.0, {507, 281, 300, 0, 0}, {21.32, 0.00, 0.00, 0.00, 0.00}},
    {7.5, {471, 281, 300, 0, 0}, {20.89, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {472, 281, 300, 0, 0}, {21.36, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {421, 281, 300, 0, 0}, {21.88, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {453, 281, 300, 0, 0}, {19.85, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {453, 281, 300, 0, 0}, {19.85, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {447, 281, 300, 0, 0}, {19.47, 17.55, 0.00, 0.00, 0.00}},
    {1.0, {441, 307, 300, 0, 0}, {19.09, 20.06, 0.00, 0.00, 0.00}},
    {1.0, {339, 334, 300, 0, 0}, {0.00, 22.94, 0.00, 0.00, 0.00}},
    {1.0, {339, 361, 300, 0, 0}, {0.00, 26.22, 0.00, 0.00, 0.00}},
    {2.0, {339, 360, 300, 0, 0}, {0.00, 25.45, 0.00, 0.00, 0.00}},
    {2.0, {339, 417, 300, 0, 0}, {0.00, 16.80, 0.00, 0.00, 0.00}},
    {2.0, {339, 424, 300, 0, 0}, {0.00, 15.00, 0.00, 0.00, 0.00}},
    {1.0, {339, 406, 300, 0, 0}, {0.00, 16.27, 0.00, 0.00, 0.00}},
    {0.5, {339, 406, 300, 0, 0}, {0.00, 16.28, 0.00, 0.00, 0.00}},
    {1.0, {339, 406, 300, 0, 0}, {21.31, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {339, 406, 300, 0, 0}, {21.32, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {332, 406, 300, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {324, 406, 300, 0, 0}, {11.53, 0.00, 0.00, 0.00, 0.00}},
    {0.6, {406, 406, 300, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {0.5, {406, 406, 300, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {0.6, {406, 406, 300, 0, 0}, {17.57, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {357, 406, 300, 0, 0}, {10.11, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {318, 406, 300, 0, 0}, {13.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {313, 406, 300, 0, 0}, {22.88, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {302, 406, 300, 0, 0}, {25.32, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {323, 406, 300, 0, 0}, {23.83, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {289, 406, 300, 0, 0}, {23.22, 0.00, 0.00, 0.00, 0.00}},
    {15.0, {299, 406, 300, 0, 0}, {21.74, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {305, 406, 300, 0, 0}, {8.52, 0.00, 0.00, 0.00, 0.00}},
    {10.0, {305, 406, 300, 0, 0}, {4.57, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.87, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {4.71, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.80, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {305, 406, 300, 0, 0}, {4.67, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.79, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {4.66, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {305, 406, 300, 0, 0}, {6.79, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {4.73, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.88, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {4.73, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.77, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {305, 406, 300, 0, 0}, {4.63, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {6.70, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {3.99, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {305, 406, 300, 0, 0}, {4.05, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {305, 406, 300, 0, 0}, {0.68, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {316, 406, 300, 0, 0}, {1.23, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {316, 406, 300, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {316, 406, 300, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {965, 1334, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {965, 1334, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {965, 1334, 0, 0, 0}, {3.18, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {968, 1334, 0, 0, 0}, {7.64, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {963, 1334, 0, 0, 0}, {17.75, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {960, 1334, 0, 0, 0}, {20.41, 17.54, 0.00, 0.00, 0.00}},
    {3.0, {950, 1340, 0, 0, 0}, {31.00, 21.88, 0.00, 0.00, 0.00}},
    {3.0, {950, 1346, 0, 0, 0}, {30.99, 27.30, 0.00, 0.00, 0.00}},
    {84.0, {950, 1341, 0, 0, 0}, {30.66, 27.11, 0.00, 0.00, 0.00}},
    {3.0, {950, 1341, 0, 0, 0}, {30.65, 25.67, 0.00, 0.00, 0.00}},
    {18.0, {950, 1342, 0, 0, 0}, {17.88, 18.53, 0.00, 0.00, 0.00}},
    {23.0, {949, 1342, 0, 0, 0}, {8.99, 6.41, 0.00, 0.00, 0.00}},
    {6.0, {949, 1342, 0, 0, 0}, {5.35, 4.86, 0.00, 0.00, 0.00}},
    {3.0, {949, 1342, 0, 0, 0}, {4.13, 3.02, 0.00, 0.00, 0.00}},
    {3.0, {949, 1342, 0, 0, 0}, {2.36, 1.88, 0.00, 0.00, 0.00}},
    {1.0, {949, 1342, 0, 0, 0}, {1.96, 1.36, 0.00, 0.00, 0.00}},
    {2.0, {949, 1342, 0, 0, 0}, {0.98, 0.72, 0.00, 0.00, 0.00}},
    {1.0, {949, 1342, 0, 0, 0}, {0.69, 0.43, 0.00, 0.00, 0.00}},
    {1.0, {950, 1342, 0, 0, 0}, {0.79, 0.26, 0.00, 0.00, 0.00}},
    {1.0, {950, 1344, 0, 0, 0}, {0.00, 0.30, 0.00, 0.00, 0.00}},
    {1.0, {950, 1344, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {950, 1344, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {690, 1231, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {690, 1231, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {690, 1231, 0, 0, 0}, {3.77, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {697, 1231, 0, 0, 0}, {8.78, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {701, 1231, 0, 0, 0}, {13.38, 12.47, 0.00, 0.00, 0.00}},
    {1.0, {705, 1213, 0, 0, 0}, {20.40, 17.27, 0.00, 0.00, 0.00}},
    {4.0, {707, 1214, 0, 0, 0}, {31.00, 25.89, 0.00, 0.00, 0.00}},
    {90.0, {701, 1215, 0, 0, 0}, {30.46, 20.47, 0.00, 0.00, 0.00}},
    {13.0, {701, 1215, 0, 0, 0}, {20.16, 19.78, 0.00, 0.00, 0.00}},
    {20.0, {701, 1215, 0, 0, 0}, {10.69, 10.64, 0.00, 0.00, 0.00}},
    {8.0, {701, 1215, 0, 0, 0}, {8.29, 6.40, 0.00, 0.00, 0.00}},
    {4.0, {701, 1215, 0, 0, 0}, {5.59, 4.97, 0.00, 0.00, 0.00}},
    {5.0, {701, 1215, 0, 0, 0}, {3.42, 2.50, 0.00, 0.00, 0.00}},
    {2.0, {701, 1215, 0, 0, 0}, {2.09, 1.90, 0.00, 0.00, 0.00}},
    {4.0, {700, 1215, 0, 0, 0}, {0.78, 0.50, 0.00, 0.00, 0.00}},
    {1.0, {700, 1215, 0, 0, 0}, {0.61, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {700, 1215, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {700, 1215, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {714, 1350, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {714, 1350, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {714, 1350, 0, 0, 0}, {3.28, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {693, 1350, 0, 0, 0}, {4.11, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {692, 1350, 0, 0, 0}, {6.72, 4.53, 0.00, 0.00, 0.00}},
    {2.0, {690, 1343, 0, 0, 0}, {17.89, 14.05, 0.00, 0.00, 0.00}},
    {4.0, {704, 1346, 0, 0, 0}, {30.98, 22.51, 0.00, 0.00, 0.00}},
    {1.0, {704, 1347, 0, 0, 0}, {30.98, 25.33, 0.00, 0.00, 0.00}},
    {90.0, {701, 1341, 0, 0, 0}, {31.00, 21.88, 0.00, 0.00, 0.00}},
    {7.0, {701, 1341, 0, 0, 0}, {26.18, 21.63, 0.00, 0.00, 0.00}},
    {19.0, {701, 1341, 0, 0, 0}, {16.55, 12.46, 0.00, 0.00, 0.00}},
    {6.0, {701, 1341, 0, 0, 0}, {12.34, 10.47, 0.00, 0.00, 0.00}},
    {10.0, {701, 1341, 0, 0, 0}, {7.57, 5.50, 0.00, 0.00, 0.00}},
    {4.0, {701, 1341, 0, 0, 0}, {4.82, 4.26, 0.00, 0.00, 0.00}},
    {4.0, {701, 1341, 0, 0, 0}, {3.07, 2.18, 0.00, 0.00, 0.00}},
    {2.0, {701, 1341, 0, 0, 0}, {1.79, 1.56, 0.00, 0.00, 0.00}},
    {1.0, {701, 1341, 0, 0, 0}, {1.37, 1.05, 0.00, 0.00, 0.00}},
    {2.0, {701, 1341, 0, 0, 0}, {0.57, 0.48, 0.00, 0.00, 0.00}},
    {1.0, {701, 1341, 0, 0, 0}, {0.36, 0.25, 0.00, 0.00, 0.00}},
    {1.0, {701, 1341, 0, 0, 0}, {0.38, 0.13, 0.00, 0.00, 0.00}},
    {1.0, {701, 1342, 0, 0, 0}, {0.00, 0.15, 0.00, 0.00, 0.00}},
    {1.0, {701, 1342, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {701, 1342, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {699, 1489, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {699, 1489, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {699, 1489, 0, 0, 0}, {2.30, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {699, 1489, 0, 0, 0}, {4.24, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {699, 1489, 0, 0, 0}, {7.83, 10.19, 0.00, 0.00, 0.00}},
    {1.0, {699, 1487, 0, 0, 0}, {14.47, 11.73, 0.00, 0.00, 0.00}},
    {5.0, {706, 1479, 0, 0, 0}, {31.00, 23.66, 0.00, 0.00, 0.00}},
    {89.0, {701, 1480, 0, 0, 0}, {30.55, 20.34, 0.00, 0.00, 0.00}},
    {9.0, {701, 1480, 0, 0, 0}, {25.08, 20.04, 0.00, 0.00, 0.00}},
    {15.0, {701, 1480, 0, 0, 0}, {18.04, 13.25, 0.00, 0.00, 0.00}},
    {6.0, {701, 1480, 0, 0, 0}, {13.71, 11.23, 0.00, 0.00, 0.00}},
    {13.0, {701, 1480, 0, 0, 0}, {7.56, 5.50, 0.00, 0.00, 0.00}},
    {2.0, {701, 1480, 0, 0, 0}, {6.04, 4.93, 0.00, 0.00, 0.00}},
    {7.0, {701, 1480, 0, 0, 0}, {2.76, 1.85, 0.00, 0.00, 0.00}},
    {1.0, {701, 1480, 0, 0, 0}, {2.03, 1.61, 0.00, 0.00, 0.00}},
    {4.0, {701, 1480, 0, 0, 0}, {0.59, 0.35, 0.00, 0.00, 0.00}},
    {1.0, {701, 1480, 0, 0, 0}, {0.43, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {701, 1480, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {701, 1480, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {794, 1202, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {794, 1202, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {794, 1202, 0, 0, 0}, {5.90, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {780, 1202, 0, 0, 0}, {15.68, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {775, 1202, 0, 0, 0}, {21.73, 18.39, 0.00, 0.00, 0.00}},
    {4.0, {773, 1216, 0, 0, 0}, {30.98, 26.27, 0.00, 0.00, 0.00}},
    {87.0, {773, 1215, 0, 0, 0}, {31.00, 21.14, 0.00, 0.00, 0.00}},
    {15.0, {773, 1215, 0, 0, 0}, {23.03, 20.36, 0.00, 0.00, 0.00}},
    {7.0, {772, 1215, 0, 0, 0}, {20.05, 15.47, 0.00, 0.00, 0.00}},
    {20.0, {773, 1215, 0, 0, 0}, {9.23, 7.05, 0.00, 0.00, 0.00}},
    {4.0, {773, 1215, 0, 0, 0}, {6.49, 6.03, 0.00, 0.00, 0.00}},
    {6.0, {773, 1215, 0, 0, 0}, {3.83, 2.93, 0.00, 0.00, 0.00}},
    {1.0, {773, 1215, 0, 0, 0}, {2.96, 2.60, 0.00, 0.00, 0.00}},
    {5.0, {773, 1215, 0, 0, 0}, {0.82, 0.65, 0.00, 0.00, 0.00}},
    {1.0, {773, 1215, 0, 0, 0}, {0.48, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {773, 1215, 0, 0, 0}, {0.28, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {773, 1215, 0, 0, 0}, {0.09, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {773, 1215, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {773, 1215, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {765, 1354, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {765, 1354, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {765, 1354, 0, 0, 0}, {2.21, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {765, 1354, 0, 0, 0}, {7.72, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {765, 1354, 0, 0, 0}, {14.41, 14.60, 0.00, 0.00, 0.00}},
    {5.0, {772, 1348, 0, 0, 0}, {31.00, 23.16, 0.00, 0.00, 0.00}},
    {1.0, {772, 1346, 0, 0, 0}, {30.99, 25.40, 0.00, 0.00, 0.00}},
    {89.0, {773, 1342, 0, 0, 0}, {30.52, 20.40, 0.00, 0.00, 0.00}},
    {12.0, {773, 1341, 0, 0, 0}, {23.04, 19.80, 0.00, 0.00, 0.00}},
    {13.0, {773, 1341, 0, 0, 0}, {16.99, 11.77, 0.00, 0.00, 0.00}},
    {19.0, {773, 1341, 0, 0, 0}, {6.84, 5.50, 0.00, 0.00, 0.00}},
    {7.0, {773, 1341, 0, 0, 0}, {2.94, 2.33, 0.00, 0.00, 0.00}},
    {5.0, {772, 1342, 0, 0, 0}, {0.54, 0.48, 0.00, 0.00, 0.00}},
    {1.0, {772, 1342, 0, 0, 0}, {0.39, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {772, 1342, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {772, 1342, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {780, 1476, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {780, 1476, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {780, 1476, 0, 0, 0}, {4.49, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {758, 1476, 0, 0, 0}, {5.03, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {758, 1476, 0, 0, 0}, {7.82, 4.24, 0.00, 0.00, 0.00}},
    {1.0, {758, 1470, 0, 0, 0}, {12.17, 10.89, 0.00, 0.00, 0.00}},
    {1.0, {757, 1473, 0, 0, 0}, {18.93, 14.95, 0.00, 0.00, 0.00}},
    {4.0, {771, 1479, 0, 0, 0}, {31.00, 24.40, 0.00, 0.00, 0.00}},
    {91.0, {773, 1480, 0, 0, 0}, {30.28, 23.33, 0.00, 0.00, 0.00}},
    {1.0, {773, 1480, 0, 0, 0}, {29.31, 23.32, 0.00, 0.00, 0.00}},
    {27.0, {773, 1480, 0, 0, 0}, {12.10, 11.78, 0.00, 0.00, 0.00}},
    {13.0, {773, 1481, 0, 0, 0}, {7.91, 5.91, 0.00, 0.00, 0.00}},
    {2.0, {773, 1481, 0, 0, 0}, {6.39, 5.31, 0.00, 0.00, 0.00}},
    {7.0, {773, 1481, 0, 0, 0}, {3.03, 2.14, 0.00, 0.00, 0.00}},
    {1.0, {773, 1481, 0, 0, 0}, {2.27, 1.88, 0.00, 0.00, 0.00}},
    {5.0, {773, 1480, 0, 0, 0}, {0.54, 0.28, 0.00, 0.00, 0.00}},
    {1.0, {773, 1480, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {773, 1480, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {895, 1220, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {895, 1220, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {895, 1220, 0, 0, 0}, {5.77, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {862, 1220, 0, 0, 0}, {4.45, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {856, 1220, 0, 0, 0}, {11.19, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {852, 1220, 0, 0, 0}, {17.76, 8.60, 0.00, 0.00, 0.00}},
    {1.0, {854, 1212, 0, 0, 0}, {19.85, 18.94, 0.00, 0.00, 0.00}},
    {4.0, {861, 1214, 0, 0, 0}, {31.00, 27.18, 0.00, 0.00, 0.00}},
    {92.0, {856, 1214, 0, 0, 0}, {27.69, 25.70, 0.00, 0.00, 0.00}},
    {3.0, {856, 1214, 0, 0, 0}, {27.59, 23.24, 0.00, 0.00, 0.00}},
    {25.0, {855, 1215, 0, 0, 0}, {13.95, 10.07, 0.00, 0.00, 0.00}},
    {11.0, {855, 1215, 0, 0, 0}, {7.51, 6.97, 0.00, 0.00, 0.00}},
    {3.0, {855, 1215, 0, 0, 0}, {6.35, 5.12, 0.00, 0.00, 0.00}},
    {5.0, {855, 1215, 0, 0, 0}, {3.17, 3.06, 0.00, 0.00, 0.00}},
    {2.0, {855, 1215, 0, 0, 0}, {2.40, 1.78, 0.00, 0.00, 0.00}},
    {3.0, {855, 1215, 0, 0, 0}, {0.73, 0.78, 0.00, 0.00, 0.00}},
    {1.0, {856, 1215, 0, 0, 0}, {0.49, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {856, 1215, 0, 0, 0}, {0.33, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {856, 1215, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {856, 1215, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {878, 1360, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {878, 1360, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {878, 1360, 0, 0, 0}, {2.56, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {853, 1360, 0, 0, 0}, {3.07, 0.00, 0.00, 0.00, 0.00}},
    {2.0, {850, 1360, 0, 0, 0}, {9.08, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {849, 1360, 0, 0, 0}, {15.60, 8.53, 0.00, 0.00, 0.00}},
    {1.0, {852, 1344, 0, 0, 0}, {17.90, 17.35, 0.00, 0.00, 0.00}},
    {4.0, {860, 1346, 0, 0, 0}, {31.00, 26.07, 0.00, 0.00, 0.00}},
    {90.0, {856, 1342, 0, 0, 0}, {30.44, 23.89, 0.00, 0.00, 0.00}},
    {4.0, {856, 1342, 0, 0, 0}, {27.55, 23.80, 0.00, 0.00, 0.00}},
    {25.0, {856, 1342, 0, 0, 0}, {14.76, 9.98, 0.00, 0.00, 0.00}},
    {13.0, {855, 1342, 0, 0, 0}, {7.33, 6.35, 0.00, 0.00, 0.00}},
    {1.0, {855, 1342, 0, 0, 0}, {6.94, 5.66, 0.00, 0.00, 0.00}},
    {8.0, {855, 1342, 0, 0, 0}, {2.40, 2.24, 0.00, 0.00, 0.00}},
    {1.0, {855, 1342, 0, 0, 0}, {2.10, 1.59, 0.00, 0.00, 0.00}},
    {4.0, {854, 1342, 0, 0, 0}, {0.47, 0.41, 0.00, 0.00, 0.00}},
    {1.0, {854, 1342, 0, 0, 0}, {0.32, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {854, 1342, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {854, 1342, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {865, 1486, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {865, 1486, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {865, 1486, 0, 0, 0}, {3.72, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {861, 1486, 0, 0, 0}, {5.67, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {858, 1486, 0, 0, 0}, {8.64, 7.37, 0.00, 0.00, 0.00}},
    {2.0, {850, 1478, 0, 0, 0}, {20.07, 16.23, 0.00, 0.00, 0.00}},
    {4.0, {860, 1480, 0, 0, 0}, {31.00, 25.01, 0.00, 0.00, 0.00}},
    {92.0, {856, 1480, 0, 0, 0}, {29.91, 23.41, 0.00, 0.00, 0.00}},
    {2.0, {856, 1480, 0, 0, 0}, {27.99, 23.37, 0.00, 0.00, 0.00}},
    {37.0, {856, 1481, 0, 0, 0}, {8.21, 7.04, 0.00, 0.00, 0.00}},
    {2.0, {856, 1481, 0, 0, 0}, {7.68, 5.73, 0.00, 0.00, 0.00}},
    {8.0, {856, 1481, 0, 0, 0}, {3.01, 2.51, 0.00, 0.00, 0.00}},
    {1.0, {856, 1482, 0, 0, 0}, {2.68, 1.97, 0.00, 0.00, 0.00}},
    {5.0, {856, 1483, 0, 0, 0}, {0.49, 0.59, 0.00, 0.00, 0.00}},
    {1.0, {856, 1483, 0, 0, 0}, {0.35, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {856, 1483, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {856, 1483, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {936, 1211, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {936, 1211, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {936, 1211, 0, 0, 0}, {5.07, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {937, 1211, 0, 0, 0}, {20.72, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {940, 1211, 0, 0, 0}, {22.92, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {944, 1211, 0, 0, 0}, {25.35, 21.74, 0.00, 0.00, 0.00}},
    {2.0, {950, 1214, 0, 0, 0}, {31.00, 25.90, 0.00, 0.00, 0.00}},
    {1.0, {950, 1215, 0, 0, 0}, {31.00, 28.27, 0.00, 0.00, 0.00}},
    {87.0, {949, 1215, 0, 0, 0}, {30.65, 27.86, 0.00, 0.00, 0.00}},
    {34.0, {948, 1216, 0, 0, 0}, {11.60, 11.60, 0.00, 0.00, 0.00}},
    {5.0, {948, 1216, 0, 0, 0}, {10.05, 7.80, 0.00, 0.00, 0.00}},
    {10.0, {948, 1216, 0, 0, 0}, {4.57, 3.53, 0.00, 0.00, 0.00}},
    {2.0, {948, 1216, 0, 0, 0}, {3.30, 3.01, 0.00, 0.00, 0.00}},
    {1.0, {948, 1217, 0, 0, 0}, {2.80, 3.75, 0.00, 0.00, 0.00}},
    {1.0, {948, 1217, 0, 0, 0}, {2.38, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {948, 1217, 0, 0, 0}, {0.93, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {949, 1217, 0, 0, 0}, {1.11, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {949, 1217, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {949, 1217, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {953, 1470, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {953, 1470, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {953, 1470, 0, 0, 0}, {8.48, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {945, 1470, 0, 0, 0}, {8.22, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {946, 1470, 0, 0, 0}, {11.58, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {947, 1470, 0, 0, 0}, {16.30, 15.30, 0.00, 0.00, 0.00}},
    {1.0, {947, 1473, 0, 0, 0}, {22.95, 17.90, 0.00, 0.00, 0.00}},
    {2.0, {948, 1477, 0, 0, 0}, {26.67, 24.51, 0.00, 0.00, 0.00}},
    {2.0, {949, 1478, 0, 0, 0}, {31.00, 24.94, 0.00, 0.00, 0.00}},
    {5.0, {950, 1479, 0, 0, 0}, {30.89, 26.03, 0.00, 0.00, 0.00}},
    {81.0, {950, 1479, 0, 0, 0}, {29.12, 25.79, 0.00, 0.00, 0.00}},
    {6.0, {950, 1479, 0, 0, 0}, {28.99, 22.86, 0.00, 0.00, 0.00}},
    {19.0, {949, 1480, 0, 0, 0}, {18.08, 15.61, 0.00, 0.00, 0.00}},
    {7.0, {949, 1479, 0, 0, 0}, {15.19, 11.98, 0.00, 0.00, 0.00}},
    {8.0, {949, 1479, 0, 0, 0}, {9.92, 8.86, 0.00, 0.00, 0.00}},
    {7.0, {949, 1479, 0, 0, 0}, {6.83, 5.17, 0.00, 0.00, 0.00}},
    {4.0, {949, 1479, 0, 0, 0}, {4.19, 3.81, 0.00, 0.00, 0.00}},
    {3.0, {949, 1479, 0, 0, 0}, {2.90, 2.10, 0.00, 0.00, 0.00}},
    {3.0, {949, 1479, 0, 0, 0}, {1.24, 1.16, 0.00, 0.00, 0.00}},
    {3.0, {949, 1478, 0, 0, 0}, {0.53, 0.29, 0.00, 0.00, 0.00}},
    {1.0, {949, 1478, 0, 0, 0}, {0.00, 0.19, 0.00, 0.00, 0.00}},
    {1.0, {949, 1478, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {949, 1478, 0, 0, 0}, {0, 0, 0, 0, 0}},
    {1.0, {1732, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1732, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1732, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1784, 0, 0, 0, 0}, {19.71, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1810, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1826, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1786, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1810, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1789, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1836, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1818, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1838, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1822, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1841, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1799, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1815, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1778, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1822, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1784, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1795, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1851, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1816, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1803, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {11.0, {1837, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {14.0, {1798, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {9.0, {1834, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1790, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1832, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1818, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1838, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1826, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1785, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1803, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1795, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1828, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1802, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {1795, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {9.0, {1826, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1777, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1821, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1804, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {24.0, {1812, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1787, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1797, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1782, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1796, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1844, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1810, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1787, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {5.0, {1808, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1797, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1818, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {8.0, {1765, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {7.0, {1806, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {6.0, {1791, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1802, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1783, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1799, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {10.0, {1782, 0, 0, 0, 0}, {31.00, 0.00, 0.00, 0.00, 0.00}},
    {503.0, {1783, 0, 0, 0, 0}, {6.77, 0.00, 0.00, 0.00, 0.00}},
    {20.0, {1783, 0, 0, 0, 0}, {4.28, 0.00, 0.00, 0.00, 0.00}},
    {113.0, {1783, 0, 0, 0, 0}, {3.57, 0.00, 0.00, 0.00, 0.00}},
    {13.0, {1783, 0, 0, 0, 0}, {2.44, 0.00, 0.00, 0.00, 0.00}},
    {107.0, {1783, 0, 0, 0, 0}, {2.53, 0.00, 0.00, 0.00, 0.00}},
    {12.0, {1783, 0, 0, 0, 0}, {1.33, 0.00, 0.00, 0.00, 0.00}},
    {84.0, {1783, 0, 0, 0, 0}, {1.30, 0.00, 0.00, 0.00, 0.00}},
    {4.0, {1783, 0, 0, 0, 0}, {0.98, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1783, 0, 0, 0, 0}, {0.41, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1794, 0, 0, 0, 0}, {0.70, 0.00, 0.00, 0.00, 0.00}},
    {1.0, {1794, 0, 0, 0, 0}, {0.00, 0.00, 0.00, 0.00, 0.00}},
    {3.0, {1794, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
};

static const struct {
  short first, count;
} fx2_index[] = {
    {0, 15},
    {15, 30},
    {45, 27},
    {72, 35},
    {107, 45},
    {152, 31},
    {183, 48},
    {231, 57},
    {288, 61},
    {349, 57},
    {406, 67},
    {0, 0},
    {0, 0},
    {0, 0},
    {473, 38},
    {511, 72},
    {583, 24},
    {607, 55},
    {662, 49},
    {711, 22},
    {733, 14},
    {747, 23},
    {770, 50},
    {820, 30},
    {850, 36},
    {886, 18},
    {904, 46},
    {950, 22},
    {972, 30},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {1002, 83},
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0},
    {1085, 22},
    {1107, 18},
    {1125, 23},
    {1148, 19},
    {1167, 19},
    {1186, 16},
    {1202, 18},
    {1220, 20},
    {1240, 19},
    {1259, 17},
    {1276, 19},
    {1295, 23},
    {1318, 71},
    {0, 0},
    {0, 0}
};

static void contour2(sound *s, const fx2_point *p, int n, int code) {
  const int k = fx_noise_index(code);
  const int q = fx_pulse_index(code);
  for (int i = 0; i < n; i++) {
    frame f;
    memset(&f, 0, sizeof(f));
    for (int j = 0; j < 5; j++) {
      f.freq[j] = p[i].freq[j];
      f.vol[j] = p[i].vol[j];
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
/* FX2-END */

/* FXV-BEGIN */
/* the sound effects that are a buzz, voiced at the default speech pitch
 * with formants that move, played as the chip plays
 * them, pulse by pulse: measured the same way as the allophones played from
 * the chip's frames (see vf_points), on both units. each point is a pulse:
 * its time after the one before (glided to in a straight line), the three
 * formants and their volumes. A2 is played this way; A1, A3 and M2 measured
 * this way came out further from the recordings, and keep their traces (A1's
 * traced again: see fx_points) */
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

/* C1 is voiced: a harmonic series whose own pitch sweeps, with three
 * formants moving through it. on both units, in Man Goes Down (C1 at speed 62,
 * where it is slow enough to see), every line is a whole multiple of the
 * pitch, and the pitch steps a whole voicing tick (see voice_hz()) at a time:
 * 24 ticks (333Hz) for the first 40ms, up to 14 (570Hz) by 80ms, then one tick
 * slower every few milliseconds to 40 ticks (200Hz) at the end, the same on
 * both units within 4Hz. the fourth VoxProbe recording plays it at pitch 60,
 * 88 and 150 and at bend 0 and 15, and the sweep is the same at all of them
 * but pitch 150 (a tick or two faster there), so the Pitch control doesn't
 * move it (see sound.own_pitch); Bend moves the formants as usual. over the
 * last 200ms one line rises from 2.0 to 2.9kHz (3.35kHz at bend 15) straight
 * through the harmonics: oscillator 3 there runs free of the voicing (see
 * frame.free_osc). the trace this replaces took the loudest harmonic for a
 * tone, jumping from one to the next, under broad noise: at speed 62 it was
 * noise and 4.3dB too loud against the speech.
 *
 * the points are every 1.9ms of the sweep at the default speed: the pitch as
 * measured, and the formants and volumes, as knots every 5 to 20ms, fitted to
 * both units of Man Goes Down and of the fourth VoxProbe recording at the
 * default and bend 15, with C1's level against the game's first phrase, full
 * band (150Hz to 16kHz, see docs/design.md). fitted: Man Goes Down 0.60 to
 * 0.85 and level +4.3 to 0.0dB, default 0.81 to 0.81, bend 15 0.78 to 0.80.
 * left out: pitch 150 0.74 to 0.77, bend 0 0.73 to 0.78 (5.4 to 3.0dB), the
 * VoxRecord recording 0.813 to 0.817, pitch 60 0.81 to 0.79 */
typedef struct c1_point {
  float ms, pitch;
  float freq[3];
  float vol[3];
} c1_point;

static const c1_point c1_points[] = {
    {1.00, 332.8, {877, 1443, 1997}, {10.15, 1.21, 0.00}},
    {1.90, 332.8, {765, 1439, 2089}, {6.63, 0.64, 0.00}},
    {1.90, 332.8, {848, 1354, 2096}, {11.68, 3.93, 0.00}},
    {1.90, 332.8, {996, 1242, 2074}, {19.58, 8.50, 0.00}},
    {1.90, 332.8, {1112, 1172, 2064}, {25.95, 12.43, 0.00}},
    {1.90, 347.2, {1004, 1388, 2133}, {21.57, 11.86, 0.00}},
    {1.90, 347.2, {896, 1604, 2203}, {17.19, 11.29, 0.00}},
    {1.90, 347.2, {823, 1626, 2249}, {16.38, 14.57, 0.00}},
    {1.90, 347.2, {784, 1454, 2273}, {19.14, 21.71, 0.00}},
    {1.90, 347.2, {745, 1282, 2297}, {21.90, 28.86, 0.00}},
    {1.90, 347.2, {660, 1407, 2243}, {22.42, 23.50, 0.00}},
    {1.90, 347.2, {569, 1574, 2179}, {22.61, 16.36, 0.00}},
    {1.90, 347.2, {557, 1662, 2137}, {23.54, 12.43, 0.19}},
    {1.90, 347.2, {786, 1510, 2163}, {26.68, 18.14, 0.95}},
    {1.90, 347.2, {1014, 1357, 2190}, {29.82, 23.86, 1.71}},
    {1.90, 347.2, {1076, 1440, 2217}, {25.70, 23.50, 1.52}},
    {1.90, 347.2, {1038, 1665, 2243}, {17.23, 19.50, 0.76}},
    {1.90, 347.2, {1000, 1890, 2270}, {8.75, 15.50, 0.00}},
    {1.90, 380.3, {1335, 1981, 2205}, {17.23, 20.64, 2.67}},
    {1.90, 399.3, {1670, 2073, 2140}, {25.70, 25.79, 5.33}},
    {1.90, 399.3, {1791, 2064, 2103}, {28.79, 28.14, 7.57}},
    {1.90, 420.3, {1555, 1888, 2110}, {22.88, 25.86, 9.10}},
    {1.90, 420.3, {1319, 1712, 2118}, {16.98, 23.57, 10.62}},
    {1.90, 420.3, {1211, 1607, 2040}, {17.50, 22.14, 9.57}},
    {1.90, 420.3, {1147, 1526, 1933}, {20.17, 21.00, 7.67}},
    {1.90, 443.7, {1076, 1474, 1832}, {22.58, 19.74, 5.90}},
    {1.90, 443.7, {966, 1625, 1771}, {23.25, 17.64, 5.14}},
    {1.90, 443.7, {855, 1777, 1710}, {23.92, 15.55, 4.38}},
    {1.90, 469.8, {747, 1667, 1730}, {23.49, 15.17, 4.76}},
    {1.90, 469.8, {640, 1297, 1829}, {21.96, 16.50, 6.29}},
    {1.90, 469.8, {533, 926, 1928}, {20.44, 17.83, 7.81}},
    {1.90, 499.1, {433, 1078, 2032}, {17.58, 19.17, 12.00}},
    {1.90, 499.1, {334, 1303, 2136}, {14.54, 20.50, 16.57}},
    {1.90, 499.1, {264, 1418, 2218}, {11.13, 20.60, 20.00}},
    {1.90, 532.4, {279, 1198, 2232}, {6.65, 16.98, 20.00}},
    {1.90, 532.4, {294, 977, 2245}, {2.18, 13.36, 20.00}},
    {1.90, 532.4, {369, 900, 2258}, {1.51, 11.05, 20.48}},
    {1.90, 532.4, {480, 909, 2272}, {3.13, 9.52, 21.24}},
    {1.90, 570.4, {590, 918, 2285}, {4.75, 8.00, 22.00}},
    {1.90, 570.4, {361, 1086, 2298}, {5.61, 11.43, 23.14}},
    {1.90, 570.4, {133, 1255, 2312}, {6.46, 14.86, 24.29}},
    {1.90, 570.4, {59, 1412, 2295}, {6.00, 18.32, 24.00}},
    {1.90, 570.4, {241, 1550, 2228}, {3.33, 21.85, 21.33}},
    {1.90, 532.4, {424, 1688, 2162}, {0.67, 25.37, 18.67}},
    {1.90, 532.4, {376, 1632, 2164}, {3.57, 22.61, 18.29}},
    {1.90, 532.4, {250, 1511, 2188}, {8.33, 17.75, 18.67}},
    {1.90, 532.4, {128, 1447, 2183}, {11.90, 14.08, 18.50}},
    {1.90, 532.4, {29, 1779, 1966}, {7.14, 18.75, 14.50}},
    {1.90, 532.4, {-70, 2111, 1749}, {2.38, 23.42, 10.50}},
    {1.90, 532.4, {-125, 2182, 1667}, {0.33, 24.96, 8.79}},
    {1.90, 532.4, {-134, 1992, 1721}, {1.00, 23.39, 9.36}},
    {1.90, 532.4, {-143, 1803, 1775}, {1.67, 21.82, 9.93}},
    {1.90, 532.4, {-152, 1612, 1829}, {2.33, 20.25, 10.50}},
    {1.90, 499.1, {-161, 1422, 1882}, {3.00, 18.68, 11.07}},
    {1.90, 499.1, {-158, 1288, 1931}, {3.46, 17.39, 11.64}},
    {1.90, 499.1, {-120, 1320, 1963}, {3.30, 16.94, 12.21}},
    {1.90, 499.1, {-83, 1351, 1996}, {3.14, 16.50, 12.79}},
    {1.90, 499.1, {-45, 1383, 2028}, {2.98, 16.06, 13.36}},
    {1.90, 499.1, {-7, 1415, 2060}, {2.83, 15.61, 13.93}},
    {1.90, 499.1, {31, 1447, 2092}, {2.67, 15.17, 14.50}},
    {1.90, 499.1, {69, 1478, 2125}, {2.51, 14.72, 15.07}},
    {1.90, 499.1, {106, 1510, 2157}, {2.35, 14.28, 15.64}},
    {1.90, 499.1, {130, 1545, 2182}, {2.17, 14.33, 15.43}},
    {1.90, 469.8, {131, 1586, 2195}, {1.96, 15.21, 13.90}},
    {1.90, 469.8, {132, 1627, 2208}, {1.74, 16.09, 12.38}},
    {1.90, 469.8, {133, 1668, 2221}, {1.53, 16.97, 10.86}},
    {1.90, 469.8, {134, 1709, 2234}, {1.31, 17.85, 9.33}},
    {1.90, 469.8, {135, 1750, 2247}, {1.10, 18.74, 7.81}},
    {1.90, 469.8, {136, 1791, 2260}, {0.88, 19.62, 6.29}},
    {1.90, 469.8, {137, 1832, 2272}, {0.67, 20.50, 4.76}},
    {1.90, 469.8, {138, 1873, 2285}, {0.46, 21.38, 3.24}},
    {1.90, 469.8, {139, 1914, 2298}, {0.24, 22.26, 1.71}},
    {1.90, 443.7, {140, 1955, 2311}, {0.03, 23.14, 0.19}},
    {1.90, 443.7, {202, 1904, 2288}, {0.25, 21.98, 1.33}},
    {1.90, 443.7, {274, 1840, 2259}, {0.54, 20.53, 2.86}},
    {1.90, 443.7, {345, 1777, 2230}, {0.82, 19.07, 4.38}},
    {1.90, 443.7, {417, 1713, 2201}, {1.11, 17.62, 5.90}},
    {1.90, 443.7, {488, 1649, 2172}, {1.39, 16.17, 7.43}},
    {1.90, 443.7, {560, 1585, 2143}, {1.68, 14.72, 8.95}},
    {1.90, 420.3, {631, 1521, 2114}, {1.96, 13.26, 10.48}},
    {1.90, 420.3, {702, 1458, 2085}, {2.25, 11.81, 12.00}},
    {1.90, 420.3, {774, 1394, 2056}, {2.54, 10.36, 13.52}},
    {1.90, 420.3, {845, 1330, 2027}, {2.82, 8.91, 15.05}},
    {1.90, 420.3, {924, 1324, 2017}, {3.21, 8.00, 15.57}},
    {1.90, 420.3, {1014, 1414, 2039}, {3.79, 8.00, 14.43}},
    {1.90, 420.3, {1105, 1505, 2062}, {4.36, 8.00, 13.29}},
    {1.90, 399.3, {1195, 1595, 2084}, {4.93, 8.00, 12.14}},
    {1.90, 399.3, {1286, 1686, 2107}, {5.50, 8.00, 11.00}},
    {1.90, 399.3, {1376, 1776, 2129}, {6.07, 8.00, 9.86}},
    {1.90, 399.3, {1467, 1867, 2152}, {6.64, 8.00, 8.71}},
    {1.90, 399.3, {1557, 1957, 2174}, {7.21, 8.00, 7.57}},
    {1.90, 399.3, {1648, 2048, 2197}, {7.79, 8.00, 6.43}},
    {1.90, 399.3, {1738, 2138, 2219}, {8.36, 8.00, 5.29}},
    {1.90, 399.3, {1829, 2229, 2241}, {8.93, 8.00, 4.14}},
    {1.90, 380.3, {1762, 2207, 2256}, {8.75, 7.96, 4.17}},
    {1.90, 380.3, {1674, 2170, 2270}, {8.46, 7.91, 4.36}},
    {1.90, 380.3, {1585, 2133, 2284}, {8.18, 7.86, 4.55}},
    {1.90, 363.0, {1497, 2096, 2298}, {7.89, 7.82, 4.74}},
    {1.90, 363.0, {1408, 2059, 2312}, {7.61, 7.77, 4.93}},
    {1.90, 363.0, {1320, 2022, 2326}, {7.32, 7.72, 5.12}},
    {1.90, 363.0, {1231, 1985, 2340}, {7.04, 7.67, 5.31}},
    {1.90, 363.0, {1142, 1948, 2354}, {6.75, 7.62, 5.50}},
    {1.90, 347.2, {1054, 1910, 2367}, {6.46, 7.58, 5.69}},
    {1.90, 347.2, {965, 1873, 2381}, {6.18, 7.53, 5.88}},
    {1.90, 347.2, {915, 1842, 2391}, {5.95, 7.34, 6.04}},
    {1.90, 347.2, {928, 1821, 2393}, {5.80, 6.91, 6.13}},
    {1.90, 347.2, {942, 1800, 2396}, {5.66, 6.48, 6.23}},
    {1.90, 332.8, {955, 1779, 2398}, {5.52, 6.05, 6.32}},
    {1.90, 332.8, {968, 1758, 2401}, {5.37, 5.62, 6.42}},
    {1.90, 332.8, {982, 1737, 2403}, {5.23, 5.20, 6.51}},
    {1.90, 332.8, {995, 1716, 2406}, {5.09, 4.77, 6.61}},
    {1.90, 319.4, {1008, 1695, 2408}, {4.95, 4.34, 6.70}},
    {1.90, 319.4, {1022, 1675, 2411}, {4.80, 3.91, 6.80}},
    {1.90, 319.4, {1035, 1654, 2413}, {4.66, 3.48, 6.89}},
    {1.90, 307.2, {1048, 1633, 2415}, {4.52, 3.05, 6.99}},
    {1.90, 307.2, {1035, 1629, 2433}, {4.29, 2.94, 6.75}},
    {1.90, 307.2, {1018, 1628, 2452}, {4.05, 2.87, 6.46}},
    {1.90, 307.2, {1001, 1627, 2472}, {3.82, 2.79, 6.18}},
    {1.90, 295.8, {984, 1626, 2492}, {3.58, 2.72, 5.89}},
    {1.90, 295.8, {966, 1625, 2511}, {3.34, 2.65, 5.61}},
    {1.90, 295.8, {949, 1624, 2531}, {3.10, 2.58, 5.32}},
    {1.90, 295.8, {932, 1623, 2550}, {2.86, 2.51, 5.04}},
    {1.90, 285.2, {915, 1622, 2570}, {2.62, 2.44, 4.75}},
    {1.90, 285.2, {898, 1622, 2590}, {2.39, 2.37, 4.46}},
    {1.90, 275.4, {881, 1621, 2609}, {2.15, 2.29, 4.18}},
    {1.90, 275.4, {866, 1619, 2624}, {1.96, 2.17, 3.96}},
    {1.90, 275.4, {854, 1615, 2633}, {1.84, 1.96, 3.87}},
    {1.90, 266.2, {843, 1611, 2641}, {1.72, 1.74, 3.77}},
    {1.90, 266.2, {831, 1607, 2649}, {1.60, 1.53, 3.68}},
    {1.90, 266.2, {820, 1603, 2657}, {1.48, 1.31, 3.58}},
    {1.90, 257.6, {809, 1600, 2665}, {1.36, 1.10, 3.49}},
    {1.90, 257.6, {797, 1596, 2673}, {1.24, 0.88, 3.39}},
    {1.90, 249.6, {786, 1592, 2682}, {1.12, 0.67, 3.30}},
    {1.90, 249.6, {774, 1588, 2690}, {1.00, 0.46, 3.20}},
    {1.90, 249.6, {763, 1584, 2698}, {0.88, 0.24, 3.11}},
    {1.90, 242.0, {751, 1580, 2706}, {0.76, 0.03, 3.01}},
    {1.90, 234.9, {732, 1592, 2714}, {0.69, 0.00, 2.94}},
    {1.90, 234.9, {711, 1605, 2722}, {0.62, 0.00, 2.87}},
    {1.90, 234.9, {690, 1618, 2731}, {0.54, 0.00, 2.79}},
    {1.90, 228.2, {669, 1632, 2739}, {0.47, 0.00, 2.72}},
    {1.90, 221.8, {648, 1645, 2747}, {0.40, 0.00, 2.65}},
    {1.90, 221.8, {627, 1658, 2755}, {0.33, 0.00, 2.58}},
    {1.90, 221.8, {606, 1672, 2763}, {0.26, 0.00, 2.51}},
    {1.90, 215.8, {585, 1685, 2771}, {0.19, 0.00, 2.44}},
    {1.90, 210.2, {564, 1698, 2780}, {0.12, 0.00, 2.37}},
    {1.90, 210.2, {543, 1712, 2788}, {0.04, 0.00, 2.29}},
    {1.90, 210.2, {530, 1720, 2802}, {0.00, 0.04, 2.14}},
    {1.90, 199.7, {530, 1720, 2825}, {0.00, 0.13, 1.86}},
    {1.90, 199.7, {530, 1720, 2848}, {0.00, 0.23, 1.57}},
    {1.90, 199.7, {530, 1720, 2872}, {0.00, 0.32, 1.29}},
    {1.90, 199.7, {530, 1720, 2895}, {0.00, 0.42, 1.00}},
    {1.90, 199.7, {530, 1720, 2919}, {0.00, 0.51, 0.71}},
    {1.90, 199.7, {530, 1720, 2942}, {0.00, 0.61, 0.43}},
    {1.90, 199.7, {530, 1720, 2965}, {0.00, 0.70, 0.14}},
    {1.90, 199.7, {530, 1720, 2970}, {0.00, 0.68, 0.00}},
    {1.90, 199.7, {530, 1720, 2955}, {0.00, 0.54, 0.00}},
    {1.90, 199.7, {530, 1720, 2940}, {0.00, 0.39, 0.00}},
    {1.90, 199.7, {530, 1720, 2926}, {0.00, 0.25, 0.00}},
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
  if (fx2_index[code - 200].count > 0) {
    contour2(s, &fx2_points[fx2_index[code - 200].first], fx2_index[code - 200].count, code);
    return;
  }
  if (fx_index[code - 200].count > 0) {
    contour(s, &fx_points[fx_index[code - 200].first], fx_index[code - 200].count,
            code);
    return;
  }
  switch (code) {
  case 240: dtmf(s, 215, 304); break; /* D0 */
  case 241: dtmf(s, 159, 275); break; /* D1 */
  case 242: dtmf(s, 159, 304); break; /* D2 */
  case 243: dtmf(s, 159, 335); break; /* D3 */
  case 244: dtmf(s, 175, 275); break; /* D4 */
  case 245: dtmf(s, 175, 304); break; /* D5 */
  case 246: dtmf(s, 175, 335); break; /* D6 */
  case 247: dtmf(s, 194, 275); break; /* D7 */
  case 248: dtmf(s, 194, 304); break; /* D8 */
  case 249: dtmf(s, 194, 335); break; /* D9 */
  case 250: dtmf(s, 215, 275); break; /* D10 */
  case 251: dtmf(s, 215, 335); break; /* D11 */
  case 238: c_noise(s, c8_db, (int)(sizeof(c8_db) / sizeof(c8_db[0]))); break; /* C8 */
  case 239: c_noise(s, c9_db, (int)(sizeof(c9_db) / sizeof(c9_db[0]))); break; /* C9 */

  case 253: { /* M1 */
    /* measured from the vox_test recording (see STEADY_LENGTH): 30ms of
     * noise with most of its energy under 2kHz, then noise around 1 to 2kHz
     * that dies away by 30dB, fastest at the start and the end. on both units
     * of the VoxRecord recording the hiss holds within 3dB of the bang for
     * 80ms, is 8dB down by 130ms, 13 by 200ms, 17 by 250ms and 27 by 290ms,
     * and stops at 300ms: these fades, where it had been faded sooner and
     * stopped at 250ms */
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
/* the noise refitted at the chip's own rate. the noise sounds were fitted
 * while this library ran at 8192 samples a second; at CHIP_RATE the same
 * values make different noise (it is a frequency jittered every sample, so
 * its spread, and where it folds back around half the rate, move with the
 * rate), and those sounds matched both units 1 to 15 points worse. so each
 * sound with noise was fitted again at CHIP_RATE, with changes to the noise
 * oscillators (4 and 5) in all its parts: a scale on their frequencies, a
 * change to their width (distortion) and the balance of their levels, against
 * both units of the VoxRecord recording with the background taken out and
 * clipped sounds left out. the sound's overall level is held where it was
 * (each sound is scored against its own loudest, so a fit left free to
 * change it made SE and others louder against the vowels, and the connected
 * groups, which nothing was fitted to, worse). each is kept only where it
 * matches both units better, by at least 0.05dB or half a point, and doesn't
 * match the second take (which nothing was fitted to) worse: TS 28 to 54%,
 * CH 67 to 83%, TT 29 to 45%, KE 62 to 72%, C9 51 to 58% on unit B. HO, WH,
 * FF, SO, SH, TH, R5, R8, A7, A9, B0, B1, B2, B5, B8, C5, M0 and M1 (clipped
 * on both units) keep theirs */
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

/* the consonants shaped further than the oscillators can. on both units of
 * the VoxRecord recording, measured at full band, each consonant's spectrum
 * differed from the chip's by much the same shape every time: taken that
 * shape away, what is left is no more than the two units differ from each
 * other (GO 5.4 to 0.7dB, ED 4.2 to 0.9, SE 5.2 to 1.3, against 1.5, 1.8 and
 * 2.0 between the units). so each is played through peaking filters half an
 * octave apart (see synth_eq()), their gains fitted to that recording, and
 * the voiceless ones whose hiss is broader on the chip than oscillators 4 and
 * 5 make (SH's runs flat from 100Hz to 2kHz, 15dB above here) get broadband
 * noise as well, at the level that fitted best of 0, 0.15, 0.3 and 0.6 of
 * theirs. an overall gain then puts each back at the level it had, which was
 * fitted among the vowels (louder, SE lost 3 to 5 points next to AW and in
 * Man Goes Down). the gains glide with the frames from one sound into the
 * next.
 *
 * on the fourth VoxProbe recording, left out: GO 0.839 to 0.869 and 4.78 to
 * 2.61dB, ED 4.30 to 1.87dB, OD 2.53 to 1.34dB, ZZ 1.95 to 1.11dB, BO 0.775
 * to 0.791 and 2.28 to 1.12dB, KO 0.850 to 0.870 (the mean of six noise
 * sequences: a hiss's match moves 2 to 4 points with the sequence alone), and
 * BE EH and EH DE a point closer. CH, SO, TT and OK fitted to nothing; EB,
 * SH, TS and KE came out worse there, and SE, its spectrum 5.4 to 2.0dB, lost
 * 3 to 5 points of spectrogram match on every recording, so they have none.
 * the code, the filters' gains, the broadband noise and the overall gain */
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
    {229, {-0.08, -2.33, -0.64, 0.61, -0.56, -3.06, -6.37, -0.93, -2.47, -0.34}, 0.00, 0.00}, /* B9 */
    {230, {1.3, 1.13, 1.19, 1.05, 0.17, 0.17, 0.43, 0.45, -0.4, -1.2}, 0.00, 0.00}, /* C0 */
    {231, {-0.06, 0.8, 0.29, -0.66, -0.65, -0.69, -0.75, -1.03, 0.87, 0.69}, 0.00, 0.00}, /* C1 */
    {232, {-1.92, -0.33, 0.08, -0.79, 0.16, 1.24, -2.94, -0.58, 0.77, 1.03}, 0.00, 0.00}, /* C2 */
    {233, {-0.84, -2.06, 0.02, 1.51, -0.34, -0.26, 0.96, -2.55, 1.29, 0.49}, 0.00, 0.00}, /* C3 */
    {234, {-2.46, -1.03, -0.03, -0.99, -1.24, 1.77, -3.81, 2.5, 2.32, 0.64}, 0.00, 0.00}, /* C4 */
    {235, {14.23, -0.52, -5.23, -0.72, -1.0, -3.16, 1.87, 5.76, -5.96, -5.58}, 0.00, 0.00}, /* C5 */
    {236, {-0.09, -0.09, -0.13, -0.9, -1.22, -0.65, -0.38, 0.09, 0.68, 0.45}, 0.00, 0.00}, /* C6 */
    {237, {1.96, 1.45, 1.34, 1.05, -0.13, 0.23, 1.21, 2.88, 0.37, -2.78}, 0.00, 0.00}, /* C7 */
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

static bool lookup(uint8_t code, sound *s) {
  memset(s, 0, sizeof(*s));
  if (code >= 128 && code < 128 + NUM_ALLOPHONES) {
    allophone(s, code);
    /* see refit_length */
    s->length *= refit_length[code - 128];
    const double g = pow(10, refit_gain_db[code - 128] / 20);
    for (int i = 0; i < s->nparts; i++)
      for (int j = 0; j < 5; j++)
        s->parts[i].target.vol[j] *= g;
    sound_shaping(s, code);
    s->no_slow = code >= 182; /* the voiceless consonants (see msa_expand()) */
  } else if (code >= 200 && code < 200 + NUM_EFFECTS) {
    effect(s, code);
    sound_shaping(s, code);
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
 * (oscillators 4 and 5) doesn't move at all. in the chip's frequency steps
 * (FREQ_STEP_HZ) a Bend is 10 steps, and the short step from 4 to 5 is 5 */
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


/* the voicing frequency for a Pitch setting. the chip counts each voicing
 * period in whole ticks of a VOICE_TICK_HZ clock: 8000 / Pitch ticks
 * (dividing in whole numbers) and one more. on the second VoxProbe recording,
 * which plays every Pitch from 0 to 254, the frequency holds flat over runs
 * of Pitch settings and steps exactly where that count changes (201 to 205
 * all give 199.7Hz), and at 12 settings the count is one more again, measured
 * consistently on both halves of each test; no simple arithmetic gives those,
 * so they are listed.
 *
 * not emulated: at very low pitch the chip also starts each sound (and each
 * copy of a repeated one) with a pulse, so pitch 0 gives a pulse a sound; but
 * at pitch 33 and 38 the pulses run on evenly across a repeat, and what
 * decides it isn't known. here pitch 0 is silent */
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

    /* fast and slow, measured from the VoxProbe recording: fast plays OWIY and
     * AXRR at 0.66 and 0.63 of their length, and slow at 1.42 and 1.31 times.
     * slow doesn't lengthen the voiceless consonants: the vox_test recording
     * has a slow SE 1.11 times and a slow TH 0.95 times its usual length, and
     * in Man Goes Down (both AtariVoxes) Slow SE Slow SE is 200ms, Slow TT's
     * burst 20ms and Slow Slow TH 50ms, where stretching them as the vowels
     * are made them 290, 60 and 70ms. its vowels, DH and GO stretch as here.
     * it takes that game's second and fourth phrases from 0.75 to 0.86 and
     * 0.63 to 0.80 (both units, full band) */
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
    /* a tone Bend takes under nothing wraps round: on both units of the
     * fourth VoxProbe recording, at bend 0 A1's third formant, which starts
     * at 120Hz, isn't 197Hz lower but mirrored, a line falling from 2.4kHz as
     * the formant rises, reaching nothing and rising again as the formant
     * passes 2.5kHz, and its first formant is a steady line near 2.4kHz.
     * both are the tone WRAP_HZ higher, played at the chip's rate (which
     * folds it under half that rate), and it stays there as the effect goes
     * on. a tone of an effect quieter than WRAP_MIN_VOL is one its trace
     * couldn't read, its frequency borrowed from a neighbour (see contour()),
     * and doesn't wrap: wrapping those put a line near 2.4kHz into C0, C3
     * and C7 at bend 0 that neither unit has */
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
    /* Bend moves an effect's noise as well: on both units of the fourth
     * VoxProbe recording C8's and C9's noise moves about 200Hz down at bend 0
     * and 440Hz up at bend 15, as their tones do. it moves as a block, its
     * width unchanged: at bend 15 nothing is left under 560Hz within 20dB,
     * where moving the oscillators, whose spread is in proportion to their
     * frequency, widened it and left the noise reaching down to 300Hz. so the
     * Bend is added after the spread (see synth_sample()): C8 and C9 at bend
     * 15 10.0 to 8.8 and 10.0 to 7.5dB, and on the first VoxProbe recording,
     * left out, C8 at bend 0 3.2 to 3.0 and at bend 15 9.8 to 8.6dB. whether
     * the hiss of speech moves too isn't measured, so it doesn't here */
    if (!s->speech)
      t.noise_shift = bend_hz;
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

/* a pause before a stop takes the place of that much of its closure: on both
 * AtariVoxes playing Man Goes Down, UX, pause 1, EK leaves 140ms between the
 * UX and EK's burst, where the pause (107ms) and EK's closure (105ms) one
 * after the other left 226ms, and TT, pause 1, BE starts BE's voicing as the
 * pause ends. the closure keeps MIN_CLOSURE_MS, which gives the 140ms. after a
 * sound the closure is all there (AW TT on the fourth VoxProbe recording).
 * the two phrases go from 0.75 to 0.81 and 0.63 to 0.68 (both units, full
 * band), and with the change to Slow (see msa_expand()) to 0.875 */
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
 * alone, and their first and last frames are the voicing coming up from
 * silence and dying away. between two sounds the chip doesn't do that: in Man
 * Goes Down, OWWW ZZ is voiced throughout on both AtariVoxes, where those
 * frames left a gap of 25 to 35ms. so after a voiced sound the frames before
 * the first at FRAMED_JOIN of the sound's loudest take its volumes, and before
 * another sound the frames after the last at FRAMED_JOIN take that one's. the
 * timing is unchanged. "man goes down" 0.824 to 0.833 (both units, full
 * band); nothing on the VoxProbe or VoxRecord recordings joins these sounds
 * to others, and none of their scores moves */
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
