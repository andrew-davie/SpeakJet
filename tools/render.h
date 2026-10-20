/*
 * This file is part of the SpeakJet C-Library package.
 * see https://github.com/andrew-davie/SpeakJet
 *
 * Copyright (c) 2026 Andrew Davie
 * SPDX-License-Identifier: MIT
 */

/*
 * render: play a whole phrase through a chip and collect the result as 16 bit
 * PCM. shared by sjwav, the sample generator and the reference test, so all
 * three render the same way. not part of the library
 */

#ifndef SPEAKJET_RENDER_H
#define SPEAKJET_RENDER_H

#include "../speakjet.h"

#include <stddef.h>
#include <stdint.h>

/* the library's output peaks at about 0.36 at full Volume; this brings it
 * close to full scale without clipping */
#define SJR_DEFAULT_GAIN 2.5

/* render codes, a phrase as the serial byte stream, until the chip falls
 * silent. byte 255 outside a command is skipped as an end-of-phrase marker.
 * a phrase longer than the input buffer is fed in as the buffer drains.
 * returns the number of samples, with *pcm malloc'd (NULL if out of memory) */
size_t sjr_render(const uint8_t *codes, size_t n, double gain, int16_t **pcm);

/* write 16 bit mono samples at SJ_SAMPLE_RATE. returns false on error */
bool sjr_write_wav(const char *path, const int16_t *pcm, size_t n);

/* read a file written by sjr_write_wav(). returns the number of samples, with
 * *pcm malloc'd, or 0 with *pcm NULL if it can't be read */
size_t sjr_read_wav(const char *path, int16_t **pcm);

#endif /* SPEAKJET_RENDER_H */
