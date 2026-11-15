# Changelog

## The rebuild

**Version numbers (2026-11-16).** The releases first tagged v2.0.0 and v2.0.1 are renumbered 0.5.0 and 0.5.1, so the C
and Go libraries share one number: a Go module at v2 or above needs a /v2 import path, which would break every
importer, so the Go library is tagged v0.5.0 upstream. The git tags v0.5.0 and v0.5.1 mark the same release commits;
v2.0.0 and v2.0.1 are withdrawn. The sound is unchanged.

### Unreleased

- **237 C7's pulse rate** follows the chip's track: Pitch 88 for its first section, then rising, where its fitted
  offsets ran its pulses 1-1.5 ms too often from 85 to 130 ms, so each pulse had too little time to die away before
  the next (both units' pulse times; between pulses the chip falls to -24..-36 dB, ours to -21..-23). The offsets
  were searched by X from the chip-measured track: 1.17 -> 1.07 (0.4.3: 1.12). No effect is now further from the
  chip than 0.4.3; effects mean 0.98 -> 0.97.

### 0.5.1 (2026-11-16; technical manual 1.19)

No allophone is now further from the chip than 0.4.3 was. 0.5.0 had four (IY IH OH UW): its two plain-vowel rules,
the longer first voicing period and the 72-tick pulse fall, were adopted for all twelve plain vowels on a scorer that
was later corrected, and on the corrected one they suit some vowels and not others. Measured per vowel (every
combination of the two rules with both its 0.4.3 and its 0.5.0 entry, X on the chip's four takes) and regressed over
every allophone: 11 closer, none further.

- **The 72-tick pulse fall** is EY UX UH AW's only; the other plain vowels fall over 58 ticks, as every other sound.
- **The longer first voicing period** from silence is every plain vowel's but IH's and OH's.
- **IH AY OH** take back their 0.4.3 entries (with the rules above OH scores 0.88, as 0.4.3 did; IH 0.95; AY 1.02).
- **233 C3** takes back its 0.4.3 description (0.92 on this engine against 0.5.0's 1.05).

Against 0.4.3 as shipped, under the same measure: allophones mean 1.07 -> 0.93 (57 closer, none further, 15 within
0.01); effects 1.22 -> 0.98 (52 closer, 1 further: C7 1.12 -> 1.17); the 88 recorded join items' cost 1437 -> 514
(56 closer, 15 further by 15 % or more); items 15 ms or more off in length 24 -> 14. Tables 7190 B. C and Go
sample-identical within 2.8e-5.

### 0.5.0 (2026-11-15; technical manual 1.18)

Every effect (200-254) rebuilt against the recordings of units A and B, one sound at a time, each judged by its
spectrogram against the chip's four takes (1.0 = as close as another recording of the chip). Against 0.4.3 as shipped:
mean 1.26 -> 0.99 over the 55 effects, 53 closer; 211 A1 keeps 0.4.3's description (0.67 on 0.4.3's engine, 0.71 on
this one; the re-described one scored 0.76); effect frames 4341 -> 4073 B, all tables 6.9 KB. The speech sounds' engine is unchanged by this part. The allophones: each keeps whichever of its 0.4.3
entry and its unreleased (2026-11-08..13) entry scores closer to the chip on this engine (17 take 0.4.3's: AWRR AXRR DH
DO EY EYIY IHWW IYEH LO MM NGO OB OG SH SO TH TS); mean over 65 allophones 1.26 (0.4.3) -> 1.17.

- **The effects' engine** (all effect-only; the speech sounds are as before):
  - they start hard (no attack ramp);
  - the bed is flatter (slope term 0.0468, one pole at 0.5) with a second, flat term that follows the signal's
    magnitude held over 20 ms, so a quiet passage keeps a bed under it (227 B7: -30 dB on both units); the bed does
    not follow the level ramps, fades and decays (220 B0 and every pure-tone effect);
  - a second-order term in the output stage (0.07);
  - a gate's off part is the last 31 % of its period, 34 dB down (229 B9: a 3.6 ms drop every 11.5 ms on every take);
  - a swept noise climbs for half its cycle (C8, C9, all four takes).
- **Voicing's first period** (the plain vowels IY..UW from silence): 12 ticks (1.5 ms) longer than the rest; every vowel
  on all four takes pulses at about 4, 16, 28, 40 ms. 10 vowels closer (UH 1.59 -> 1.34, AW 1.47 -> 1.37, IY 1.39 ->
  1.29), none further; applied to every voiced sound it made the voiced stops and nasals worse, so it stops at vowels.
- **The plain vowels' voicing pulse** falls over 72 ticks, not 58 (the effects and the other speech sounds keep 58): 11
  vowels closer (AX 1.55 -> 1.43, UX 1.62 -> 1.47, IH 1.44 -> 1.31), none further.
- **The effects' descriptions**, from the chip's own measurements: 23 re-described (C6 C7 A7 B2 B9 A6 A8 A9 C0 C9 C4
  R0 R1 A4 C2 B8 B6 R4 C5 A0 A5 and others; e.g. 236 C6 15 frames 388 B -> 7 frames 159 B, a pulse rate gliding
  67 -> 130 -> 67 Hz under two formants on their own timings; 222 B2 21 frames 671 B -> 10 frames 181 B, a 56 Hz/ms
  linear glide), the other 32 kept and checked against their pictures.
- **The speech sounds' attacks** (X scored at the best of shifts -12..+12 ms round the onset, as the gates do; the
  earlier attack law of 12 ms for every vowel was an artefact of onset-only alignment): per vowel, AW AWRR AXRR AXUW
  AYWW IY IYRR IYUW OH OW OWIY 45 ms; EYIY EYRR IHWW OHIH 60; AX EH EY OHIY OWWW UX 30; AY EHLL 20; IH UH OWRR IYEH the
  35 ms default; UW 12. The vowel rules above (first period, 72-tick fall) are being re-measured under this X.
- **Other speech sounds**: 199 PO and 198 PE's low pulse swells in over 10 ms (PE 1.16 -> 1.13, PO 1.15 -> 1.14); SO
  PO OG EK levels down 4, 4, 6 and 12 dB (0.78 -> 0.73, 1.14 -> 1.09, 1.11 -> 1.05, 0.95 -> 0.79); 193 TS 95 -> 115 ms
  (ended 18 ms before the chip; 1.04 -> 0.94); HO WH IYEH take 0.4.3's entries (HO's unreleased fit was 40 dB quiet).
- **A speech sound's attack is in pitch periods**: its aa ms at Pitch 88, shortened by 88/Pitch (VoxProbe 7's 'pitch 150
  then AW/IY/OW', both units, 5-17 % closer; nothing else changes).
- **Crossing joins out of a glide**: a crossing cut longer than the glide's last segment (its end hold) now trims across
  the segments instead of being skipped (VoxProbe 8's AYWW AW, both units: 38-42 ms long -> within 4; Slow AYWW AW 42 -> 7).
- **A vowel after a voiced stop** (BE BO DE DO EB ED, not GE GO) opens at its own level and amplitudes, its formants still
  gliding from the stop's release (Man Goes Down P1's DO AYWW, both units: the vowel at full level straight after the bar;
  ours ramped from the bar's level over the transition).
- **A voiced fricative after a pause** (DH VV ZZ ZH JH) opens with 40 ms of its noise alone before the voicing (Man Goes
  Down P2's EYIY P4 DH, both units: the frication 40 ms before the voicing where ours voiced at once).
- The pitch-period attack is never lengthened below Pitch 88 (P2's IYUW at Pitch 40 is full within its first pulses).
- **A voiced stop after a short pause** (P4-P6) opens its bar 12 ms into the pause (Man Goes Down P1's ZZ P4 DO, both
  units: the bar 11 ms after ZZ's end where ours waited 23; P1 1518 -> 1506 ms against the chip's 1500/1505).
- **The longer closure after a sound** belongs to DO (+25 ms) and GO (+15) only, as VoxProbe 13 measured; DE and GE had
  it by analogy and were 25 ms long after a vowel (VoxProbe 4's EH DE, both units: 191 -> 167 ms against 169/180).
- **A vowel or glide above Pitch 88 runs longer** (ASSUMPTION: linear in Pitch, 16 % at 150; VoxProbe 7's 'Pitch 150 then
  AW/IY/OW', both units, 133-134 ms where the library gave 113-117).
- **Slow on a plain vowel** stretches it x1.29, not x4/3 (VoxProbe 8's slowed AW and IY, both units, 149-150 ms where
  x4/3 gave 154-157); glides and nasals keep x4/3. Man Goes Down phrase 1 1500 -> 1491 ms against the chips' 1483/1490.
- **Effect attacks**: 211 A1, 240 D0 and 205 R5 start over 2 ticks (0.69 -> 0.64, 1.02 -> 0.95, 1.23 -> 1.15); the rest
  stay hard.
- **VoxProbe 14's rules** (below, from the canonical branch of 2026-11-09, merged into this release): the Speed step at
  116, the join terms IH +15 OW -10 RR -16 ms (AW's +6 was fitted on an AW whose attack and length have changed; on
  this engine it made every AW pair further and is 0), the KE locus, KE and KO after a nasal +15 ms, Slow on KO and PO.
- Earlier unreleased work, below, is part of this version.

#### Earlier, unreleased until 0.5.0 (technical manual 1.15)
#### VoxProbe 14's findings, unreleased until 0.5.0 (technical manual 1.16)

- **Crossing joins** (ASSUMPTION in the constants, shape measured on units A and B): between vowels, glides and LE LO
  WW RR the formants move on one line across the join, 5 + 5.5 ms per ERB of F1+F2 distance (at least 20 ms), ending
  31 ms into the next sound; the old rule moved them over 25 ms after the boundary. Every sound alone is sample for
  sample as before. VoxProbe 4 AW IY P 0.949 -> 0.013, IY AW 0.805 -> 0.029; all 44 recorded multi-sound items E
  4.747 -> 4.695, P 0.5525 -> 0.5101; Man Goes Down P2's "be with" moves as on both units.
- **219 A9:** its last part (the 100 Hz tone, 260-320 ms) carries a low noise band (1000 Hz, 2000 wide, level 5):
  on both units it runs flat to ~1 kHz 25-30 dB over the take's silence where ours had none. Spectrum 1.34 -> 1.31,
  E 4.77 -> 2.97, P 1.61 -> 0.58 (gate2 HOLD on tfdelta 333 -> 433 only: the new noise cells; the picture matches both
  units). Effect tables 2721 -> 2724 B.
- **219 A9** (revised): the tail's noise band is 1200 Hz, 2400 wide, level 6, with the effect's high pass at 512 Hz
  (the first band put 250-500 Hz 10 dB over the chip): within ~4 dB of both units in every band to 4 kHz.
- **The bed per effect** (fxs `bs`, a seventh byte per effect, +55 B): 215 A5 151 %, 218 A8 160 %, 220 B0 81 %, 222 B2
  77 % (E 5.11 -> 4.78, 4.35 -> 4.10, 3.76 -> 3.50, 4.10 -> 3.85; P down on all four). The common law stays.
- **229 B9:** its U-shaped pulsed line is built (900 -> 500 -> 900 Hz; the rise is one pulsed tone where the old part
  held three flat ones), every part 4.5 dB up to keep the chip's loudness: spectrum 2.49 -> 1.58, E 4.72 -> 4.55,
  P 0.89 -> 0.77 (gate PASS). Effect tables 2724 -> 2718 B.
- **The bed per effect** (fxs `bs`, a seventh byte per effect, +55 B): 218 A8 160 %, 222 B2 77 % (E 4.35 -> 4.10,
  4.10 -> 3.85; P down on both). The common law stays. (215 A5 151 % and 220 B0 81 % were fitted and
  withdrawn.)
- **The Speed step is at 116** (VoxProbe 14 section 2, both units): at Speed 115 vowels, glides and pauses play as at
  114, where they shrank on the step law. IYUW at 115 -30 -> -10 ms, AW P2 AW -35 -> +10; section 2 mean |error|
  12.0 -> 10.0 ms. Only Speed 115 changes: every sound at Speeds 60-114 and 116-127 is sample for sample as before.
- **Join terms** (alloj, a side table, +8 B): a vowel or glide join's cut grows by a term for each of the two sounds:
  IH +15, AW +6, OW -10, RR -16 ms. Vowel and liquid joins recorded on VoxProbe 4, 13 and 14 (both units), AY's
  aside: rms 16.8 -> 4.1 ms, max 42 -> 10 (AW IH +22 -> -3, AW IH AW +42 -> -2, AW RR -13 -> -2, OW IY OW -20 -> 0).
  Every sound alone and Man Goes Down's phrases are sample for sample / ms for ms as before. AY IY is still ~32 ms
  long (open).
- **KE and KO after a nasal** close 15 ms longer, not 30: the closure's quiet run against both units was 11-30 ms long
  on VoxProbe 14's NE NO MM NGE KO AW and 15 on VoxProbe 13's NE KO OW; now within 4 ms except MM (+15). VoxProbe 13
  section 6 9.6 -> 9.2 ms, VoxProbe 14 section 10 12.2 -> 10.8. Every sound alone and Man Goes Down unchanged.
- **The KE locus:** a vowel after KE starts at F2 2100 Hz, F1 300, and glides to its own over 8 ms per ERB of F2
  distance, growing 4 ms per ERB (both units by picture: KE has no formants of its own, and the chip's vowel after it
  starts at a velar locus). KE AW -30 -> -5, KE OW -45 -> -15, KE UW -30 -> -5 ms; KE IY and KE EH unchanged; VoxProbe
  14 section 7 11.7 -> 8.0. Every sound alone and Man Goes Down unchanged.
- **Slow on KO and PO** (VoxProbe 14 section 4, both units): the vowel ahead runs on and the release grows only
  before a slowed TT; a slowed KE KO PE PO after a sound closes 20 ms longer instead, before a sound as before a pause.
  AW Slow KO AW +30 -> -5, AW Slow PO AW +25 -> -10 ms; section 4 15.5 -> 11.8. Every sound alone, VoxProbe 13's Slow
  TT items and Man Goes Down unchanged.

### 0.4.3 (2026-11-08; technical manual 1.13)

From VoxProbe 13, recorded on units A and B (2026-11-08). Every sound played alone at the default Speed is sample for
sample as at 0.4.2; the tables are unchanged. Man Goes Down: phrase 1 1485 ms, phrase 2 1700, phrase 4 940 (units
1495/1502, 1695/1700, 942/945); timing inside the phrases (each warped onto both units) 14.7 -> 12.4, 16.7 -> 9.9,
9.9 -> 6.8 ms. VoxProbe 13: mean length error per section summed 120.9 -> 55.0 ms; boundaries inside items (each run
of sound's start and end against both units) 8.3 ms mean, every item's runs matching the chips'.

- **Pauses:** P2 200 -> 188 ms (VoxProbe 13 section 9, both units).
- **Speed** (above 114 ASSUMPTION): pauses scale; above 114 unslowed vowels, glides, their copies and pauses shrink by
  (155.9 - Speed) / 47.4 and the other speech sounds don't shrink.
- **Repeats:** a Slow on the second of two equal sounds plays two whole sounds.
- **Slow:** SE SO SH take half of its stretch.
- **Stops** (ASSUMPTIONS): the stop-into-vowel trim applies under Slow and not to GE GO; DE DO close 25 ms longer after
  a sound, GE GO 15; DE DO GE GO 10 ms longer after a nasal unless slowed, 40 after NGE; an unvoiced stop 30 ms longer
  after a pause, 20 when slowed and before a pause; a voiced stop after a pause keeps its whole closure.
- **Joins** (ASSUMPTIONS): a nasal and a vowel join with no overlap unless either is slowed; LE LO WW RR join vowels by
  the vowel rule; no overlap into TH.
- **Slowed glide:** 50 -> 25 ms (confirmed in form by picture on VoxProbe 13's Slow IYUW Slow SE).
- **After a hiss** (by picture, both units): the next sound starts as from silence (no sweep from the hiss), and a
  sibilant has no starting burst (phrase 4's dark blob inside SE SE is gone).
- **Stop into vowel** (ASSUMPTIONS): the F2-distance rule lengthens the release below zero (not after BO); KE KO after
  a nasal close 30 ms longer. Unvoiced stops: a silent closure after a 10 ms fade; no overlap into TT from a vowel or
  hiss, nor from NE/NO into KE KO PE PO BE BO; PO into a vowel overlaps 20 ms; the sound before a slowed one runs on
  40 ms (not a nasal, not before a pause), the stop releasing 15 ms longer. An unslowed NE or NO into GO/GE runs on
  18 ms (+25 before a slowed GO; not with Fast).
- **GE into a vowel** (ASSUMPTION): F2 glides from GE's over 8 ms per ERB of distance. **TH** between a voiced sound
  and another sound: 25 ms longer (ASSUMPTION).

### 0.4.2 (2026-11-07; technical manual 1.11)

Phrases. Every single sound plays exactly as at 0.4.1 apart from 191 TT, 141 NE, 142 NO and 143 NGE below; the rest
changes only how sounds join, pause and stretch. Fitted on units A and B only (Man Goes Down's phrases and every
two-sound pair the probes recorded); rules resting on one phrase or a few items are marked ASSUMPTION in the code and
"assumed" in the technical manual (section 6.8), and VoxProbe 13 tests each. Active length, unit A / unit B / 0.4.1 /
0.4.2: Man Goes Down's phrase 1 1495 / 1502 / 1518 / 1502 ms, phrase 2 1695 / 1700 / 1775 / 1700, phrase 4 942 / 945
/ 858 / 925; AW IY 261 / 215 / 270, BE EH 168 / 198 / 168, DO AYWW 280-282 / 315 / 288.

- **Sounds.** 191 TT: a 10 ms onset at +3 dB. 141 NE, 142 NO, 143 NGE: a weak noise band each (NE P 0.67 -> 0.33,
  NO 0.72 -> 0.50, NGE 1.02 -> 0.73 on the solo takes).
- **Speed** below 114: speech on the effects' law, (147 - Speed) / 33 (AY and IYUW x1.21 at Speed 108, both units);
  from 114 up as before.
- **Pauses:** P0 20, P1 90, P2 200, P3 648, P4 15, P5 30, P6 59 ms (VoxProbe 1, both units, 2600 and Stella; Man
  Goes Down's gaps).
- **A voiced stop after a nasal** closes nearly silent: 38 ms, the nasal's F2 and F3 falling into it, the bar 16 dB
  down, the release rising over 10 ms ("goes" in "man goes down").
- **Joins.** A stop on either side takes the short 15 ms cut (the code had looked only at the next sound), none after
  an unvoiced stop. ASSUMPTIONS: vowel and glide joins cut by 66.9 - 6.9 x the F1+F2 ERB distance (runs on when
  negative), replacing the fixed 30 ms; a voiced stop into a vowel with no Slow on it trimmed by 30 - 10 x its F2
  distance and, from silence, closing for at most 30 ms; 179 GO before OW releasing on F2 1430, F3 1920 Hz; a slowed
  glide followed by a Slow 50 ms longer.
- **Toward SE:** a voiced sound bends to 167 ZZ's F2 and F3 over its last 40 ms. 187 SE's copy 80 ms.
- **docs:** technical manual 1.11 (a new mark, "assumed"; section 6.8 lists what was fitted on Man Goes Down; Bend
  0-2's second line corrected to run parallel to the tone), Appendix A regenerated; design.md brought up to the code.

### 0.4.1 (2026-11-07; technical manual 1.10)

The measurement under the fits repaired first: unit A's 2026-11-05 takes re-placed (HE sat on WH's slot, seven
sounds unplaced), the audit's chip run found above the hum (quiet fricatives had been measured on nothing), the
depth cut no longer emptying HE PO PE TH. Then each affected sound worked to its measurement, and the worst of the
rest: 46 sounds changed. Over every sound on the solo takes of both units, against 0.3.0: E 4.35 -> 3.78 (40 % of
the gap to a second AtariVox closed, 29 % at 0.4.0), P 0.64 -> 0.55 (29 %), spectrum 2.54 -> 2.34 (15 %); against
1.0.0, E 14.03 -> 3.78, 92 % of that gap.

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
