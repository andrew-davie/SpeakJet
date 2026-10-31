# Changelog

## Rebuild (branch `rebuild`)

Written from scratch from the SpeakJet User's Manual and recordings of two
AtariVoxes alone. The brief was the smallest footprint: rules wherever a rule could be found, a few
bytes of parameters per sound otherwise.

### 2026-11-01

- **Reading ahead.** The chip takes the command after a sound early, at the
  sound's release, and a Speed or Volume read so changes the rest of the sound
  (VoxProbe 8, both units). The library now does the same. Phrases with a Speed
  or Volume right after a sound change; nothing else does.
- **Pauses** P1 and P4 are 90 and 15 ms, the gaps the chip leaves (VoxProbe 8);
  Man Goes Down P 2.90 -> 2.74 on its phrases with pauses.
- **Effects' Speed law** (147 - Speed) / 33 (was (149 - Speed) / 35): fitted on
  every effect recording at another Speed; Man Goes Down P3 (Speed 62) P 2.71 -> 2.46.
- **Lengths** refitted for ED, OHIH, EHLL, WH, EYIY, OWIY and CH (the glides ran
  10-20 ms long).
- **Effects rebuilt from their own tracks** on the clean recordings: 210 A0 (one
  gliding voice under two crossing tones), 211 A1 and 212 A2 (their lines' curves),
  208 R8 (its low tail). Tables 2666 bytes; the size budget is now 3000.
- **The effects' bed** levels off above about 1.5 kHz (the signal's change per
  tick smoothed by one pole): 220 B0, 221 B1 and 228 B8 had 6-9 dB too much,
  the DTMF digits keep theirs. Effects P 1.190 -> 1.178; speech unchanged.
- **The output stage is now the analogue circuit**, solved exactly on the held
  samples, in place of a bilinear digital filter on their per-sample average,
  whose warping and droop put the images 1-8 dB low from 5 to 14 kHz (every
  effect, both units). Every sound improves: all 1382 items P 1.053 -> 0.966,
  allophones 0.882 -> 0.808, Man Goes Down 1.633 -> 1.569.

### 2026-10-30 to 31

- **Tables back to 2.6 KB.** Each effect part is now a byte stream: its
  length, a byte saying which fields follow, its flags and level, then only
  the tones and fields it uses (noise, gate, pulse offsets, level ramp, end
  levels). The voiced stops' release targets are a five-entry side table.
  Compiled tables 2606 bytes (allophones 1008, effects 1578); the output is
  unchanged to the sample. They had reached 5.1 KB with a fixed column per
  field.
- **The voice is exactly periodic** (both units: the period holds to one 48
  kHz sample and the gaps between harmonics sit 30-60 dB under them). The
  formant jitter and the voiced noise floor are gone; they had filled those
  gaps to 15-30 dB.
- **Output stage** at two poles, 1609 Hz, Q 0.462, fitted to tones the chip
  sets directly (the DTMF pairs and VoxProbe 7's single oscillator at 125,
  1000 and 3000 Hz), all within 0.45 dB on both units.
- **Repeat.** A stop's copy is the stop again: a 20 ms closure, then its
  release, each copy lasting the stop's measured copy length (BE 75, BO 70,
  DE 60, DO 55, GE and GO 65 ms...); BE, BO, DE and DO keep their voice bar
  through it. A glide's copy glides back to its first frame instead of
  jumping. JH's copies are 80 ms, ZH's 70, the glides' 185-290, all
  measured.
- **Voiced stops** hum through their closure on the first formant, open on a
  voice bar 2-9 dB under the release, and release toward targets of their
  own (BO, EB, OB, ED, OD).
- **Effects read off the chip.** Most effects were rebuilt from the chip's
  own tracks: the robots as a carrier beside a low tone and its octave
  under a sine or square envelope; the C series pulsing at their own rate
  (Pitch plus an offset of their own, measured at four Pitches); C8 and C9's
  sweeping noise band; M0's wandering tone; A0 as a voice under two plain
  tones. The envelope oscillator's four waveforms are measured (the
  triangle is a ramp, then an instant drop). Plain tones hold their level
  and start in 1 ms; DTMF tones hold 100 ms at one level and fade over 55,
  as every unit does; M2's upper tones are at full level.
- **Fitting.** Every sound's numbers were then annealed inside its measured
  structure on two scores (E, the spectrum line by line; P, length, level
  over time and the time-frequency picture), each compared with how far the
  two real chips are from each other, and shipped only when one gained
  without the other losing. A new field is given to the anneal only where
  the chip shows it: free level ramps and end levels once invented level
  changes on 18 effects the chip holds flat, and were removed.
- **Tests and build.** A test fails if the compiled tables pass a 2700-byte
  budget (C and Go); the unused table of the manual's lengths is gone and the
  build is warning-free. The README's and text2speakjet's examples show the
  codes the tool gives now.
- **Still open.** Phrase timing: Slow seems to stretch vowels and diphthongs
  more (about x1.3-1.5) and hisses less than the single x1.22 the library
  uses, so long phrases drift; it needs measuring sound by sound. The
  effects' noise bed doesn't follow the chip's (steady tones carry far less
  than effects do). The nasals' pulse shape at low Pitch, and a few effects
  (R5, R8, B1, B8) whose structure the library can't yet match.

- Every effect has a measured level against the voice (a part byte), the
  effects' gate dips 8 dB rather than cutting, and the voice's pulse
  starts downward as the chip's does.
- VoxProbe 6 on both units: every sound has a measured level byte (the
  consonants were 5 to 26 dB too loud), Volume 16 is 15 dB down, the
  output quantiser is gone, a stop's closure is the sound before fading
  through it, and the pulsed effects C0 and M2 are silent between pulses
  (M0 is continuous).
- The tones end in a 55 ms linear fade (a part flag); B0, B3 and B7 are
  rebuilt from the chip's envelopes and tone tracks, constant where the
  fit swept them. The scorer gained a tone track in cents and a line-width
  term for the effects, so it no longer prefers a drifting fit to the
  measured tone.
- D0 to D11 are a rule: the standard DTMF pairs at equal level (measured
  on both units within 3 Hz once each unit's rate is divided out); B0 is
  2600 Hz. Every per-sample sin, exp and log is the library's own series,
  so the samples are the same to the bit on every platform; `M_PI` and
  friends are defined where strict C99 leaves them out.
- `speakjet.c` is about 700 lines and `speakjet_tables.c` holds 72
  allophones of 14 bytes and about 100 effect parts of 15 bytes (frequencies
  in 16 Hz steps, lengths in 5 ms); the object is about 12.5 KB. Same API,
  same 48 kHz output.
- What the recordings gave (see `docs/design.md`): the chip's rate, 8026
  Hz, with its arithmetic in 8000ths; the voicing period floor(Fs/Pitch)+1;
  a voicing envelope that steps to full at once and falls linearly over
  about 7 ms (measured tick by tick: there is no rise) over three formant
  sines restarting at one phase, plus a per-sound offset under the same
  envelope (a fourth oscillator at 0 Hz; one signed byte); white noise
  through a two-pole resonance (the hiss is Gaussian noise on both units,
  not a thrown tone), each band read off the chip's spectrum and the
  sibilants' peak at Nyquist; two real poles near 2 kHz and 2.9 kHz after a
  sample hold (fitted on the pulses; the tones' images agree); a noise
  floor under every sound; Bend
  as +44 Hz a step on every oscillator; Volume spanning 6 dB; Fast 0.8 and
  Slow 1.25; measured lengths that don't change with context or Pitch and
  are linear in Speed; a 35/30 ms level ramp on every sound; copies of a
  Repeat running on seamlessly at the manual's length; a sound followed at
  once by another cut short by 10 ms.
- Not found in the recordings: any effect of Stress, Relax or code 18.
- The voicing pulse was then measured directly rather than fitted (both
  units, VoxProbe 3 and 4): the envelope's first ticks, the restart phase,
  the offset term and the output poles each came out of the pulses
  themselves, and the 24 vowels, nasals and liquids with isolated pulses
  carry measured formants, levels and offsets. The library's pulse for those
  sounds is within 0.09–0.15 of the chip's (rms of the difference over the
  pulse's rms; it was 0.4–0.7 with the fitted parameters), and within 1 dB
  per sixth-octave band from 150 Hz to 1.3 kHz. The other 48 allophones and
  the effects were refitted under the measured mechanism.
- Score P over the 741 scorable recordings (P: physical distances — length,
  level contour, time-frequency cells, spectrum, formant peaks, pitch — each
  over the two real chips' own disagreement; the two chips score 0.22
  against each other; lower is better): 1.33, against 1.54 for the fitted
  parameters: allophones alone 0.90 (1.08), effects alone 1.44 (1.66),
  connected groups 1.48 (1.66), Man Goes Down 2.42 (2.66). The 24 measured
  sounds alone: 0.90 against 1.13.
- Listening to phrases from a game (WenHop, text2speakjet output) found what
  the single-sound recordings could not, and the chip's phrase recordings
  confirmed each: a sound followed at once by another neither fades out nor
  fades the next in (the 35/30 ms ramps belong to the edges of a run; inside
  a phrase the chip's level never drops more than 14-30 dB between sounds);
  the look-ahead for "followed at once" steps over the Fast, Slow, Stress,
  Relax, Pitch, Speed, Volume, Bend and Repeat bytes that text puts between
  words (every such seam had been a full fade); Fast and Slow are x0.77 and
  x1.22 of the played length and leave the stops alone; a transition after
  silence starts at the sound's own formants (not a sweep up from 0 Hz); and
  the offset term is switched off: the chip's raw spectra carry no energy
  from it at the voice fundamental, where the library had 15-20 dB too much
  on the big-offset sounds. The `dc` field stays, at zero, until the pulse
  component it stood in for is understood (it fits the high-passed pulse
  like an envelope-shaped offset but has no low-frequency energy: a
  transient of some kind). P 1.32 (allophones 0.88, connected 1.46, Man Goes
  Down 2.33).
- Unresolved: the output carries the sample hold's images above 4 kHz at the
  level the AtariVox's jack delivers them (about -30 dB, measured); through a
  console and a television they would be largely filtered, and a listener on
  full-range speakers hears them as a thin second voice doubling the first.
  Whether the library should present the jack or the television is undecided.
  Also open: the
  effects (fitted recipes, not measured mechanisms), the stops' soft closures
  and tails, and a notch between the first two formants that the pulse model
  fills by about 10 dB.
- Each allophone also carries the length of its Repeat copies (one byte);
  the chip's copies of ED and OD run 35 ms where the manual says 10, EG and
  OG 25 where it says 55.
- The tests are rewritten for the new internals; the reference samples are
  regenerated.
