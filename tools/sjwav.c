/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * sjwav: render SpeakJet codes to a WAV file
 *
 *   sjwav -o out.wav 183 135 199          codes as arguments
 *   echo "183, 135, 199" | sjwav -o out.wav     decimal codes on stdin
 *   sjwav -b -o out.wav < phrase.bin      raw bytes on stdin
 *
 * Output is 16 bit mono at 8192 Hz. Byte 255 is taken as an end-of-phrase
 * marker and skipped, as text2speakjet writes one after each phrase.
 */

/* isatty() and fileno() are POSIX, not C99 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "../speakjet.h"
#include "render.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define stdin_is_terminal() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define stdin_is_terminal() isatty(fileno(stdin))
#endif

static void usage(void) {
  fprintf(stderr,
          "usage: sjwav [-o out.wav] [-g gain] [-b] [code ...]\n"
          "  codes are decimal 0-255, as arguments or on stdin (comma or space\n"
          "  separated); -b reads raw bytes from stdin instead\n"
          "  -o  output file (default speakjet.wav)\n"
          "  -g  output gain (default %.1f)\n",
          SJR_DEFAULT_GAIN);
  exit(2);
}

/* append one code, growing the array as needed */
static void push(uint8_t **codes, size_t *n, size_t *cap, long v) {
  if (v < 0 || v > 255) {
    fprintf(stderr, "sjwav: code %ld out of range 0-255\n", v);
    exit(1);
  }
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 256;
    *codes = (uint8_t *)realloc(*codes, *cap);
    if (*codes == NULL) {
      fprintf(stderr, "sjwav: out of memory\n");
      exit(1);
    }
  }
  (*codes)[(*n)++] = (uint8_t)v;
}

static void parse_text(const char *s, uint8_t **codes, size_t *n, size_t *cap) {
  while (*s) {
    if (isdigit((unsigned char)*s)) {
      char *end;
      push(codes, n, cap, strtol(s, &end, 10));
      s = end;
    } else if (*s == ',' || isspace((unsigned char)*s)) {
      s++;
    } else {
      fprintf(stderr, "sjwav: unexpected '%c' in codes\n", *s);
      exit(1);
    }
  }
}

int main(int argc, char **argv) {
  const char *out = "speakjet.wav";
  double gain = SJR_DEFAULT_GAIN;
  bool binary = false;
  uint8_t *codes = NULL;
  size_t n = 0, cap = 0;
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      out = argv[++i];
    else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
      gain = atof(argv[++i]);
    else if (strcmp(argv[i], "-b") == 0)
      binary = true;
    else
      usage();
  }

  if (i < argc) {
    for (; i < argc; i++)
      parse_text(argv[i], &codes, &n, &cap);
  } else if (stdin_is_terminal()) {
    usage();
  } else if (binary) {
    int c;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    while ((c = getchar()) != EOF)
      push(&codes, &n, &cap, c);
  } else {
    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL)
      parse_text(line, &codes, &n, &cap);
  }

  if (n == 0)
    usage();

  int16_t *pcm = NULL;
  const size_t samples = sjr_render(codes, n, gain, &pcm);
  if (pcm == NULL) {
    fprintf(stderr, "sjwav: out of memory\n");
    return 1;
  }
  if (!sjr_write_wav(out, pcm, samples)) {
    fprintf(stderr, "sjwav: can't write %s\n", out);
    return 1;
  }
  fprintf(stderr, "%s: %.2fs\n", out, (double)samples / SJ_SAMPLE_RATE);

  free(pcm);
  free(codes);
  return 0;
}
