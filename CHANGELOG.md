# Changelog

## The rebuild

**Version numbers (2026-11-16).** The releases first tagged v2.0.0 and v2.0.1 are renumbered 0.5.0 and 0.5.1, so the C
and Go libraries share one number: a Go module at v2 or above needs a /v2 import path, which would break every
importer, so the Go library is tagged v0.5.0 upstream. The git tags v0.5.0 and v0.5.1 mark the same release commits;
v2.0.0 and v2.0.1 are withdrawn. The sound is unchanged.

### 0.5.8 (2026-11-25; technical manual 1.29)

**The scale.** X now judges against unit A alone: VoxProbe 17's three rounds of every sound (one session) with vrnA,
the same unit in an earlier session, so its spread includes what changes from one power-up to the next (technical
manual 12.10). Every sound reads further on it than on the old four takes from both units, so earlier figures don't
compare; every release was rescored on it. All 127 sounds mean X 1.64 (0.4.3) -> 1.26 (0.5.7) -> 1.20 (-27 % on 0.4.3,
-4 % on 0.5.7), median 1.46 -> 1.16 -> 1.13; allophones 1.30 -> 1.07 -> 1.01; effects 2.10 -> 1.50 -> 1.45; over 1.0
113 -> 95 -> 87. 54 sounds closer than 0.5.7 - 130 EY -41 %, 176 ED -28 %, 174 DE -25 %, 254 M2 -24 %, 129 IH -21 %,
158 IYEH -19 %, 171 BO -18 % - and **none further**, by any amount. Phrase items 236.3 -> 234.3, none further. Tables 7324 bytes.

**Engine**

- **A voiced fricative after a pause and a Reset has no noise lead-in** (VoxProbe 17, unit A: VV ZZ ZH DH after P1 and
  Reset voice at once in all three rounds, and VoxRecord's solo takes, also after a Reset, agree). Man Goes Down's DH,
  with a sound and P4 before it and no Reset, keeps the 40 ms lead-in.

**Sounds**

- About 50 sounds refitted to unit A. Each fit is locked in time to the chip (its start and end charged), checked by
  picture, and an allophone's fit keeps a step only if no glide or phrase item built from it gets worse. Five effects
  refitted part by part with every band's start and end held to the chip's: 254 M2 -16 %, 206 R6 -7 %, 217 A7 -5 %,
  235 C5 -4 %, 222 B2 -3 %. 165 JH gains
  an onset (5 ms, -10.5 dB) in place of its hard burst; 233 C3 loses its trailing 25 ms of silence.

**Tried and left out** (each measured against VoxProbe 17 and worse on X or the phrase items): a silent lead-in for
voiced stops from silence, shorter closures for 191 TT and 196 EK, a shorter 252 M0 tail, a 4 ms longer 179 GO closure
(its end right, one slowed phrase 7 % further), a longer slowed voiced-stop bar, and an onset gain from silence, shared or
per sound.

### 0.5.7 (2026-11-24; technical manual 1.28)

**The scale.** X as in 0.5.6. Beside it, a new picture and score: the gallery's one delta is now the **excess** - ours
against all four takes, cell by cell (10 ms x 1/3 octave), shrunk by the takes' own spread there, so colour means
something no real AtariVox does (the user's idea). XE, its score, is calibrated leave-one-out on the takes' median (1.0 = a
typical real take). XE and X agree on bodies and disagree on edges: XE's smoothing hides pulse-scale gaps, X's coarse and
level views charge level moved within a sound. Installs are judged on X and the hold rules, with XE and the pictures beside.
All three releases rescored on this measure (the 0.5.6 build itself, not its release figures): all 127 sounds mean X
1.44 (0.4.3) -> 1.07 (0.5.6) -> 1.02 (-29 % on 0.4.3, -4 % on 0.5.6), median 1.29 -> 1.02 -> 1.00; allophones 1.20 ->
0.95 -> 0.92; effects 1.75 -> 1.22 -> 1.15 (median 1.22 -> 1.12); over 1.0 98 -> 67 -> 64. 122 sounds closer than 0.4.3
(AY SH TU ED +4 %, C7 level, as at 0.5.6); 70 closer than 0.5.6 - DO -35 %, C0 -25 %, BO -20 %, A6 -17 %, R2 -15 %, B9
-15 % - and only KE further by more than 2 % (+7 % at free placement, closer at its installed one). Phrases 236.3 /
173.7 (0.5.6 235.7 / 175.3; one unit against the other 143.9 / 138.3). Tables 7356 bytes.

**Engine**

- **171 BO from silence opens on an event before its bar** (all four takes: 6-8 ms ahead of the onset, the bar's noise band
  at its own level above 1.2 kHz and 8-15 dB under it below 1 kHz, a dip, then the bar): `BO_PRE` 9 ms of the bar with F1
  x0.15, then 2 ms of silence. BO 1.230 -> 1.182, then its level +1 dB 1.169. 170 BE shows the event on three takes, but
  every model of it scored worse (1.005 -> 1.2) and it is left out; a click (`BAR_CLICK`) and a noise lead out of the bar
  were tried on both and refused.
- **A vowel or glide from silence without an attack of its own ramps from -12 dB, not -26** (`PON_FLOOR` 0.05 -> 0.25).
  Measured as a 2 ms level 300 Hz-5 kHz, the first pulse of eight solo vowels and glides sits 12 dB under their steady
  pulses on all four takes, ours 15-20; 0.5.6's per-pulse measurement (-32 dB at 1 ms, Pitch 88) read lower - the two
  methods disagree and both are recorded at the constant. At 1 ms resolution after the change, 135 OH's and 137 OW's
  first pulses land within 1 dB of the chip's and their second pulses match (139 UW's first is 6 dB strong). X over the 28 vowels and glides 0.8171 -> 0.8132, five better,
  none worse; XE 0.751 -> 0.697 (OWWW 1.30 -> 0.78, IYUW 1.06 -> 0.76, EYRR, OWRR, UW). The same floor for sounds with an
  aa of their own read 16 of 21 better on XE but worse on X and its level-over-time on seven, and was reverted.

- **A voiced fricative (VV ZZ ZH DH) joined straight on to another sound starts without its onset boost**, as it
  already starts without its attack. The boost, measured on sounds from silence, lifted a joined ZZ's first pulse to
  5x its level (IH ZZ peak 1.078 -> 0.210): a click, and a clip at Gopher's gain 1.5, in "is" and "ears", found in the
  AtariVox demo ROMs. Unvoiced sounds keep it.

**Tables (xstruct's menus widened)**

- The allophone menu moves every engine byte a sound uses (hp wf tc th ba nd gh gm, bl og rl) and the low pulse (pa pt pf
  pd); the effect menu moves each tone's start and end frequency (x0.97/1.03), each part's lr, and the effect's attack and
  release. Every candidate is held to the hold rules at the installed placement and its picture checked.
- Allophones: 172 EB 1.482 -> 1.394, 196 EK 0.858 -> 0.815, 199 PO 1.395 -> 1.331, 187 SE 1.293 -> 1.253, 182 CH
  1.126 -> 1.089, 185 WH 1.119 -> 1.088, 198 PE 1.920 -> 1.886, 180 EG, 181 OG, 186 FF, 192 TU.
- Effects (31): 216 A6 1.216 -> 1.103, 229 B9 1.260 -> 1.149, 231 C1 1.273 -> 1.158, 232 C2 0.928 -> 0.845, 230 C0
  0.898 -> 0.830, 215 A5 0.719 -> 0.666, 217 A7 1.284 -> 1.225, 205 R5 1.500 -> 1.429, 206 R6 1.732 -> 1.655, 222 B2
  1.598 -> 1.520, 214 A4, 233 C3, 235 C5, 236 C6, 203 R3, 207 R7, 239 C9 and others; 237 C7 held (a dropout).
- Refits (fixer, walked back): 171 BO 1.479 -> 1.226, 239 C9, 199 PO, 191 TT 0.841 -> 0.831.
- A second xstruct pass on the effects that moved: 230 C0 0.833 -> 0.751, 216 A6 1.103 -> 1.014, 229 B9 1.149 -> 1.074,
  231 C1 1.159 -> 1.114, 222 B2 1.520 -> 1.469, 217 A7 1.225 -> 1.188, 232 C2 0.845 -> 0.813, 239 C9 1.607 -> 1.579,
  235 C5 1.462 -> 1.437; 237 C7 held (a third dropout).
- xstruct's effect menu also moves the bed scale and the onset burst (bs, th): 201 R1 1.348 -> 1.317, 218 A8 1.235 ->
  1.221, 247 D7 1.004 -> 0.995 (the excess fault pass put the D and R effects' onset faults in the bed).

**Pictures.** Every sound's gallery picture is wide: the chip, ours and the excess stacked on one time axis on the
left; the whole-sound spectrum (each of the four takes, their range shaded, ours on top) and the sound's X at every
release (each rebuilt from its tag, placed by release date) on the right. Releases on today's measure, mean X: 0.3.0
1.52, 0.4.3 1.44, 0.5.0 1.32, 0.5.6 1.07, 0.5.7 1.02.

**Measured and not a fault:** the chip's apparent slow fade over its last 5-8 ms is its recording background (the level
settles at the pre-onset noise); the effects' onsets match the chip to the 2 ms step except A0 (early) and A6 (late).

### 0.5.6 (2026-11-22; technical manual 1.27)

**The scale.** Every figure in this release is on the re-baselined X (technical manual 12.8): X2, a weighted mean of five
views each divided by what a real take scores, with the recordings' background now given to both sides. Figures in the
entries of earlier releases are on the scales of their day. Against 0.4.3, both rescored on this measure: allophones mean X 1.20 -> 0.95 (-21 %), effects 1.75 -> 1.22
(-30 %), all 127 sounds 1.44 -> 1.07 (-26 %), median 1.29 -> 1.02; sounds over 1.0 98 -> 67; 118 closer, 7 further (TU, AY, ED,
SH, UW +4 %, D4 +3 %, C7 +1 %); edgecheck timing flags 252 -> 124 (docs/scores.md). Connected speech, 44 phrase items on both
units, mean cost 264 (0.5.4) -> 235.7, median 185 -> 175 (one unit against the other: 143.9 / 138.3). Tables 7356 bytes
(every table, sizeof; the 6823 and 6825 quoted below were counted with a 31-byte allophone record, 216 bytes short).

**Engine**

- **A vowel or glide from silence ramps in from -26 dB, not from nothing** (with no attack of its own; `PON_FLOOR`).
  Both units' first pulses lie on a straight 35 ms line from -26 dB, at Pitch 88 (-32, -8, -1, 0 dB at 1, 11, 23, 35 ms)
  and at Pitch 150 (-29, -10, -5, -2, -1, 0 at 0, 10, 20, 29, 36, 43 ms); from zero the first three pulses were 5-12 dB
  low and every band read the onset ~11 ms late. A rise by pulse count fitted Pitch 88 and doubled VoxProbe 7's Pitch 150
  OW on both units, and was dropped. IYUW, OWWW, EYRR, OWRR 0.7-2.8 % closer; 135 OH (aa 35 -> 0) 0.599 -> 0.577, 139 UW
  (aa 11 -> 0) 0.965 -> 0.919; 88 phrase items +0.03 %.
- **193 TS's decaying release falls on the noise band alone, evenly in dB**, the white floor holding (it scaled the whole
  frame linearly, so the floor fell with the band and the level held 80 ms then dropped; both units: 1.5-8 kHz falls 17-18
  dB across the sound, 300-1500 Hz 6-8). TS X 1.120 -> 0.882 with its refit; its 500-1500 Hz tail is back.
- **The 40 Hz output high-pass on speech too** (it was on the effects since 0.5.4): six vowels and glides' first harmonic
  was 0.8 dB strong on every take, the pole takes 0.9 dB there.
- **An alveolar's voice bar (DE DO ED OD) carries its upper formants at 0.05 of the release's** (`ABAR_UP`; the other
  voiced stops keep 0.12): 175 DO's bar sat 10 dB over the chip at 1.5-2.5 kHz. DE -4.5 %, DO -3.0 %.
- **Audit** of the join and timing rules (each behind a switch in `SJ_AUDIT` builds only; normal builds unchanged):
  DO's closure after a sound +13 ms (was +25), GO's none (was +15); TT's +3 dB before a vowel and a voiced stop's bar
  opening into a short pause deleted; 0.5.4's carry across a short pause deleted (never read since 0.5.5); SE's attack back
  to 13 ms. VoxProbe 16 3.909 -> 3.807 dB band error; Man Goes Down P1 1475 -> 1485 against 1490.
- **Table bytes are held to their range.** A fit could write a negative onset length (174 DE's `ob` -3); C wrapped it to
  253, Go refused to build. gen_tables now holds every unsigned allophone byte at 0 as well as 255.

**Sounds**

- **Every sound refitted on the new measure**, with its placement locked to the chip's and its start and end charged by
  the band (a fit may not buy X by sliding a sound in time), each kept only if X improved without its structure, timing
  flags, dropouts, level-over-time or the old X1 getting worse, and its picture checked against both units. Where the
  final best failed, the newest earlier best that passed was taken.
- By hand: 191 TT's white floor off and band +2 dB (3.3 dB quiet against AW in five recording sets); 238 C8's noise band
  widened round its sweep (the whole-sound spectrum on the chip's from 100 Hz to 4 kHz); 222 B2 1.5 dB quieter; 218 A8's
  opening a plain glide in Hz to the chip's flat 501/1002 Hz octave (a curved glide was tried and removed: across 206 chip
  glides in 50 effects a straight ramp in Hz fits best); 199 PO's low pulse decay shorter; 208 R8 and 224 B4 timed and
  held to the chip's lines; 214 A4's third formant at 0.05 in parts 4-5; 215 A5 back to its 0.5.5 parts; 128 IY F1 272 Hz.
- 148 RR back to its numbers before the 2026-11-19 refit, which was the worst regression against 0.4.3 on the new measure
  (0.871 -> 0.783 now); 188 SO back to its earlier numbers (by ear).

**Measure and tools** (lab only, not in the library): the 2048-point spectrogram view had no background (the takes' first
50 ms is under two windows), and the spectrum view clamped the takes at their background but not ours; both fixed.

**Before the audit (2026-11-18)**


- **A voiced fricative after a pause keeps its whole length after its 40 ms noise lead-in**, and takes the vowels' Speed step
  above 115, lead-in included (VoxProbe 12, both units, per sound by DTW: Man Goes Down P2's DH after P4 140-150 ms at Speed
  114 and 110 at 118, ours 97 at both; now 137 and 110). P2 1680 -> 1690 against 1680-1690; P2 sent without its Speed
  1950 -> 2095/2110 now 1990; VoxProbe 16 3.849 -> 3.820 dB; of 519 scored items only P2 changes. Reference samples
  regenerated (intro, pangram, pitch).

- **Attack, release and onset steps on 15 sounds** (table): a structure search over 56 sounds' edges, each step kept only
  where X improved. 187 SE -3.8 %, 192 TU -1.5 %, 211 A1 -1.3 %, 220 B0, 234 C4, 236 C6 -1.2 %, 200 R0 -1.0 %, 188 SO,
  144 NGO, 147 WW, 217 A7, 253 M1, 177 OD, 169 DH, 183 HE -0.1 to -0.9 %. The other 39 sounds' steps were dropped (194 KE
  and 197 OK +40 %). Reference samples regenerated. Tables 7324 bytes (every table, sizeof).

- **A vowel after NGE moves from NGE's frame 6 ms longer per ERB of F2 distance**, as after GE (VoxProbe 16, both units:
  NGE AW 242 ms and NGE UW 243 where ours gave 195; NGE IY, its F2 near NGE's, on time). VoxProbe 16 4.071 -> 3.947 dB.

- **A slowed vowel or glide above Speed 115 takes the vowels' Speed step**, as an unslowed one does (provisional; VoxProbe
  16, both units: Speed 118 FF RR Slow UW 200-210 ms, ours 225 -> 210; Man Goes Down phrase 2 1690/1685, ours 1725 -> 1675).
  VoxProbe 16 3.947 -> 3.949 dB.

- **The vowel join counts F1's distance at 0.6 of F2's**: cut 64 - 7.5 d ms (was 66.9 - 6.9 d, F1 and F2 equal). 20 recorded
  vowel pairs, both units: IY EH / EH IY overlap 18 ms where IY UW / UW IY run on 18, all 7.1 ERB apart. Pair lengths
  12.3 -> 8.3 ms rms; VoxProbe 13's pairs (section 5) 9.6 -> 8.6 ms mean error; VoxProbe 16 3.949 -> 3.849 dB; move timing
  unchanged (12.2 ms). Reference samples regenerated.

- **NE and NO overlap BE BO PE PO as other sounds do**; only KE and KO keep the no-overlap join (VoxProbe 13, both units: NE BO
  OW 250 ms, NO BO OW 257, NE PO OW 290; ours were 280 / 290 / 315, now 255 / 255 / 300; NE KO OW 290 unchanged). VoxProbe
  13's stop items (section 6) 8.9 -> 6.8 ms mean error, worst +32 -> +18; nothing else moves. Reference samples regenerated.

- **AY carries a +12 ms join term** (table; with F1 at 0.6 of F2 the AY joins sat 10-15 ms under the chip: AY IY 245 -> 235 ms
  against 230, AY EH and AY AW as the chip). 20 recorded vowel pairs 8.3 -> 7.3 ms rms; VoxProbe 13 section 5 8.6 -> 7.5 ms
  mean error. Tables 6823 bytes.

- **IH's +15 ms join term is back in the table** (it had dropped out by 0.5.5), and WW and RR take no term from the sound after
  them: VoxProbe 13's AW IH AW +28 -> under 15 ms, section 5 7.5 -> 6.7 ms mean error; Man Goes Down P2's IY WW IH 320 against
  325 (305 with IH's term on WW). Tables 6825 bytes.

### 0.5.5 (2026-11-18; technical manual 1.24)

Connected speech against a real unit (one unit's phrase recordings against the other's: 143.9 on the 88 items). Each rule
tested on every item, none worse; solo sounds unchanged.

Mechanisms in place of join rules:

- **The formants never jump.** Each sound starts where the last voiced sound left the formants, held through a hiss and a
  pause under 70 ms; SE and TT set them to the alveolar locus (F1 400, F2 1600). Replaces 0.5.4's carry across a short pause
  and the SE/TT start rule; 231.9 -> 225.3, and SE P4 AW and TT P4 AW, which no rule reached, 16-40 % closer.
- **A vowel glides at a rate**: 5 ms per ERB of F1/F2 distance (at least the common 25 ms), stretched by Speed and Slow.
  Replaces the slowed vowel after a nasal (60 ms); 225.3 -> 224.3.

The effects' tone switches (226 B6, the user's eye on its spectrum line: a mountain on the chip at 2.2-3.1 kHz where ours had
a valley):

- **A new tone takes the oscillator whose tone just stopped**, before an idle one: the chip carries the new tone on the old
  oscillator; ours started it fresh and cross-faded. B6 1.063 -> 1.046, B5 0.877 -> 0.860, no effect worse.
- **A tone's leap passes through its midpoint**: an oscillator already sounding whose frequency leaps by more than 25 % plays
  the midpoint of old and new for 1.5 ms, then the new (all four takes: a ~1.7 ms burst at the arithmetic midpoint at every
  part change - the hump between B6's 1.8 and 3.5 kHz tones, low at both ends and peaking in the middle; a constant-rate
  glide drew a slope). Not from silence, not into or out of a voiced part, not on a small step. All 43 effects neutral (X sum
  41.52 -> 41.53, none worse by 0.01).
- **B6's 4.5 kHz tones were the 3.5 kHz tone's DAC image** (the two lines sum to the 8026 Hz rate on every take; our output
  images too): removed, its tones held at the chip's 1803 / 3504 Hz, the high parts 3 dB up, its bed 150. B6 1.046 -> 1.016,
  its lines 1.78 -> 1.24. C2's two and A4's two (parts 6 and 7) were images too (they pair with a tone of their part at
  the rate): removed, C2 0.837 -> 0.836, A4 0.991 -> 0.994. B9's two pair with nothing (real tones near the band edge): kept.

**Joins rebuilt from VoxProbe 16** (2026-11-18, six takes of every join class alone and Man Goes Down with each control
stripped). The rules fitted to Man Goes Down through the 88-item phrase cost were compensating for each other (a correct
nasal overlap scored worse against them), so they are gone and the phrase cost is retired as a gate; the judges are
VoxProbe 16's own items, timing by F1/F2 tracks and levels in 16 bands on both units:

- **Removed** (each fitted on phrases, not on a recorded join): IY 5 ms shorter after a sound, LE 5 ms longer before one, a
  vowel 10 ms shorter after MM, a nasal 5 ms shorter and a slowed nasal 10 ms longer before a vowel, a low-F2 vowel 15 ms
  shorter into a nasal, a slowed vowel's F1 move into a nasal, TT 10 ms longer before a vowel, AW's 25 and 10 ms releases,
  a vowel's 20 ms fade-in after a short pause, a slowed nasal's quick attack, P2's "TT P1 BE" closure exception, and P1's
  "unless slowed" exemption from the nasal join.
- **A nasal overlaps the vowel after it by 13 ms, MM by 22** (both units: MM into a vowel hands over ~22 ms before the
  nasal's end alone, NE ~13, whatever the vowel - a formant-distance rule put NE IY and NE UW 22-32 ms early); scaled by
  Speed and Slow. **A vowel overlaps the nasal after it by 15 ms.**
- **Speech takes the speech length law below Speed 114 too**, (162 - Speed) / 48: the effects' law gave Speed 90 MM AW
  x1.84 where both units play x1.68.
- **An unvoiced stop after a pause closes 15 ms longer, not 30** (UX P1 EK 260 ms on both units, ours 270; Man Goes Down
  P4 930 against 930-935). 4.207 -> 4.168 dB.
- **NGE's and MM's second formants are the 2249 and 1430 Hz lines** (both units: the line that glides into the next vowel's
  F2), the near-silent 423 / 769 Hz ones third; alone sample-identical, their joins closer (4.168 -> 4.114 dB).
- **LE and LO join vowels as the nasals do**, off the vowel-distance rule (LE AW and AW LE within 6 ms; WW and RR stay on
  it, where they read 3.6-8.5 dB worse off it). 4.114 -> 4.093 dB.
- **A vowel's own Speed law below 114**, (149.3 - Speed) / 35.3 (both units: AW IY and MM AW at Speed 90 x1.65-1.68; the
  effects' law gave x1.73): Speed 90 MM AW 320 ms = the chip's, AW IY 450 against 435. 4.093 -> 4.071 dB.
- VoxProbe 16 join items (sections 3-8), levels: 4.569 -> 4.207 dB; move timing 14.3 -> 13.9 ms rms; Man Goes Down P1
  1480 ms against 1490 (without its Slows 1280 against 1305), P2 1725 against 1688, P4 945 against 932. Every solo sound
  unchanged. Open: Slow at Speed 108 and 118 (our stretch compounds with Speed: MM Slow AY +35 ms, P2 +37), AW IY at
  Speed 90 +25, NGE into a vowel.

The join rules first found (two now replaced by the held formants and the glide rate, above):

- **Straight after SE or TT a sound's F1 starts at 400 Hz at most**, beside 0.5.4's F2 at 1600 (both units, SE AW: the
  vowel's first ~12 ms on a low F1 under the 1.5 kHz line). All 12 SE/TT-into-a-vowel items closer; 247.5 -> 235.3.
- **A slowed nasal before a vowel or glide runs 10 ms longer** (Slow NE AW: 222 -> 109 and 317 -> 135); 235.3 -> 231.9.
- **A vowel after a short pause (P4, P5) fades in over 20 ms at most** (NE P4 AW: 305 -> 241, 355 -> 277; AW P4 AW
  closer); 249.4 -> 247.5.
- **A slowed vowel after a nasal moves over 60 ms, and into a nasal moves F1 to the nasal's over its last 60 ms** (Man
  Goes Down P1's MM Slow AY Slow NE, both units: F1 arcs where ours stepped; P1 548 -> 462, 620 -> 531); 251.4 -> 249.4.
- **A vowel straight after MM runs 10 ms shorter** (MM AW: the chip's AW 110 ms, ours 125; after NE any cut was
  worse); 258.1 -> 251.4.
- **No run-on for a slowed glide before a Slow** (0.4.x's 25 ms for Man Goes Down P4: with 0.5.4's rules ours ran ~15 ms
  long there and in VoxProbe 13's Slow IYUW Slow SE; P4 943 -> 594 and 801 -> 626); 264.0 -> 258.1.

### 0.5.4 (2026-11-17; technical manual 1.23)

- **Connected speech, measured against a real unit** (each unit's phrase recordings scored against the other's: 143.9 on the 88 phrase items, where the library stood at 309.6). Join rules, each tested on every item with none worse: a vowel or glide into TT moves F2 to the alveolar locus (1600 Hz) over its last 20 ms; an unslowed TT before a vowel releases 10 ms longer; straight after SE or TT a sound's F2 starts at 1600 Hz; across a pause under 70 ms the next sound starts from the last one's formants; an unslowed nasal hands over to a vowel 5 ms sooner; a vowel with F2 under 1500 Hz ends 15 ms sooner into a nasal; AW before P4-P6 fades over 10 ms; a slowed nasal comes in almost at once. Phrase items 309.6 -> 264.0; every solo sound unchanged.
- **The end fall extended to the liquids (LE LO WW RR) and to AW alone** (from silence, nothing after it): found by a detector of your valley fault over every sound (work/fresh/valleys.py, the picture's own spectrum). The worst excess fell from 6.0 dB (RR) to 2.7 (IYEH); RR X 0.757 -> 0.737, AW 0.853 -> 0.820; phrases unchanged.
- **A vowel or glide ending into silence slows its last periods** (your observation on OH AX UX: the valleys between harmonics far too deep; the chip's last periods run 11.48, 11.96, 13.25 ms as it fades and smear its upper harmonics): each new period in the last 25 ms up to 14 ticks long. 72 allophones X 0.848 -> 0.840 (OH 0.799 -> 0.746, AX 0.913 -> 0.880, UX 0.849 -> 0.813), AYWW +0.026; phrases 311.4 -> 309.8. Not AW (its own fade rules; with it every phrase ending on AW was worse).
- **Voiced speech carries the signal-held noise floor too** (your observation from the gallery: AX UX's spectra meet the chip's at the harmonic peaks and fall far under it between them; the valleys measured 5-19 dB too deep at h8): 72 allophones X 0.867 -> 0.848, 45 better, LO +0.017; phrases 313.0 -> 311.4, none worse. The voice itself stays periodic.
- **No long second period for a vowel straight after a voiced stop or a nasal** (phrase items 316.4 -> 313.0: BE IY, BE EH, Slow NE AW better, none worse); after silence, a pause or an unvoiced sound it keeps it.
- **A plain vowel's long voicing period is its second, not a delay before its first pulse** (both units: AW's pulses -23, -7, -1 dB with gaps 10.7, 12.3, 11.4 ms; EH 11.5, 12.9, 11.8): all 12 plain vowels better, X 0.906 -> 0.883; IH and OH no longer left out. Phrases 315.0 -> 316.4 (20 items better; 4 worse, a vowel straight after BE or NE).
- **IH re-fitted on the region score** (the faults the gallery shows): regions 8 -> 11, X 0.93 -> 0.86.
- **EH re-fitted on the region score** (the faults the gallery shows): regions 6 -> 4, X 0.81 -> 0.81.
- **NGE re-fitted on the region score** (the faults the gallery shows): regions 6 -> 5, X 0.78 -> 0.77.
- **OW re-fitted on the region score** (the faults the gallery shows): regions 7 -> 2, X 0.88 -> 0.85.
- **UW re-fitted on the region score** (the faults the gallery shows): regions 8 -> 5, X 1.02 -> 0.99.
- **OWIY re-fitted on the region score** (the faults the gallery shows): regions 8 -> 3, X 0.85 -> 0.81.
- **BE re-fitted on the region score** (the faults the gallery shows): regions 8 -> 6, X 1.04 -> 1.01.
- **C6 re-fitted on the region score** (the faults the gallery shows): regions 54 -> 46, X 1.11 -> 1.08.
- **C7 re-fitted on the region score** (the faults the gallery shows): regions 59 -> 46, X 1.06 -> 1.03.
- **C2 re-fitted on the region score** (the faults the gallery shows): regions 61 -> 43, X 0.86 -> 0.84.
- **A1 re-fitted on the region score** (the faults the gallery shows): regions 65 -> 30, X 0.59 -> 0.59.
- **B2 re-fitted on the region score** (the faults the gallery shows): regions 80 -> 70, X 1.04 -> 1.03.
- **B4 re-fitted on the region score** (the faults the gallery shows): regions 39 -> 32, X 0.97 -> 0.96.
- **The effects' noise bed doubles under a voiced part that pulses only some tones** (the robots: the chip's valleys between harmonics sit 10-15 dB over ours, not growing with the harmonic - a floor, not jitter): all 55 effects X sum 51.25 -> 51.07, regions 2064 -> 2043; R3 R4 R5 R7 R8 R9 C7 better, none worse beyond 0.006. Under every voiced part it was worse (A1 A2 A3 M2 C0 C2 C4 C3).
- **LE before another sound runs 5 ms longer than alone**: its solo takes want 110 ms, the phrase items 115 (88 items 312.0 -> 309.4, none worse).
- **D5 re-fitted on the region score** (the faults the gallery shows): regions 11 -> 8, X 0.77 -> 0.76.
- **EK re-fitted on the region score** (the faults the gallery shows): regions 12 -> 8, X 0.79 -> 0.73.
- **D4 re-fitted on the region score** (the faults the gallery shows): regions 12 -> 9, X 0.80 -> 0.80.
- **IYUW re-fitted on the region score** (the faults the gallery shows): regions 63 -> 3, X 1.19 -> 0.92.
- **GO re-fitted on the region score** (the faults the gallery shows): regions 13 -> 6, X 1.00 -> 0.96.
- **AX re-fitted on the region score** (the faults the gallery shows): regions 14 -> 4, X 1.00 -> 0.97.
- **EG re-fitted on the region score** (the faults the gallery shows): regions 14 -> 12, X 0.83 -> 0.81.
- **IY straight after another sound runs 5 ms shorter than from silence**: its solo takes want 130 ms, the phrase items 125 (88 items 321.5 -> 311.3, 7 better, none worse).
The re-evaluation release: every release since 0.1.0 scored the same way found the score itself at fault. X could place a
voiced sound one voice period off (the same sound scored 0.90 or 1.14 by a 1 ms change of its fade), and phrases that
open on a hiss were measured with the hiss outside their window (SE P1 AW: 15641 -> 129). With both fixed, several 0.5.x
choices turned out wrong and are undone or re-fitted below. Against 0.4.3, scored the corrected way: allophones 0.98 ->
0.89, effects 1.20 -> 0.95, joins 481 -> 306 (the published 1437 -> 513 was the faulty measure).

- **KO re-fitted on the region score** (the faults the gallery shows): regions 15 -> 11, X 0.80 -> 0.79.
- **OD re-fitted on the region score** (the faults the gallery shows): regions 18 -> 16, X 0.79 -> 0.77.
- **WH re-fitted on the region score** (the faults the gallery shows): regions 19 -> 16, X 0.75 -> 0.73.
- **HO re-fitted on the region score** (the faults the gallery shows): regions 19 -> 15, X 0.86 -> 0.71.
- **FF re-fitted on the region score** (the faults the gallery shows): regions 20 -> 16, X 0.79 -> 0.77.
- **LE re-fitted on the region score** (the faults the gallery shows): regions 20 -> 3, X 0.96 -> 0.93.
- **TH re-fitted on the region score** (the faults the gallery shows): regions 20 -> 6, X 0.74 -> 0.73.
- **PO re-fitted on the region score** (the faults the gallery shows): regions 20 -> 12, X 1.07 -> 1.04.
- **DE re-fitted on the region score** (the faults the gallery shows): regions 21 -> 15, X 1.20 -> 1.19.
- **IY re-fitted on the region score** (the faults the gallery shows): regions 22 -> 5, X 0.89 -> 0.88.
- **SE re-fitted on the region score** (the faults the gallery shows): regions 27 -> 22, X 0.93 -> 0.93.
- **ED re-fitted on the region score** (the faults the gallery shows): regions 22 -> 15, X 0.79 -> 0.78.
- **SO re-fitted on the region score** (the faults the gallery shows): regions 24 -> 24, X 0.72 -> 0.71.
- **EB re-fitted on the region score** (the faults the gallery shows): regions 24 -> 19, X 1.11 -> 1.05.
- **DO re-fitted on the region score** (the faults the gallery shows): regions 26 -> 20, X 1.09 -> 1.09.
- **SO re-fitted on the region score** (the faults the gallery shows): regions 31 -> 25, X 0.72 -> 0.72.
- **OB re-fitted on the region score** (the faults the gallery shows): regions 33 -> 27, X 1.08 -> 1.02.
- **R9 re-fitted on the region score** (the faults the gallery shows): regions 51 -> 42, X 1.09 -> 1.08.
- **R7 re-fitted on the region score** (the faults the gallery shows): regions 64 -> 57, X 1.40 -> 1.37.
- **B2 re-fitted on the region score** (the faults the gallery shows): regions 94 -> 80, X 1.04 -> 1.04.
- **A0: its pulse rate falls faster at the end** (the user's note: every line slopes down sharply at 300-340 ms): regions 72 -> 67, X 0.97 -> 0.98.
- **A7: its lines start lower and bend up** (the user's note; lines 6.38 -> 2.78): regions 138 -> 129, X 0.99 -> 0.99.
- **C7 re-fitted on the region score** (the faults the gallery shows): regions 89 -> 56, X 1.07 -> 1.08.
- **R7 re-fitted on the region score** (the faults the gallery shows): regions 79 -> 65, X 1.39 -> 1.39.
- **C4 re-fitted on the region score** (the faults the gallery shows): regions 43 -> 28, X 1.10 -> 1.00.
- **C3 re-fitted on the region score** (the faults the gallery shows): regions 49 -> 31, X 0.83 -> 0.76.
- **IH re-fitted on the region score** (the faults the gallery shows): regions 48 -> 8, X 0.99 -> 0.93.
- **SE re-fitted on the region score** (the faults the gallery shows): regions 34 -> 27, X 0.97 -> 0.93.
- **SE re-fitted on the region score** (the faults the gallery shows): regions 45 -> 34, X 0.96 -> 0.97.
- **GE re-fitted on the region score** (the faults the gallery shows): regions 19 -> 8, X 0.96 -> 0.91.
- **D7 re-fitted on the region score** (the faults the gallery shows): regions 25 -> 20, X 0.70 -> 0.70.
- **A6 re-fitted on the region score** (the faults the gallery shows): regions 26 -> 16, X 0.94 -> 0.88.
- **A6 re-fitted on the region score** (the faults the gallery shows): regions 32 -> 26, X 0.95 -> 0.94.
- **AY re-fitted on the region score** (the faults the gallery shows): regions 33 -> 16, X 1.01 -> 0.99.
- **NGO re-fitted on the region score** (the faults the gallery shows): regions 36 -> 20, X 1.02 -> 0.98.
- **AXUW re-fitted on the region score** (the faults the gallery shows): regions 38 -> 20, X 1.17 -> 1.17.
- **C9 re-fitted on the region score** (the faults the gallery shows): regions 34 -> 22, X 1.39 -> 1.40.
- **JH re-fitted on the region score** (the faults the gallery shows): regions 49 -> 23, X 0.91 -> 0.83.
- **A5 re-fitted on the region score** (the faults the gallery shows): regions 52 -> 34, X 0.62 -> 0.62.
- **TS re-fitted on the region score** (the faults the gallery shows): regions 52 -> 25, X 0.93 -> 0.90.
- **AXRR re-fitted on the region score** (the faults the gallery shows): regions 54 -> 9, X 0.79 -> 0.77.
- **C5 re-fitted on the region score** (the faults the gallery shows): regions 178 -> 143, X 0.95 -> 0.96.
- **C1 re-fitted on the region score** (the faults the gallery shows): regions 56 -> 47, X 0.95 -> 0.94.
- **B9 re-fitted on the region score** (the faults the gallery shows): regions 87 -> 45, X 1.10 -> 1.00.
- **IYEH re-fitted on the region score** (the faults the gallery shows): regions 68 -> 5, X 0.95 -> 0.95.
- **C6 re-fitted on the region score** (the faults the gallery shows): regions 75 -> 54, X 1.11 -> 1.11.
- **C0 re-fitted on the region score** (the faults the gallery shows): regions 70 -> 40, X 1.07 -> 1.05.
- **C0 re-fitted on the region score** (the faults the gallery shows): regions 95 -> 69, X 1.09 -> 1.07.
- **M0 re-fitted on the region score** (the faults the gallery shows): regions 99 -> 48, X 0.92 -> 0.83.
- **PE: its low pulse decays over 50 ms (was 35)**: regions 10 -> 10, X 1.07 -> 1.05.
- **C2 re-fitted on the region score** (the faults the gallery shows): regions 110 -> 62, X 0.92 -> 0.86.
- **136 AW fades out over 35 ms** (its own takes: 0.92 -> 0.90), **25 after a pause, a slowed sound, LE or AYWW**
  (the chip ends it sharper there: Slow NE AW, Slow AY AW, AYWW AW, LE AW 19-30 % closer on at least one unit; the two
  units disagree on TT P1 AW, AW P1 AW, SE P1 AW). **EH** takes the default fade-in (0.84 -> 0.81), **AYWW** 60 ms
  (1.01 -> 0.97).
- **The effects' output through a 40 Hz high-pass**: on all four takes nothing sounds under ~50 Hz, where B2 A0 C7 R5 R7
  were 15-22 dB up (the AtariVox's output stage). Effects' mean X unchanged.
- **218 A8's lines as the chip draws them**: both rising tones start from ~150 Hz, not 210-260, and the first sound's
  upper line bends down to ~520 Hz at its end. Regions 157 -> 117, X 1.08 -> 1.05.
- **217 A7's parts smoothed**: a 5 ms part at full level and level jumps between parts drew broadband stripes the chip
  hasn't; its opening burst 8 dB quieter. Regions 190 -> 143, X 1.00 -> 0.99.
- **235 C5 re-fitted on the region score**, its second part's low tone dropped (not on the chip there): regions 200 -> 169,
  X 0.96 -> 0.96.
- **214 A4 is pulsed at 65 Hz from 105 to ~190 ms**: on all four takes a low block (energy ~130 Hz, stripes every 15.3 ms)
  runs under the sweep there; a voiced 130 Hz formant at 65 Hz (Pitch-23) beside the plain sweeping tones (flag o), its
  second part split at 105 ms. Regions 278 -> 65, X 0.97 -> 0.99.
- **252 M0's noise through its resonance twice**, the band twice as wide: the chip's band falls away steeply each side,
  where one stage left a dark block under 1 kHz (by picture); X 0.93 -> 0.92.
- **PE and PO's low pulse swells in over 25 ms, not 10, and PO's pulse runs at 62 Hz, not 53 (it fell a cycle behind the chip's by 60 ms), from 12 ms after a 4 ms closure, with a 45 ms decay**, judged by picture:
  X barely sees the band (under the recordings' hum). On the chip the pulse starts ~15 ms after the noise and builds to its
  deepest at 40-60 ms; ours started at ~30 ms and peaked at once.
- **199 PO back on its own timing** (closure 8 ms, low pulse at 28): 0.5.3's shared PE/PO timing was chosen on the
  slipping alignment; aligned on the whole sound PO is 1.10 with it, 1.06 without. PE keeps the 15/25 (1.05 against 1.10).
- **Release fades (rr) re-fitted on the same alignment**: BE 1.12 -> 1.04, TS 0.99 -> 0.93, LO 0.86 -> 0.80, AYWW 1.06 -> 1.01;
  no phrase changes. AW's (35 better alone) moves its phrases both ways (-55 % to +44 %) and stays.
- **Fade-ins (aa) re-fitted on an alignment that can't slip a pitch period** (0.5.0's were chosen on one that could):
  IY 0.94 -> 0.89, IH 0.91 -> 0.87, OW 0.91 -> 0.88, IYRR 0.79 -> 0.71, EYRR 0.86 -> 0.78, AWRR 0.89 -> 0.82, EYIY 0.91 -> 0.84, OWIY 0.87 -> 0.84, OHIH 0.91 -> 0.81, IYEH 1.05 -> 1.01, IYUW 0.96 -> 0.92. Phrases 313.2 -> 311.9, none further. AYWW and EH keep theirs (their phrases got worse).
- **158 IYEH 10 % longer** (225 -> 247.5 ms: 1.01 -> 0.97) and **134 UX 2 dB louder** (0.96 -> 0.94); phrases within 2 %.
- **221 B1, 224 B4, 225 B5, 230 C0, 252 M0, 254 M2, one part each**: B1 0.97 -> 0.94, B4 0.99 -> 0.96, B5 0.91 -> 0.87,
  C0 1.13 -> 1.10, M0 0.97 -> 0.93, M2 0.62 -> 0.60.
- **202 R2, 205 R5, 211 A1, one part each**: R2's first part 2 dB louder (1.04 -> 1.01), R5's second part 10 % shorter
  (1.15 -> 1.13), A1's first part 2 dB quieter (0.62 -> 0.60).
- **206 R6, 207 R7, 239 C9, one part each**: R6's first part 2 dB louder (1.30 -> 1.26), R7's first part 10 % shorter
  (1.42 -> 1.39), C9's first part 10 % longer (1.42 -> 1.40); further either way scored worse.
- **171 BO's bar 2 dB louder** (bl -10 -> -6): 1.20 -> 1.18; no phrase changes. **B0** fades out over 10 ms
  (0.79 -> 0.77).
- **Effects' own fade-ins and fade-outs re-fitted on the corrected score**: a robot sound voiced from its first part
  (R2-R4, R6-R9) fades in over 5 ms (R2 1.08 -> 1.04, R4 1.20 -> 1.16, R6 1.34 -> 1.30, R8 1.21 -> 1.18); B5 B8 B9 R8
  fade out over 35 ms (B5 0.96 -> 0.91, B8 0.95 -> 0.91); B0 starts at once (0.88 -> 0.79); D0 fades out over 10 ms
  (0.91 -> 0.86); D8 fades in over 2 (0.80 -> 0.78). No effect further.
- **The 58-tick voicing fall for every vowel again.** 0.5.1 gave EY UX UH AW a 72-tick fall; X then chose it on an
  alignment that can lock a voiced sound a pitch period off. Aligned on the whole sound: AW 1.07 -> 0.92, UH 1.04 -> 0.92,
  EY 1.04 -> 0.93, UX 1.03 -> 0.96, nothing else changes; phrases 321.7 -> 313.2, none further.
- **A true nasal after a sound moves in 5 ms, not 25** (MM NE NO NGE NGO): on both units the vowel's F1 is gone from
  the nasal at once; ours carried AW's ~700 Hz 10-29 dB into MM. AW MM -23..-26 %, IY MM -13 %, Man Goes Down P1 -8..-10 %;
  phrase cost 328.5 -> 321.7, nothing further. LE LO WW RR (stored with the nasals) keep 25 ms.
- **187 TT before a vowel is 3 dB louder** (pauses and controls between allowed): on both units the burst sits at
  -24..-29 dB against the vowel, ours at -30..-34. TT alone unchanged; phrase cost 378.9 -> 328.5 (TT P4 AW -78 % on
  unit B, Slow TT AW -56 % and -27 %); TT AW +4 % on unit A where unit B's is -20 %.
- **136 AW starts fast before an unvoiced sound or LE** (a 20 ms fade-in, where its own 45 ms and the 35 ms default
  applied; AW LE 17-31 % and AW SE 16 % closer on top of the figures here): on both units AW TT and AW LE are 7-18 dB louder than ours
  at 400-800 Hz over AW's first 20-40 ms, where AW's 45 ms fade-in held it down; before a vowel, RR or MM the fade-in
  fits. AW TT -39 %, AW LE -14 % and -19 %, phrase cost 385.0 -> 378.9 (no lag fit), nothing further than +2 %.

### 0.5.3 (2026-11-16; technical manual 1.21)

Allophones mean X 0.93 -> 0.92 (0.4.3: 1.07), effects 0.97, joins 514 -> 513 (0.4.3: 1437). Two new mechanisms: nasals
and voiced stops' bars carry a noise part, as the chip's do.

- **BE and BO's bar above F1 is noise**, not weak formants (1800 Hz, 1400 wide, level 6, F2 F3 off in the bar): on both
  units above 700 Hz the part of the bar that differs pulse to pulse carries nearly all of it. BE 1.21 -> 1.14,
  BO 1.24 -> 1.20; phrases 513.0 -> 512.7 (BE EH -4.8 %; BE IY -3.0 % on unit A, +3.2 % on unit B: the units disagree).
- **An alveolar stop's bar is partly noise** (DE DO ED OD: 1000 Hz, 800 wide, level 4): on both units their bar's pulses
  match one another only 0.40-0.59 above 700 Hz (ours 0.76-0.81). DE 1.29 -> 1.24, DO 1.14 -> 1.09, OD 0.84 -> 0.79,
  ED 0.81 -> 0.80; phrases unchanged (513.0, the largest move +0.24 %). Labial bars stay without it (it cost BE IY
  4-11 %); the velars' bar is periodic on the chip.
- **Nasals carry a noise part**: 140 MM, 144 NGO, 145 LE and 146 LO get the band NE NO NGE already had (2400 Hz,
  1200 wide, level 3). On the chip a nasal's pulses differ one to the next above 700 Hz (consecutive-pulse match
  0.73-0.83 on all four takes of MM and NE, ours 0.94-0.97). MM 1.21 -> 1.01, NGO 1.09 -> 1.02, LE 1.16 -> 1.09,
  LO 0.87 -> 0.86; phrases 513.9 -> 513.0.
- **198 PE and 199 PO** share one timing: a 15 ms closure and the low pulse at 25 ms (PE's were 38 and 50, PO's 8 and
  28; the chip's noise starts ~10 ms before ours did). PE 1.11 -> 1.05, PO 1.09 -> 1.07, no phrase changes.
- **161 AXUW**: F3 holds with F1 until the late fall (the chip keeps ~2350 Hz), where it slid from AX's to UW's
  through the move. X 1.21 -> 1.18, no phrase changes.
- **194 KE** starts quieter: its first 5 ms 3 dB down instead of 1.5 dB up (og 3 -> -6), X 0.84 -> 0.76, no phrase changes.
- **156 NGO** starts quiet: its first 15 ms 6 dB down (ob 15, og -12; the chip's lead-in), X 1.15 -> 1.09, no phrase
  changes. The Go library's onset-frame rule now matches C's (any sound with an onset, not only voiced fricatives).
- **238 C8's noise sweep** moves linearly in Hz, not geometrically: the chip's band runs in straight lines between
  about 800 and 2950 Hz on both units, where the old sweep curved. Its period is 20 ms, not 21 (the chip's cycle is
  19.5 ms on both units). 1.75 -> 1.63 (0.4.3: 1.89). Only C8 uses the sweep; no other sound changes.
- **Docs**: the manual's Appendix A and B regenerated from the tables (they had drifted); the README's Man Goes Down
  phrase 2 length corrected to 1705 ms (it read 1690), phrase 4 dropped from that table (the recordings' labels end
  before its final EK burst); 136 AW and 158 IYEH are 0.01-0.02 further than 0.4.3, where the summary said none.

### 0.5.2 (2026-11-16; technical manual 1.20)

- **237 C7's pulse rate** follows the chip's track: Pitch 88 for its first section, then rising, where its fitted
  offsets ran its pulses 1-1.5 ms too often from 85 to 130 ms, so each pulse had too little time to die away before
  the next (both units' pulse times; between pulses the chip falls to -24..-36 dB, ours to -21..-23). The offsets
  were searched by X from the chip-measured track: 1.17 -> 1.07 (0.4.3: 1.12). No effect is now further from the
  chip than 0.4.3; effects mean 0.98 -> 0.97.
- **Checked and ruled out:** a shared, replayed noise sequence. Takes first appeared to share one noise waveform
  (0.45-0.71); it was the recordings' mains hum (different sounds match as well; above 500 Hz the takes are
  independent). The noise stays random.

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
