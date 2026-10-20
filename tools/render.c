/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 256

/* a phrase never runs longer than this; stops a runaway (e.g. Repeat 255 of a
 * long effect, many times over) eating memory */
#define MAX_SECONDS 600

typedef struct pcm_buffer {
  int16_t *data;
  size_t n, cap;
  bool failed;
} pcm_buffer;

static void render_chunk(sj_chip *sj, pcm_buffer *b, double gain, bool *sounding) {
  float f[CHUNK];
  *sounding = sj_render(sj, f, CHUNK);

  if (b->failed)
    return;
  if (b->n + CHUNK > b->cap) {
    const size_t cap = b->cap ? b->cap * 2 : 8192;
    int16_t *d = (int16_t *)realloc(b->data, cap * sizeof(int16_t));
    if (d == NULL) {
      b->failed = true;
      return;
    }
    b->data = d;
    b->cap = cap;
  }
  for (int i = 0; i < CHUNK; i++) {
    double v = (double)f[i] * gain;
    v = v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v;
    b->data[b->n++] = (int16_t)lround(v * 32767.0);
  }
}

size_t sjr_render(const uint8_t *codes, size_t n, double gain, int16_t **pcm) {
  sj_chip *sj = sj_new();
  pcm_buffer b = {NULL, 0, 0, false};
  bool sounding = false;
  const size_t limit = (size_t)MAX_SECONDS * SJ_SAMPLE_RATE;

  *pcm = NULL;
  if (sj == NULL)
    return 0;

  for (size_t i = 0; i < n && !b.failed && b.n < limit; i++) {
    const uint8_t code = codes[i];
    uint8_t data = 0;

    if (sj_command_size(code) == 2) {
      if (i + 1 >= n)
        break; /* a command missing its data byte */
      data = codes[++i];
    } else if (code == 255) {
      continue;
    }

    /* the buffer is full, so play until there is room */
    while (!sj_write(sj, code, data) && !b.failed && b.n < limit)
      render_chunk(sj, &b, gain, &sounding);
  }

  do
    render_chunk(sj, &b, gain, &sounding);
  while ((sounding || sj_buffered(sj) > 0) && !b.failed && b.n < limit);

  sj_free(sj);
  if (b.failed) {
    free(b.data);
    return 0;
  }
  *pcm = b.data;
  return b.n;
}

static void put16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(unsigned char *p, unsigned long v) {
  put16(p, (unsigned)(v & 0xffff));
  put16(p + 2, (unsigned)((v >> 16) & 0xffff));
}

bool sjr_write_wav(const char *path, const int16_t *pcm, size_t n) {
  unsigned char h[44];
  const unsigned long bytes = (unsigned long)n * 2;

  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1); /* PCM */
  put16(h + 22, 1); /* mono */
  put32(h + 24, SJ_SAMPLE_RATE);
  put32(h + 28, SJ_SAMPLE_RATE * 2);
  put16(h + 32, 2);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put32(h + 40, bytes);

  FILE *f = fopen(path, "wb");
  if (f == NULL)
    return false;
  bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
  for (size_t i = 0; i < n && ok; i++) {
    unsigned char s[2];
    put16(s, (unsigned)(uint16_t)pcm[i]);
    ok = fwrite(s, 1, 2, f) == 2;
  }
  return fclose(f) == 0 && ok;
}

size_t sjr_read_wav(const char *path, int16_t **pcm) {
  unsigned char h[44];
  *pcm = NULL;

  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return 0;
  if (fread(h, 1, sizeof(h), f) != sizeof(h) || memcmp(h, "RIFF", 4) != 0 ||
      memcmp(h + 8, "WAVEfmt ", 8) != 0 || memcmp(h + 36, "data", 4) != 0) {
    fclose(f);
    return 0;
  }

  const size_t n = ((size_t)h[40] | (size_t)h[41] << 8 | (size_t)h[42] << 16 |
                    (size_t)h[43] << 24) / 2;
  int16_t *d = (int16_t *)malloc((n ? n : 1) * sizeof(int16_t));
  size_t i = 0;
  if (d != NULL) {
    unsigned char s[2];
    for (; i < n && fread(s, 1, 2, f) == 2; i++)
      d[i] = (int16_t)(uint16_t)(s[0] | s[1] << 8);
  }
  fclose(f);
  if (d == NULL || i != n) {
    free(d);
    return 0;
  }
  *pcm = d;
  return n;
}
