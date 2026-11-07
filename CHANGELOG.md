# Changelog

## The rebuild

### Unreleased (2026-11-07; technical manual 1.10)

The measurement under the fits repaired first: unit A's 2026-11-05 takes re-placed (HE sat on WH's slot, seven
sounds unplaced), the audit's chip run found above the hum (quiet fricatives had been measured on nothing), the
depth cut no longer emptying HE PO PE TH. Then each affected sound worked to its measurement, and the worst of the
rest. Over every sound on the solo takes of both units, against 0.3.0: E 4.35 -> 3.83 (37 % of the gap to a second
AtariVox closed, 29 % at 0.4.0), P 0.64 -> 0.56 (24 %), spectrum 2.54 -> 2.38 (11 %).

- **The sibilants' starting burst** is twice what the byte reached: `th` is now /64 of the band (was /256; stored
  values divided by four, byte-identical), SE SH SO at x2-x3 better on every figure.
- **A decaying release** (193 TS: -13 dB over its 95 ms, the white floor holding): the band's end level as a
  field, `nd` /256. The allophone row grows to 27 bytes.
- **Allophones.** 183 HE's noise high pass 304 Hz; 186 FF 75 ms, 199 PO 55, 175 DO 30, 170 BE 40, 179 GO 45, 131 EH
  140; 133 AX, 134 UX and 137 OW with their own attacks (the chip's first pulses are partial); 195 KO's burst
  high-passed at 304 Hz; 196 EK's band 2000 Hz; searches on HO TH SE SO TS KO GE EB ZZ ZH.
- **Effects.** Fade-outs where the chip's last 25-40 ms fade and ours stopped: 206 R6 25 ms, 207 R7 20, 254 M2 15,
  224 B4 40; 229 B9's first parts 5 dB up; searches inside their structures on R6 R4 R3 R2 A7 A8 B2 B6 B7 B8 C0 C3
  C4 C6. 253 M1 rebuilt as the chip plays it: a 50 ms block of low noise under a hiss high-passed at 608 Hz — two new
  bytes in the effect structure row (a noise high pass and a burst length; the burst level /64, values divided by
  four). Tables 5131 B.
- **docs:** technical manual 1.10 (the burst scale, `nd`, TS), Appendices A and B regenerated.

### 0.4.0 (2026-11-06; technical manual 1.8)

Structure measured by a band × time audit (library minus chip, 24 log bands × 2.5 ms, against the chip's own
take-to-take spread, both units), then each sound worked to its measured structure. Against 0.3.0 over all 2355
recordings, both units: P 0.895 -> 0.844 (-5.6 %). Tables about 5 KB.

- **Timing (VoxProbe 12).** Slow x4/3 and Fast x2/3 of the played length (were x1.22 and x0.77), Slow also on the
  voiced stops; a sound straight after the same sound plays as a Repeat copy (such joins had been ~31 ms long).
- **The voice period** from a 16-bit phase step, floor(65536 / floor(rate · 65536 / 8000)) + 1 ticks; the reset
  default is the manual's Pitch 88 (VoxProbe 12: identical on every take).
- **Allophones.** Unvoiced fricatives switch in 3 ms; nasals rise in 25 ms; voiced stops' bars start at once with
  weak upper formants; each stop its own closure, bar and release levels; louder onsets; the sibilants' burst of
  low noise; WH's white floor; every allophone's noise high-passed at 400 Hz; per-sound attack and release; 161
  AXUW's F1 opening and closing late. The allophone row grows from 14 to 26 bytes.
- **Effects.** A per-effect structure row (attack, release, a 5 ms starting click); the effect release 25 ms;
  C8/C9's sweep 650 Hz - 2.4 kHz with its noise high-passed; 219 A9, 223 B3 and 228 B8 rebuilt from the chip's
  tracks; tone endpoints re-anchored on the measured tracks for seven more.
- **docs:** the technical manual 1.8 and design.md brought up to the code; Appendices A and B regenerated.

### 0.3.0 (2026-11-02)

The rebuild is released as 0.3.0: a different library from the first
implementation (tag v0.1.0; every table and most rules re-measured from the
chip), with the same API. Since v0.1.0:
the analogue output stage (two real poles and a real zero, solved exactly per
held sample), the effects' noise bed, reading ahead, Speed and Volume laws
from VoxProbe 8, the pulse periods from VoxProbe 9, sound tables tuned by
CMA-ES inside their measured structure (rounds 4 to 6), and structure fixes
for 235 C5 and 236 C6. Tables 2721 bytes.

- **The pulse period, from VoxProbe 9.** An effect's pulse runs ceil(8000 / rate)
  ticks, one shorter than the voice's floor(8000 / Pitch) + 1 wherever 8000 / rate
  is whole (Pitch 100, 160, 200, 250...): 235 C5 at Pitch 200 now buzzes at
  200.6 Hz as the chip does, not 195.8. Both periods are figured in the chip's
  own 8000ths rather than the render rate. The reset default is Pitch 87, not
  the manual's 88: at the default every voiced sound runs 92 ticks on both
  units, which only 87 gives; provisional until a probe plays a sound at the
  default and after an explicit Pitch 88. Man Goes Down P 1.592 -> 1.550.
- **Round 6 of tuning**, each sound gated on spectrum, E and P together:
  14 effects and 20 allophones improved and shipped, the rest held.
- **One score definition everywhere**: the gates and the searches read the same
  spectrum, E and P, and the allophone gate uses full E.
- **docs/technical-manual.md rewritten** as an implementer's reference to the
  chip: interface, tick loop, voice, noise, allophones, effects, commands,
  reading ahead, the output stage, open questions and every sound's parameters.
- Scores at release (README "How close it is"): P 0.61 over the allophones,
  0.92 over the effects, 0.89 over all 1191 recordings.

Known and not yet modelled: Bend below zero (the chip folds a tone bent under
about 50-90 Hz to near 2.4 kHz), 215 A5's 40 Hz tone, the linear Volume law,
the Pitch-driven pulse timbre of 235 C5 and 200 R0 to 203 R3, and the death
sound in Man Goes Down.

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
  208 R8 (its low tail), 221 B1 (its level through the sweep), 226 B6 (two tones
  taking turns, with glides between and a fade at the end). Tables 2700 bytes;
  the size budget is now 3000.
- **The effects' bed** levels off above about 1.5 kHz (the signal's change per
  tick smoothed by one pole): 220 B0, 221 B1 and 228 B8 had 6-9 dB too much,
  the DTMF digits keep theirs. Effects P 1.190 -> 1.178; speech unchanged.
- **The output stage is now the analogue circuit**, solved exactly on the held
  samples, in place of a bilinear digital filter on their per-sample average,
  whose warping and droop put the images 1-8 dB low from 5 to 14 kHz (every
  effect, both units). Every sound improves: all 1382 items P 1.053 -> 0.966,
  allophones 0.882 -> 0.808, Man Goes Down 1.633 -> 1.569.
- **A real zero at 14 kHz in the output stage**: the images still sat 1-4 dB low
  from 6 to 14 kHz. All 1382 items P 0.966 -> 0.949, spectrum 2.91 -> 2.80.

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
