/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * speakjet: a clean room emulation of the SpeakJet speech and sound
 * synthesizer used in the AtariVox.
 *
 * Written by Claude, Anthropic's AI model, under the direction of Andrew
 * Davie. See "How it was made" in README.md.
 *
 * docs/design.md says what is taken from the SpeakJet User's Manual
 * and what is guessed.
 *
 *   - sj_write_byte() takes the raw serial byte stream and frames the two
 *     byte commands itself
 *   - sj_get_controls()/sj_set_controls() for save states
 *   - sj_power_on() to return the whole chip, sound included, to its
 *     power-on state
 *
 * A chip is NOT safe to use from more than one thread at once: the caller
 * must serialise writes and renders. Different chips are independent; the
 * library has no global state.
 *
 * The library is plain C99 and needs only libm.
 */

#ifndef SPEAKJET_H
#define SPEAKJET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* library version */
#define SJ_VERSION_MAJOR 1
#define SJ_VERSION_MINOR 0
#define SJ_VERSION_PATCH 0
#define SJ_VERSION "1.0.0"

/* the rate at which samples are produced. from the manual: "The frequency is
 * produced at a rate of 8192 samples per second" */
#define SJ_SAMPLE_RATE 8192

/* the size of the input buffer, in bytes, and the level above which the
 * Buffer Half Full status line is set. from the manual */
#define SJ_BUFFER_SIZE 64
#define SJ_BUFFER_HALF_FULL 32

typedef struct sj_chip sj_chip;

/* create a chip in its power-on state. returns NULL if out of memory */
sj_chip *sj_new(void);
void sj_free(sj_chip *sj);

/* return the chip to its power-on state: buffer emptied, controls at their
 * defaults and the synthesizer silent */
void sj_power_on(sj_chip *sj);

/* the number of bytes a command takes, including its data byte */
int sj_command_size(uint8_t code);

/* write a command to the input buffer. the data byte is used only by
 * commands that take one. returns false if the buffer is too full to take
 * the command, in which case it is lost */
bool sj_write(sj_chip *sj, uint8_t code, uint8_t data);

/* write one byte of the serial command stream. a command that takes a data
 * byte is held until its data byte arrives. returns false if a completed
 * command was lost to a full buffer */
bool sj_write_byte(sj_chip *sj, uint8_t byte);

/* the state of the Buffer Half Full status line: true when the buffer holds
 * more than SJ_BUFFER_HALF_FULL bytes */
bool sj_buffer_half_full(const sj_chip *sj);

/* the state of the Speaking status line, as of the last sj_render() */
bool sj_speaking(const sj_chip *sj);

/* bytes in the input buffer, including the command that is executing */
int sj_buffered(const sj_chip *sj);

/* reset the chip as though the reset line had been cycled: the buffer is
 * cleared and the controls return to their defaults. the sound already
 * playing is not cut short. a half received two byte command is dropped */
void sj_reset(sj_chip *sj);

/* fill buf with the next n samples of output, at SJ_SAMPLE_RATE, in the
 * range -1 to 1. returns true if the chip is producing sound */
bool sj_render(sj_chip *sj, float *buf, size_t n);

/* the mnemonic of an allophone (128 to 199) or sound effect (200 to 254),
 * e.g. "IY" or "M1". NULL for any other code */
const char *sj_sound_name(uint8_t code);

/* the controls and modifiers that apply to the sounds still to come. these
 * are what a save state needs to keep; sound already playing and the input
 * buffer are not part of it */
typedef struct sj_controls {
  uint8_t volume; /* 0 to 127 */
  uint8_t speed;  /* 0 to 127 */
  uint8_t pitch;  /* 0 to 255 */
  uint8_t bend;   /* 0 to 15 */

  /* repeat count for the next sound (0 or 1 for no repeat) */
  uint8_t repeat;

  /* modifiers for the next sound */
  bool fast, slow, stress, relax;

  /* a two byte command waiting for its data byte in sj_write_byte(), or -1 */
  int pending_code;
} sj_controls;

/* the defaults the chip starts with and returns to on reset */
#define SJ_DEFAULT_VOLUME 96
#define SJ_DEFAULT_SPEED 114
#define SJ_DEFAULT_PITCH 88
#define SJ_DEFAULT_BEND 5

void sj_get_controls(const sj_chip *sj, sj_controls *c);

/* values are clamped to the ranges above; an invalid pending_code is
 * dropped */
void sj_set_controls(sj_chip *sj, const sj_controls *c);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKJET_H */
