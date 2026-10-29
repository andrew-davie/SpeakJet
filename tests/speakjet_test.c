/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * tests for the C port. the first four follow speakjet_test.go; the rest
 * cover what the C version adds. speakjet.c is included so that the tests
 * can reach its internals, as the Go tests do
 */

#include "../speakjet.c"

#include <stdio.h>

static int failures = 0;

#define FAIL(...)                                                              \
  do {                                                                         \
    printf("FAIL %s: ", __func__);                                             \
    printf(__VA_ARGS__);                                                       \
    printf("\n");                                                              \
    failures++;                                                                \
  } while (0)

/* every sound code plays something, and the allophones' table lengths are
 * within the range the chip was measured to play (30 to 300 ms at Speed 114) */
static void test_sounds(void) {
  for (int code = 128; code <= 199; code++) {
    const allophone *a = &allophones[code - 128];
    if (a->ms * MS_STEP < 10 || a->ms * MS_STEP > 320)
      FAIL("code %d (%s): length %d ms", code, sj_sound_name((uint8_t)code), a->ms * MS_STEP);
  }
  for (int code = 200; code <= 254; code++) {
    int ms = 0;
    for (int i = 0; i < fxindex[code - 200][1]; i++)
      ms += fxparts[fxindex[code - 200][0] + i].ms * MS_STEP;
    if (ms <= 0)
      FAIL("code %d (%s): no length", code, sj_sound_name((uint8_t)code));
  }
}

/* render runs the chip until it falls silent, returning the sample count */
static size_t render(sj_chip *sj, float **out) {
  static float all[256 * 10000];
  size_t n = 0;
  for (int i = 0; i < 10000; i++) {
    const bool sounding = sj_render(sj, all + n, 256);
    n += 256;
    if (!sounding && sj_buffered(sj) == 0)
      break;
  }
  if (out != NULL)
    *out = all;
  return n;
}

static void test_phrase_length(void) {
  sj_chip *sj = sj_new();
  const uint8_t phrase[] = {183, 135, 199}; /* HO OH PO */
  for (size_t i = 0; i < sizeof(phrase); i++)
    sj_write(sj, phrase[i], 0);

  /* count samples one at a time until the chip stops sounding */
  float buf[1];
  int n = 0;
  while (sj_render(sj, buf, 1))
    n++;

  const double got = (double)n * 1000 / SJ_SAMPLE_RATE;
  /* the sounds' lengths, the stop's closure, and the two sounds followed at
   * once by another cut short by the overlap */
  const double want = (allophones[183 - 128].ms + allophones[135 - 128].ms + allophones[199 - 128].ms) * MS_STEP +
                      T.closure - 2 * T.overlap;
  if (fabs(got - want) > 10)
    FAIL("phrase played for %.0fms, expected about %.0fms", got, want);
  sj_free(sj);
}

static void test_buffer(void) {
  sj_chip *sj = sj_new();
  for (int i = 0; i < SJ_BUFFER_SIZE; i++) {
    if (!sj_write(sj, 128, 0)) {
      FAIL("buffer refused byte %d", i);
      break;
    }
    if (sj_buffer_half_full(sj) != (i + 1 > SJ_BUFFER_HALF_FULL))
      FAIL("after %d bytes buffer half full is %d", i + 1, sj_buffer_half_full(sj));
  }
  if (sj_write(sj, 128, 0))
    FAIL("buffer accepted a 65th byte");
  if (sj_write(sj, 21, 100))
    FAIL("buffer accepted a two byte command when full");

  /* draining the buffer clears the status line */
  render(sj, NULL);
  if (sj_buffer_half_full(sj) || sj_buffered(sj) != 0)
    FAIL("buffer not empty after rendering: %d bytes", sj_buffered(sj));
  sj_free(sj);
}

static void test_output_range(void) {
  sj_chip *sj = sj_new();
  for (int c = 128; c <= 254; c++) {
    sj_write(sj, (uint8_t)c, 0);
    float *out;
    const size_t n = render(sj, &out);
    for (size_t i = 0; i < n; i++) {
      if (isnan(out[i]) || out[i] > 1.5f || out[i] < -1.5f) {
        FAIL("code %d: sample out of range: %f", c, out[i]);
        break;
      }
    }
  }
  sj_free(sj);
}

/* the byte stream gives the same sound as the commands written directly */
static void test_write_byte(void) {
  const uint8_t stream[] = {20, 110, 21, 90, 22, 120, 26, 3, 128, 30, 5, 199};
  sj_chip *a = sj_new();
  sj_chip *b = sj_new();

  for (size_t i = 0; i < sizeof(stream); i++)
    sj_write_byte(a, stream[i]);
  sj_write(b, 20, 110);
  sj_write(b, 21, 90);
  sj_write(b, 22, 120);
  sj_write(b, 26, 3);
  sj_write(b, 128, 0);
  sj_write(b, 30, 5);
  sj_write(b, 199, 0);

  if (sj_buffered(a) != sj_buffered(b))
    FAIL("buffered %d, expected %d", sj_buffered(a), sj_buffered(b));

  float x[512], y[512];
  for (int k = 0; k < 40; k++) {
    sj_render(a, x, 512);
    sj_render(b, y, 512);
    if (memcmp(x, y, sizeof(x)) != 0) {
      FAIL("byte stream output differs from command output");
      break;
    }
  }

  /* a two byte command is held until its data byte arrives */
  sj_power_on(a);
  sj_write_byte(a, 21);
  if (sj_buffered(a) != 0)
    FAIL("half a command reached the buffer");
  sj_write_byte(a, 50);
  if (sj_buffered(a) != 2)
    FAIL("completed command not buffered: %d", sj_buffered(a));

  /* reset drops a half received command */
  sj_write_byte(a, 22);
  sj_reset(a);
  sj_write_byte(a, 128);
  sj_controls c;
  sj_get_controls(a, &c);
  if (c.pending_code != -1)
    FAIL("pending command survived reset");

  sj_free(a);
  sj_free(b);
}

static void test_controls(void) {
  sj_chip *sj = sj_new();
  sj_controls c;

  sj_get_controls(sj, &c);
  if (c.volume != SJ_DEFAULT_VOLUME || c.speed != SJ_DEFAULT_SPEED ||
      c.pitch != SJ_DEFAULT_PITCH || c.bend != SJ_DEFAULT_BEND ||
      c.pending_code != -1)
    FAIL("power-on controls are not the defaults");

  /* controls set by commands are reported once executed */
  const uint8_t stream[] = {20, 50, 21, 60, 22, 70, 23, 8, 26, 4, 7, 14, 23};
  for (size_t i = 0; i < sizeof(stream); i++)
    sj_write_byte(sj, stream[i]);
  float buf[16];
  sj_render(sj, buf, 16);
  sj_get_controls(sj, &c);
  if (c.volume != 50 || c.speed != 60 || c.pitch != 70 || c.bend != 8 ||
      c.repeat != 4 || !c.fast || !c.stress || c.slow || c.relax ||
      c.pending_code != 23)
    FAIL("controls not as commanded");

  /* and restored by set */
  sj_chip *other = sj_new();
  sj_set_controls(other, &c);
  sj_controls d;
  sj_get_controls(other, &d);
  if (c.volume != d.volume || c.speed != d.speed || c.pitch != d.pitch ||
      c.bend != d.bend || c.repeat != d.repeat || c.fast != d.fast ||
      c.slow != d.slow || c.stress != d.stress || c.relax != d.relax ||
      c.pending_code != d.pending_code)
    FAIL("controls did not round trip");

  /* out of range values are clamped, a bad pending code dropped */
  c.volume = 255;
  c.speed = 255;
  c.bend = 99;
  c.pending_code = 128;
  sj_set_controls(other, &c);
  sj_get_controls(other, &d);
  if (d.volume != 127 || d.speed != 127 || d.bend != 15 || d.pending_code != -1)
    FAIL("controls not clamped");

  sj_free(other);
  sj_free(sj);
}

static void test_names(void) {
  if (strcmp(sj_sound_name(128), "IY") != 0 || strcmp(sj_sound_name(199), "PO") != 0 ||
      strcmp(sj_sound_name(200), "R0") != 0 || strcmp(sj_sound_name(254), "M2") != 0)
    FAIL("wrong names at the ends of the tables");
  if (sj_sound_name(127) != NULL || sj_sound_name(255) != NULL)
    FAIL("names for codes that are not sounds");
}

int main(void) {
  test_sounds();
  test_phrase_length();
  test_buffer();
  test_output_range();
  test_write_byte();
  test_controls();
  test_names();

  if (failures != 0) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
