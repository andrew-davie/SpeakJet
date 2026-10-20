/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * renders every phrase in samples/codes.txt and compares it with the WAV
 * beside it, so any change to the sound shows up. run from the repository
 * root (make test does), or pass the samples directory
 *
 * the tolerance allows for floating point differing in its last bits between
 * compilers and maths libraries; a real change to the sound is far larger
 */

#include "../tools/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOLERANCE 2

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "samples";
  char path[1024];
  snprintf(path, sizeof(path), "%s/codes.txt", dir);

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    printf("FAIL: can't open %s\n", path);
    return 1;
  }

  int phrases = 0, failures = 0;
  char line[8192];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (line[0] == '#' || line[0] == '\n')
      continue;
    char *colon = strchr(line, ':');
    if (colon == NULL)
      continue;
    *colon = '\0';
    const char *name = line;

    /* the codes after the name */
    uint8_t codes[4096];
    size_t n = 0;
    for (char *p = colon + 1; *p && n < sizeof(codes);) {
      char *end;
      const long v = strtol(p, &end, 10);
      if (end == p) {
        p++;
        continue;
      }
      codes[n++] = (uint8_t)v;
      p = end;
    }

    int16_t *got = NULL, *want = NULL;
    const size_t ngot = sjr_render(codes, n, SJR_DEFAULT_GAIN, &got);
    snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
    const size_t nwant = sjr_read_wav(path, &want);
    phrases++;

    if (want == NULL) {
      printf("FAIL %s: can't read %s\n", name, path);
      failures++;
    } else if (ngot != nwant) {
      printf("FAIL %s: %zu samples, reference has %zu\n", name, ngot, nwant);
      failures++;
    } else {
      int worst = 0;
      size_t at = 0;
      for (size_t i = 0; i < ngot; i++) {
        const int d = abs(got[i] - want[i]);
        if (d > worst) {
          worst = d;
          at = i;
        }
      }
      if (worst > TOLERANCE) {
        printf("FAIL %s: sample %zu differs by %d (tolerance %d)\n", name, at,
               worst, TOLERANCE);
        failures++;
      }
    }
    free(got);
    free(want);
  }
  fclose(f);

  if (phrases == 0) {
    printf("FAIL: no phrases in %s/codes.txt\n", dir);
    return 1;
  }
  if (failures != 0) {
    printf("%d of %d reference phrase(s) failed\n", failures, phrases);
    return 1;
  }
  printf("ok: %d reference phrases match\n", phrases);
  return 0;
}
