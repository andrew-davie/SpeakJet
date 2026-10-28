# Notes moved out of speakjet.c

The measurements and fitting history behind the library's tables and constants, kept here when the comments in
`speakjet.c` (and the Go port) were cut down to what explains the code. Each section is headed by the line of code
the note was written above. See also `design.md` and `CHANGELOG.md`.

## `#define VOICE_FADE_STEP_MS 0.25`

the chip's voicing fade: the level of the formant oscillators after each voicing pulse, every
VOICE_FADE_STEP_MS from the pulse, in between in a straight line, and after the last nothing (see
ENV_VOICE in synth_sample()). measured from the waveforms of the third VoxProbe recording, where at
pitch 40 each pulse stands alone and about 24 of them were averaged, on 18 vowels, nasals and
liquids on two AtariVoxes. each pulse was fitted as three sines starting together, times one
envelope shared by every sound, through one output filter (see CHIP_RATE); the envelope and the
filter were solved for along with each sound's frequencies and amplitudes. that leaves 1/470 of the
pulses' energy unexplained (-26.7dB), against 1/28 for the straight 8ms ramp first used. the fade
starts at full level, dips to about 0.7 within half a millisecond and comes back to about 0.8 (the
onset shapes the whole spectrum: smoothing it into a flat start costs the single pulses 1.8dB),
holds to 2ms, falls almost in a straight line and is gone by 7.5ms. fitted to either unit alone it
fits the other within 0.2dB, and fitted the same way to pulses this library makes it gives back this
library's own envelope, so the method doesn't shape it. Volume and Bend don't change it, and nor
does Pitch from 25 to 60

## `#define ONSET_MS 40.0`

how long the chip takes to bring a vowel, nasal, liquid or diphthong's voicing up from silence: in a
straight line, from nothing to full level. on the VoxRecord recording, both units, the first three
voicing periods of the 37 sounds played alone stand at -17, -6.3 and -1.5dB against the rest
(medians), which 40ms gives to within 1.4dB; ramping the volumes alone gave -8, -2.7 and 0, because
the mixer's limit (MAX_MIXER1) holds the sum of the formants at full level long before the volumes
get there. AW MM at eight pitches from 60 to 250 takes about 30ms throughout on both units, so it is
a time rather than a number of periods. the voiced stops and fricatives start as they did: the ramp
on them made both units match them worse. the nasals come up faster: measured from where they rise
out of the recording's background (which, being quieter sounds, they sit nearer to), both units have
them within 3dB of full level by the second period. 25ms fits them best (40ms 90.7%, 25ms 91.4%)

## `#define CHIP_RATE 8028.0`

the chip's output. it makes its samples at CHIP_RATE, not the manual's 8192 a second: on both units
of the VoxRecord recording every steady tone has a mirror image at the rate less the tone (B7's
3915Hz at 4124Hz, 1dB down), which puts the rate at 8040Hz on one unit and 8015Hz on the other
(their clocks differ by the same 0.3 percent as their pitches), the same as their voicing ticks.
each sample is held until the next, so the images repeat above that, around each multiple of the
rate, at the levels holding gives, up to 23kHz on the recordings, and a smooth analogue low pass
after the chip shapes them all.  that low pass is measured twice. below 4kHz the voicing pulses of
the third VoxProbe recording give it, level and phase, on both units: -1.2dB at 1kHz, -2.8 at 1.5,
-5.4 at 2, -8.9 at 2.5, -11.8 at 3 and -14.3 at 3.5kHz against 500Hz, holding included (it agrees
within 2 to 3dB with the tone sweeps of the VoxRecord recording, -10.8 at 2.9 and -15.8 at 3.6kHz).
above 4kHz the images give it: 160 of them from 26 tone effects on both units, each against its own
tone, less what holding accounts for (within 2dB of each other): -18dB at 5.5kHz, -23 at 7.5, -27 at
10.5, -33 at 15.5 and -38 at 23.5kHz. it includes whatever the AtariVox and the recorder add.  so
here the chip's samples are held, each output sample is the average of what is held over its time,
and output_sos (a filter at SJ_SAMPLE_RATE, fitted to both measurements, within 0.3dB RMS below 4kHz
and 1.6dB above) is the low pass. the level at 500Hz is the one the output had before

## `#define SKIRT_DB (-24.0)`

the chip's output has a floor of noise under whatever it plays: on both units of the VoxRecord
recording a pure tone (the DTMF tones, B3, R3 and the rest) sits on noise from 100Hz to 3kHz 40 to
55dB under it, above the recording's background, which this library's clean tones left out, every
band between the tones counting as missing. here white noise follows the output's level (over about
25ms) SKIRT_DB under it: the effects of the VoxRecord recording 5.94 to 4.57dB with their
spectrogram match within a tenth of a point, and the speech unchanged (it hides under the harmonics)

## `#define EQ_BANDS 10`

the consonants' shaping (see sound_eq): EQ_BANDS peaking filters half an octave apart, each made
again whenever the gain asked of it changes, then the overall gain. not when it has moved some step:
compilers that fuse a multiply and an add (clang on arm64) and those that don't (gcc on x86) differ
in a gliding gain's last bit, which moved the sample a step was crossed on, and the filters carried
that on, 21 apart in the pangram

## `#define STEADY_LENGTH 1.5`

the chip's lengths for speech sounds, measured from a recording of a real SpeakJet playing Alex
Herbert's vox_test ROM (2004), compared sound by sound with this library playing the same codes. at
the default speed the steady sounds (vowels, nasals, liquids, fricatives) last about 1.5 times the
manual's length (EH 2.07, IH 1.6, NGE 1.43, and SE 1.67, TH 1.42, NE 1.14 when slow), diphthongs
about 1.2 times (EYIY 1.0, IHWW 1.5, and OHIY 1.2, AXRR 1.3 when fast), and voiceless stops what the
manual gives (TT 1.0). at speed 127 the same steady and stop lengths and the speed curve below give
the recording's syllable length to within a few milliseconds. the stops and affricates still use
these; the other allophones now have their own lengths from a later recording (see allophone_length)

## `typedef struct formants`

formant targets. frequencies are approximate values for an adult male speaker, from published
measurements of English vowels and consonants, except where two independent measurements of the real
chip agree.  measured on the chip: where two independent measurements of a sound's formant agree, it
is set to their mean. they agree when they are within 10% of each other or within 88Hz (the spacing
of the voice's harmonics at the default pitch, about as finely as a formant can be read from a
recording). the measurements are the openFPGA-7800 AtariVox engine's, read from recordings of games
speaking (docs/ATARIVOX.md, MIT licence), and those behind Stella's SpeakJet engine
(feature/speakjet branch). formants where the two disagree, or that only one of them measured, keep
their published values:  OW: F1 F2      AX: F1 F2 F3   RR: F1 F2 F3   UX: F1 F2   IH: F1 F2 F3 MM:
F1 F3      EY: F1 F2 F3   EH: F1 F2      AW: F2  the third take of the VoxRecord recording (see
msa_speed_factor()), whose vowels barely clip, confirms AX, UX, EH, EY, IH, OW, UH, LO and RR to
within about 5% (formants read by linear prediction, the same way on the recording and on this
library's own output, so that the method's errors cancel). it corrects five: AW F1 (590 to 695Hz),
UW F1 and F2 (300 to 380, 870 to 1065), AY F1 and F2 (660 to 710, 1720 to 1660), OH F1 and F2 (730
to 690, 1090 to 1150) and WW F1 and F2 (300 to 495, 650 to 765). IY can't be read this way. the
nasals (MM among them) are fitted to its spectra instead (see allophone()).  the second VoxProbe
recording then measured the oscillators directly: at pitch 40 each voicing pulse stands alone, a few
milliseconds of the three formant sines starting together, so their frequencies can be fitted to the
waveform itself. IY, IH, EY, EH, AY (but its weak F3), AX, UX, OH, AW, OW, UH, UW, LE, WW, RR and MM
take those frequencies (NE and NO were worse with them on both recordings, and keep theirs). the
biggest changes: IY F3 3010 to 2539Hz, LE F2 1300 to 970, WW F1 495 to 385, MM F1 396 to 257. with
them the sustained sounds of the first VoxProbe recording (not used to measure them) go from 4.5 to
3.7dB of the chip's spectra.  the third VoxProbe recording measures them again, on two units, with
the chip's own fade and output filter (see voice_fade): IY, IH, EY, EH, AY, AX, UX, OH, AW, OW, UH,
UW, LE, WW, RR, MM, NE and NO are those, the mean of both units (unit B's clock is 0.29% slow),
which agree within a few hertz. F1 and F2 of all 18 are whole multiples of about 88.4Hz less 3.5Hz,
within 2.2Hz RMS

## `static frame formant_phases(frame f, double p2, double p3)`

the chip doesn't restart all three formant sines at the same phase: fitted to the single pulses of
the third VoxProbe recording with the chip's fade and output filter (see voice_fade), each sound's
second and third formants start at their own phase against the first, the same on both units (IH's
0.13 and 0.17 of a cycle ahead, MM's 0.25 and 0.27, IY's 0.92 and 0.93). they shape the dips between
the formants: restarting all three together, as first assumed here, leaves those dips up to 10dB too
deep (IY, MM, NE) and the single pulses 4.5dB from the chip's rather than 2.0. formant_phases gives
a voiced frame those phases. but measured at pitch 40, they don't all hold at speaking pitch:
against the VoxRecord recording at the default pitch (background removed, unit B and unit A's
unclipped sounds) and the first VoxProbe recording at pitches 88 and 60 (which nothing here was
fitted to), IY, EY, EH, AY, AX, UX, OH, AW and MM match better without theirs (IY 85 to 91% and 3.2
to 2.0dB, and 3.4 to 2.2dB on VoxProbe), so those restart together; OW, UH, UW, NGE, NGO, NE, LE, WW
and RR keep theirs

## `#define FRICATIVE_DB 17.0`

how much quieter the chip's noise is than the levels first chosen here. measured from the vox_test
recording (see sound.length) by comparing each noise sound's level against the vowel beside it, on
the chip and here. on the chip hiss sits 16 to 23dB under the vowels; here it was level with them.
the corrections: SE 23dB, CH (the SH frame) 16.4dB, TH 11.8dB, FF 13.7dB, stop bursts 19.5dB (TT
23dB, KE 16dB) and aspiration 14.5dB (TT). sounds the recording doesn't have take the mean of the
fricatives that it does. the voiceless fricatives now have these levels built into their frames (see
frSE())

## `static frame frSE(void) { return noise_frame(lAlveolar, 3629.7, 2609.1, 25.87, 5.78, 33.4); }`

frames for the fricatives SE, SO and SH. the noise of the voiceless fricatives (here and HE, HO, WH,
FF and TH below) is shaped to the VoxRecord recording (see msa_speed_factor()). HE, HO and WH have
their spectra on the chip, measured in third octaves from 250Hz to 4kHz in three takes, fitted
through the output filter here. SE, SO, SH, FF and TH, like the stops and affricates (see STOPN()),
were fitted further, as whole sounds: each was played here from a set of values, which a search
(differential evolution) moved to bring its spectrogram closest to the chip's in the first and third
takes together, and its length to the chip's. two takes rather than one, because on the hiss sounds
the chip's own takes match each other only 30 to 70%, so fitting one fits that take's noise. the
chip's hiss is narrow bands, peaked where these were broad: SE and SO at 3 to 4kHz, SH at 2.5 to
3.2kHz, HE and HO around 1 to 1.5kHz, WH at 0.6 to 0.9kHz. their levels are set by
allophone_gain_db. all of them were then fitted again to both units at once (see STOPN())

## `#define STOP(ms_, cl_, bu_, vo_, lo_, bf0, bf1, bv0, bv1, af0, af1)            \`

stop descriptions: duration, closure, burst, voiced, locus, burst frequencies, burst volumes,
aspiration frequencies. STOP() gives a stop the first noise widths, aspiration volumes and voicing
(see stop), and STOPN() gives them too. the stops that use STOPN() (BE, EB, OB, ED, OD, TT, TU, KE,
EK and OK) and the affricates (CH, JH and TS) were fitted as whole sounds to the VoxRecord recording
(see frSE()). the chip's bursts and aspiration are narrower noise than first chosen here (TT and TU
hiss at 2.6 to 3.9kHz, which the first values spread from 1 to 2kHz), its voiced stops' loci differ
from the published ones, and EB, OB, ED and OD last 25 to 35ms on the chip, not 10. a closure that
is silent can't be measured from a sound played alone, so those keep the length they had (35 to
42ms, 7ms for ED and OD), and TT's comes from the connected groups (see allophone_length). the other
stops' burst and aspiration frequencies were scaled together (by 0.5 to 1.15) to bring each spectrum
closest to the recording's third take; most of the chip's bursts are lower than first chosen here
(PE and PO by half).  all of them but BE, BO, DO, ED, OD, GE, GO, EG and OG (which are played from
the chip's frames: see vf_points) and TT were then fitted again, with the output filter measured
from the voicing pulses (see CHIP_RATE), to both units' VoxRecord recordings at once: the
spectrogram, the spectrum and the level over time, each unit's level compared down to its own
background, so that a part the chip doesn't play costs as much as one it plays wrongly (the first
fit had put faint tails and tones at the background's level, where they cost nothing). each keeps
the length it had (see allophone_length), which comes from the connected groups; fitted to the
sounds alone the lengths came out shorter, and connected speech went from 21 to 60ms out. TT keeps
its values, which the connected groups fix: its refit made groups ending in TT 100ms short. with
these KE went from 54 to 74% spectrogram match and 11.3 to 4.2dB, KO 55 to 81%, EK 71 to 88%, OK 68
to 86%, DE 44 to 86%, JH 49 to 80% and CH 65 to 75%

## `static const double allophone_length[] =`

how much longer the chip plays each allophone than the manual's length (what its parts add up to),
at the default speed. measured on both units of the VoxRecord recording, alone and in connected
groups (the chip gives a sound in a group a longer slot than played alone), and refitted on every
recording of both units together (see final_length). the framed sounds (see vf_points) are the
measured length already, so theirs is near 1

## `static const double final_length[] =`

final_length multiplies an allophone's length when it is the last before a pause or the end and
isn't repeated: on the chip a vowel played last is longer than the same vowel inside a phrase (IY
and AW 1.25 times, EH 1.18) and TT and FF shorter (0.64, 0.79), which no one length fitted both
ways. fitted on every recording of both units, each sound kept only where no kind of recording came
out worse

## `static const double copy_length[] =`

the copies after the first, when a sound is repeated, as a multiple of the manual's length (see
msa_expand()), fitted the same way on the fourth VoxProbe recording's repeats (five of each
diphthong and voiced stop, ten of each at pitch 40) and VoxRecord's: on the chip many copies are
shorter than the manual says (LO and DO 0.75 and 0.7 times, ZZ 0.69, EB 0.51), which left the copies
drifting out of step: LO 1.14 to 0.25, ZZ 1.44 to 0.87, DO 1.24 to 0.91

## `static const double allophone_gain_db[] =`

how much louder (in dB) the chip plays each allophone than the oscillator volumes here give, against
AW. fitted to the VoxRecord recording's third take, whose vowels barely clip: each allophone's level
(where it is within 20dB of its loudest) was matched to the recording's, measured the same way,
relative to AW. the vowels needed little; the nasals, liquids and hiss were too loud here (the hiss
levels were set from an earlier, clipped recording, which made the vowels read quiet) and BE, BO, PE
and PO too quiet

## `typedef struct vf_point`

some voiced sounds are played as the chip plays them, frame by frame. at the default pitch each
voicing pulse of the VoxRecord recording stands alone (the fade is over long before the next pulse),
so each pulse of every voiced sound alone was fitted as three formant sines with the chip's fade and
output filter (see voice_fade), on both units, and the two units' pulses paired and averaged. each
point is a pulse: its time after the one before, the glide to it, the three frequencies and their
volumes (an absolute level: AW's first formant is 24). a formant too weak to measure takes the
frequency it has where it is strongest. these are the sounds whose recordings this brings closer on
both units: the diphthongs, which move between their formants in steps rather than one straight
line, the voiced fricatives, which on the chip are mostly voicing, and voiced stops such as BO and
GE, which are a voice bar and then a short vowel-like release with no noise burst to speak of. their
length is the one measured, not stretched (see allophone_length)

## `static const struct`

the hiss of ZZ and ZH. their frames are the voicing alone, measured where nothing above 4kHz was
compared, but both units have hiss at 3.5 to 4.5kHz 20 to 24dB under the voicing's loudest band, 7
to 8dB more than the voicing gives (ZZ and ZH at pitch 40 on the fourth VoxProbe recording, and ZZ
in Man Goes Down). here oscillators 4 and 5 give it, half enveloped by the voicing and following
each frame's level; their frequencies, width and level are fitted to the fourth VoxProbe recording,
where ZZ goes from 3.8 to 1.9dB and ZH 5.2 to 2.4dB (full band). left out, the VoxRecord recording's
ZH goes from 3.9 to 1.8dB, and Man Goes Down's ZZ is within 1dB at 3.5 and 4.5kHz

## `case 128: steady(s, 70, voiced_frame(fIY, 24.0, 15.6, 9.8)); break; /* IY */`

the vowels' and liquids' three volumes are the chip's: at pitch 40 on the third VoxProbe recording
each voicing pulse is the three formant sines alone, and fitting them through the chip's fade and
output filter (see voice_fade) gives each one's amplitude, the mean of both units. some of these
sounds are instead played from the chip's own frames, pulse by pulse (see vf_points)

## `case 143: steady(s, 70, formant_phases(voiced_frame(FM(368.334, 2266.3, 2386.423), 22.0, 6.845, 2.093), 0.225, 0.909)); break; /* NGE */`

NGE and NGO had no measurement (VoxProbe 3 doesn't play them), but on the VoxRecord recording both
units agree on every harmonic of them to within 0.5dB RMS. their frequencies, volumes and phases are
fitted to those harmonics, played here: NGE 87.0% to 97.0% and 3.93 to 1.11dB, NGO 93.1% to 96.2%
and 2.98 to 1.83dB. on the chip NGE's second and third formants are one peak near 2.3kHz, where they
had been 2.1 and 2.7kHz

## `const frame hiss = noise_frame(lAlveolar, 3282.5, 3250.4, 8.04, 19.69, 48.6);`

on both units (the VoxRecord recording) TS is hiss that starts at its loudest, peaked at 3.5kHz, and
dies away: 6dB down 40ms on, 14dB by 120ms, spreading out to a flat spectrum with a little left at
3.5kHz. the fit as a stop had it the other way round, a long broad burst into a short hiss at the
end. the hiss here is that fit's, then the same oscillators spread wide (distortion) and moved up,
gliding down to 20dB under it. against both units this matches 37% (13% before) where they match
each other 35%, its spectrum 3.7dB off (3.5 before, 3.5 between the two)

## `enum { FX_TONE, FX_NOISE, FX_BUZZ };`

the manual describes the sound effects only by name and length. the others here are traced from the
third take of the VoxRecord recording (see msa_speed_factor()), which plays each on its own and
clips little: every 5ms the level and the sound's kind were measured, and the trace reduced to the
points below, between which the sound glides in a straight line. a tone is up to three pure sines
(the chip's tones have no harmonics to speak of, 45dB and more down): the strongest peaks, within
15dB of the loudest, that aren't its harmonics (R0 to R9, for example, are three tones each). noise
(a flat spectrum) is noise around the frequency. a buzz (a pitch between 40 and 500Hz found by
autocorrelation, with six or more peaks) is voiced at that pitch with oscillators 1 to 3 on its
three formants, read by linear prediction: A1, A2, A3 and M2 buzz at 87Hz, the default speech pitch,
and M2 ("WOW") is a vowel glide. each point is where it is reached (in ms from the start of the
sound), its kind, the buzz's pitch, its level in dB (-60 is silence), the three frequencies (0 for
none) and the second and third's levels against the first (-60 while a tone that comes and goes is
absent, so that it fades rather than sweeping from 0Hz). the levels are the recording's with the
output filter's loss at each frequency added back (see CHIP_RATE), as the filter here takes it off
again. many effects are nothing like their names suggest, and many differ in length from the manual:
A1 and A2 last about a second, and M0 is a 1.2 second tone around 1.8kHz that dies away. the points
at the start and end of an effect that were no louder than the recording's background (within 6dB of
the first point) are silent: the trace had taken that background for part of the effect, making each
one 20 to 50ms too long. the tone effects are traced again in fx_knots.  A1 is traced again: its
trace had taken the strongest peaks, which jump between its parts. on both units of the VoxRecord
and fourth VoxProbe recordings, read from the peaks of its harmonics every 25ms (within 10Hz of each
other), it is three formants: one steady at 207Hz, one rising from 720 to 1645Hz and one from about
120 to 2870Hz, faster, crossing the other near 300ms. the same at pitch 60 and 150 and at speeds 90
and 127, and at bend 15 all three 440Hz higher. the levels were fitted to the harmonics' peaks at
the default pitch: on the rest, left out (both units of the fourth VoxProbe recording at pitch 60
and 150, bend 15 and speeds 90 and 127), 30 to 93% and 7.4 to 1.3dB, and on the first VoxProbe
recording, which nothing here was fitted to, 36 to 91% and 7.3 to 1.1dB. at bend 0 the chip plays
something else: the second formant moves 197Hz down as it should, but the third is mirrored, falling
as 2.5kHz less its rise, and a line holds near 2.4kHz. both are a frequency Bend took under nothing
(see msa_expand()): the third formant starts at 120Hz, and the steady line is the first, which read
207Hz off the harmonics but goes under at bend 0 too, so it is put at 175Hz, the lowest that does:
every A1 item 91.0 to 94.5% and 1.63 to 1.33dB, bend 0 59 to 82% and 3.8 to 2.3dB

## `{2, 0, 0, -60, {0, 0, 0}, {0, -60, -60}},`

C7: traced again from unit B's VoxRecord recording every 2ms (8ms windows), the one tone it is,
which sweeps up from 500Hz to 2.7kHz and drops back five times in 300ms, faster each time; the 5 to
20ms points before had smoothed the sweeps into a wave under 1kHz, with three tones where the chip
has one. with its own peaking filters (see sound_eq) all 13 recordings of both units, at pitch 60
and 150 and bend 0 and 15 too, 1.33 to 1.04

## `/* FX-GATE-BEGIN */`

the effects the chip gates: their tones pulse on and off at a steady rate, which the chip's envelope
oscillator does. on the VoxRecord recording's third take, measured from each effect's envelope a
millisecond at a time: C0 and C4 pulse at the default speech pitch (85 and 87Hz), C2 at 66Hz, C3 at
59Hz, C6 at 125Hz, on for a quarter to two fifths of each cycle (B9 pulses at 87Hz too, but plays
closer to the recording ungated). C8 and C9 are gated noise, built in effect() (see c_noise()). the
envelope shape for each is the one that brought it closest to the recording. 0 is not gated. for
codes 200 to 254. C0, C2, C3, C4 and C6 have since been found to pulse with the voicing instead (see
fx_pulse), which takes the place of their gates here

## `static const struct`

the fourth VoxProbe recording plays the gated effects at pitch 60 and 150 and at bend 0 and 15, on
both units. their gates follow the Pitch control, a steady distance from the voicing frequency at
all three pitches: C0 4.7Hz under it, C2 20.5Hz under, C3 27.9Hz under, C4 on it and C6 42.7Hz over.
and each pulse has the same length whatever the rate: folded at the gate's period, the envelope
starts each pulse at full level and fades to nothing in a fixed time, as the voicing does (see
voice_fade): C4 as the voicing, C0 about 1.3 times as long, C3 and C6 about 1.6 times, and C2 as
long but in two humps (its tones beating, restarted together each pulse). at low pitch the rest of
each period is silent; at high pitch the next pulse cuts the fade short.  C2, C4 and C6 play that
way here: voiced at the voicing frequency plus their distance, along voice_fade stretched as
measured, with their noise (see fx_noise) refitted to it. fitted on both recordings at the default
pitch, at pitch 60 and at bend 0, and checked at pitch 150, at speeds 90 and 127 and at bend 15,
which were left out: C2 73 to 77% and 2.5 to 2.2dB on those, C4 65 to 71% and 4.0 to 3.6dB, C6 65 to
67% and 3.0 to 2.1dB. C0 and C3 came out worse played this way (on the 4kHz comparison of the time);
C3 pulses at a moving distance, and C0, measured full band, closer and with a shorter fade (below).
R8 and R9 pulse too. their low tone, near 650Hz, is restarted at half its frequency: on both units
of the fourth VoxProbe recording its lines are 306, 333 and 380Hz apart at pitch 60, 88 and 150, the
rate falls with the tone (R9 333 to 296Hz over its first 40ms as the tone falls 667 to 609Hz), and
the rate doesn't move with Bend while the tone does, which is what put the strongest line near 330Hz
at bend 0 and near 1kHz at bend 15 (the low tone read 126Hz lower there than on the VoxRecord
recording). their high tone (3.1 and 3.4kHz) is steady, so it moves to oscillator 4, and their noise
goes. here the restart rate is half the loudest tone of oscillators 1 to 3, moved with the Pitch
control as the others are; 10Hz either side of half fitted 3 to 5 points worse. the fade, 0.7 of the
voicing's, and the tones' levels were fitted at pitch 60, at bend 0 and on the VoxRecord recording:
on pitch 150, bend 15 and the speeds, left out, R8 61 to 77% and 4.9 to 2.8dB, R9 34 to 67% and 7.1
to 3.2dB. a rate rising 0.86Hz to each hertz of the voicing, which pitch 150 suggests, made R8
worse.  C3 pulses too, but its pulses slow down as it plays: on every C3 of both units of the fourth
VoxProbe recording and of the VoxRecord recording they come 15, 15, 16, 16, 17, 19, 19, 22 and 22ms
apart at the default pitch, 5 to 6ms long each at every pitch. at speed 90 the slowing is drawn out
and at speed 127 squeezed, so it follows the trace, not the clock, and at pitch 60 and 150 the gaps
(24 to 50ms and 8 to 12ms) are the voicing less a distance growing from about 20 to 42Hz along it,
as at the default pitch. so its distance falls by drift hertz for each millisecond of the trace.
fitted on every C3: 66.9 to 77.2% and 4.06 to 2.67dB, pitch 60 50 to 72%, speed 127 65 to 79%, its
old steady 59Hz gate at pitch 60 having pulsed nearly twice as fast as the chip.  C0 pulses as well.
its gate is the voicing less 4 to 6Hz at all three pitches (every 18.1, 12.3 and 7.0ms at pitch 60,
88 and 150, on both units), where it had a steady 85Hz gate. a steady distance on that gate made it
worse, its square pulses too long at low pitch; pulses fading at 0.66 of the voicing's fade, with
its noise (see fx_noise) 6dB quieter, fitted on every C0 of both recordings: 76.2 to 79.8% and 2.15
to 2.43dB, pitch 60 73 to 80%, bend 0 76 to 83%, bend 15 72 to 78%, pitch 150 82 to 79%

## `static const struct`

the noise under some of the traced effects. on both units of the VoxRecord recording C0, C1, C2, C4,
C6 and C7 have a broad noise under their tones that the traces, which take the strongest peaks as
tones, leave out (C1 is as noisy as it is tonal). here oscillators 4 and 5 add it, at a level
against the tones' and gated as deep as fitted, along with their frequencies and width, to both
units at once. C0 70 to 78% and 2.7 to 2.3dB, C1 42 to 67% and 5.7 to 1.7dB, C2 74 to 79% and 5.1 to
3.3dB, C4 68 to 75% and 4.8 to 4.3dB, C6 66 to 72% and 3.8 to 2.7dB, C7 58 to 65% and 6.3 to 1.9dB.
C3's made it match one unit worse, so it has none. C2's, C4's and C6's levels and depths were fitted
again for their pulses (see fx_pulse): 3dB quieter each, and 0.4, 1 and 0 deep.  the tone effects
(see fx_knots) have it too: on both units a pure tone sits on a skirt of noise and scattered lines
15 to 30dB under it, across the band, 15 to 30dB over the recording's background, where these had
clean sines with nothing. fitted the same way (depth 0: they aren't gated) and kept where neither
unit's spectrogram match fell more than half a point: M0 67 to 97% and 5.6 to 0.5dB, A0 66 to 86%
and 7.9 to 2.0dB, B6 6.1 to 0.7dB, R2 3.3 to 0.6dB, R9 5.5 to 1.8dB, A6 3.7 to 1.4dB. B7's made its
spectrogram worse, as did R3's, R7's, R8's, A5's, B3's and B4's. R9's went again when it was found
to pulse (see fx_pulse)

## `static void contour(sound *s, const fx_point *p, int n, int code)`

contour plays a traced effect: a quick start on the first point, then a glide to each of the others
in turn, then 3ms to silence. a tone doesn't move while it is silent: it fades out where it was and
fades in where it is going. the traces give the silent points whatever the background held (C3's
first and last are 227Hz), and gliding from those swept each tone in from far below, which at bend
15 put a strong sweep near 660Hz into C3 that neither unit of the fourth VoxProbe recording has
(10.6 to 10.1dB there)

## `static const double c8_db[] =`

C8 and C9 are noise from about 400Hz to 2.8kHz, flat within a few dB, the same on both units of the
VoxRecord recording and nearly the same as each other, gated at 52Hz 13 to 16dB deep. their traces
had taken the noise's strongest peaks for three tones, falling away above 1.5kHz. here they are
oscillators 4 and 5 spread wide, gated by a saw (see noise_depth), every 5ms at the level both units
have (the mean of the two, in dB against the loudest); the frequencies, width and depth are fitted
to both units at once, and the depth comes out at 0.83 (-15dB), as measured. C8 and C9 34% to 58%
and 8.4 to 2.9dB.  the fourth VoxProbe recording moves them with Bend (see msa_expand()), and at
bend 15 both units' noise has a sharp lower edge, nothing under 560Hz within 20dB, which a new
random frequency every sample, spreading the noise in long skirts around the oscillator, can't give.
holding each random value 0.75ms makes a band with edges instead; with the spread, the frequencies
and the hold fitted to both recordings at default and bend 0 and the pitches, and scored over four
noise sequences rather than one (the noise carries on from the sounds before), C8 and C9 go from 49
to 55% and 2.8 to 2.3dB there, 52 to 56% and 2.8 to 2.5dB on the VoxRecord recording, and at bend
15, left out of the fit, from 37 to 62% and 8.3 to 4.4dB.  the gate keeps time with Speed, not
Pitch: on both units of the fourth VoxProbe recording C8's gate comes every 19.1ms at the default
speed, 32.7 at speed 90 and 11.2 at speed 127 (1.71 and 0.59 times, where Speed stretches the sound
1.75 and 0.59 times), and every 18.9 to 19.2ms at pitch 60 and 150 and both bends. C8 at speed 90 62
to 68%, at speed 127 85 to 86%

## `static const short fx_knots[] =`

the sound effects that are pure tones, traced from both units' VoxRecord recordings: every
millisecond (12ms windows), each tone followed while it lasts, the two units' tracks paired and
averaged, the levels the recording's with the output filter's loss added back, each effect scaled so
that its loudest point is at the oscillators' limit. each tone has an oscillator of its own and is
kept as its knots (fx_knots): its time from the start of the sound in tenths of a millisecond, its
frequency in Hz and its volume in hundredths, between which the synth glides in straight lines. only
the knots a straight line can't pass close enough are kept, each effect at the loosest tolerance (1
to 12% and 0.5 to 6dB) that moves its score over every recording of both units by no more than 0.02.
before its first knot a tone sits silent at its first frequency, so tones start in place rather than
sweeping in. effects whose recordings are mostly not tones (buzzes and noise) keep fx_points

## `static const struct`

A4 is not a sine: on unit B of the VoxRecord recording its tone carries its third harmonic, exactly
three times its frequency all the way up, 10 to 14dB under it, and no fifth; the harmonic is gone as
it nears 4kHz, half the chip's rate. the noise under it before (see fx_noise) is not there on the
chip. with the harmonic on oscillator 2 at the level that fitted best and no noise, 1.33 to 1.00. no
other effect has a harmonic to speak of

## `typedef struct fxv_point`

the sound effects that are a buzz, voiced at the default speech pitch with formants that move,
played as the chip plays them, pulse by pulse: measured the same way as the allophones played from
the chip's frames (see vf_points), on both units. each point is a pulse: its time after the one
before (glided to in a straight line), the three formants and their volumes. A2 is played this way;
A1, A3 and M2 measured this way came out further from the recordings, and keep their traces (A1's
traced again: see fx_points)

## `typedef struct c1_point`

C1 is voiced: a harmonic series whose own pitch sweeps, with three formants moving through it. on
both units, in Man Goes Down (C1 at speed 62, where it is slow enough to see), every line is a whole
multiple of the pitch, and the pitch steps a whole voicing tick (see voice_hz()) at a time: 24 ticks
(333Hz) for the first 40ms, up to 14 (570Hz) by 80ms, then one tick slower every few milliseconds to
40 ticks (200Hz) at the end, the same on both units within 4Hz. the fourth VoxProbe recording plays
it at pitch 60, 88 and 150 and at bend 0 and 15, and the sweep is the same at all of them but pitch
150 (a tick or two faster there), so the Pitch control doesn't move it (see sound.own_pitch); Bend
moves the formants as usual. over the last 200ms one line rises from 2.0 to 2.9kHz (3.35kHz at bend
15) straight through the harmonics: oscillator 3 there runs free of the voicing (see
frame.free_osc). the trace this replaces took the loudest harmonic for a tone, jumping from one to
the next, under broad noise: at speed 62 it was noise and 4.3dB too loud against the speech.  the
points are every 1.9ms of the sweep at the default speed: the pitch as measured, and the formants
and volumes, as knots every 5 to 20ms, fitted to both units of Man Goes Down and of the fourth
VoxProbe recording at the default and bend 15, with C1's level against the game's first phrase, full
band (150Hz to 16kHz, see docs/design.md). fitted: Man Goes Down 0.60 to 0.85 and level +4.3 to
0.0dB, default 0.81 to 0.81, bend 15 0.78 to 0.80. left out: pitch 150 0.74 to 0.77, bend 0 0.73 to
0.78 (5.4 to 3.0dB), the VoxRecord recording 0.813 to 0.817, pitch 60 0.81 to 0.79

## `const frame bang = noise_frame(lGlottal, 500, 1400, 31, 26, 255);`

measured from the vox_test recording (see STEADY_LENGTH): 30ms of noise with most of its energy
under 2kHz, then noise around 1 to 2kHz that dies away by 30dB, fastest at the start and the end. on
both units of the VoxRecord recording the hiss holds within 3dB of the bang for 80ms, is 8dB down by
130ms, 13 by 200ms, 17 by 250ms and 27 by 290ms, and stops at 300ms: these fades, where it had been
faded sooner and stopped at 250ms

## `static const struct`

the noise refitted at the chip's own rate. the noise sounds were fitted while this library ran at
8192 samples a second; at CHIP_RATE the same values make different noise (it is a frequency jittered
every sample, so its spread, and where it folds back around half the rate, move with the rate), and
those sounds matched both units 1 to 15 points worse. so each sound with noise was fitted again at
CHIP_RATE, with changes to the noise oscillators (4 and 5) in all its parts: a scale on their
frequencies, a change to their width (distortion) and the balance of their levels, against both
units of the VoxRecord recording with the background taken out and clipped sounds left out. the
sound's overall level is held where it was (each sound is scored against its own loudest, so a fit
left free to change it made SE and others louder against the vowels, and the connected groups, which
nothing was fitted to, worse). each is kept only where it matches both units better, by at least
0.05dB or half a point, and doesn't match the second take (which nothing was fitted to) worse: TS 28
to 54%, CH 67 to 83%, TT 29 to 45%, KE 62 to 72%, C9 51 to 58% on unit B. HO, WH, FF, SO, SH, TH,
R5, R8, A7, A9, B0, B1, B2, B5, B8, C5, M0 and M1 (clipped on both units) keep theirs

## `static const struct`

the consonants shaped further than the oscillators can. on both units of the VoxRecord recording,
measured at full band, each consonant's spectrum differed from the chip's by much the same shape
every time: taken that shape away, what is left is no more than the two units differ from each other
(GO 5.4 to 0.7dB, ED 4.2 to 0.9, SE 5.2 to 1.3, against 1.5, 1.8 and 2.0 between the units). so each
is played through peaking filters half an octave apart (see synth_eq()), their gains fitted to that
recording, and the voiceless ones whose hiss is broader on the chip than oscillators 4 and 5 make
(SH's runs flat from 100Hz to 2kHz, 15dB above here) get broadband noise as well, at the level that
fitted best of 0, 0.15, 0.3 and 0.6 of theirs. an overall gain then puts each back at the level it
had, which was fitted among the vowels (louder, SE lost 3 to 5 points next to AW and in Man Goes
Down). the gains glide with the frames from one sound into the next.  on the fourth VoxProbe
recording, left out: GO 0.839 to 0.869 and 4.78 to 2.61dB, ED 4.30 to 1.87dB, OD 2.53 to 1.34dB, ZZ
1.95 to 1.11dB, BO 0.775 to 0.791 and 2.28 to 1.12dB, KO 0.850 to 0.870 (the mean of six noise
sequences: a hiss's match moves 2 to 4 points with the sequence alone), and BE EH and EH DE a point
closer. CH, SO, TT and OK fitted to nothing; EB, SH, TS and KE came out worse there, and SE, its
spectrum 5.4 to 2.0dB, lost 3 to 5 points of spectrogram match on every recording, so they have
none. the code, the filters' gains, the broadband noise and the overall gain

## `static const struct`

the chip starts most tones at once: on both units of the VoxRecord recording D1, D5, B3 and R3 come
up from the background to within 5dB in a millisecond, where their traces (5ms frames) ramp up over
5 to 10ms. so the effects below start sharply: the parts before the first whose summed volume
reaches the given fraction of the sound's loudest are silent, and that part is reached at once (its
glide 0), the sound's length unchanged. the fraction is each effect's own, fitted on every recording
of both units and kept only where none came out worse; the rest (the buzzes, the C series' gradual
starts, R1) keep their ramps. the VoxRecord effects 0.936 to 0.819

## `static const double pause_ms[] = {14, 107, 203, 645, 25, 43, 70};`

pause lengths in milliseconds for codes 0 to 6. the manual gives 0, 100, 200, 700, 30, 60 and 90.
the VoxProbe recording plays AW, a pause and AW three times for each pause, and these lengths give
the same silence between the AWs as the chip, within about 5ms (the AWs measured the same way on
both): 21, 109, 212, 661, 26, 47 and 73ms. pause 0 is not nothing: the chip still leaves a gap
between the sounds

## `static double msa_bend_hz(const msa *m)`

Bend doesn't multiply the frequencies but adds to them. the manual only says that bend goes from a
"deep-hollow" to a "High-metallic" sounding voice. measured from three takes of the VoxRecord
recording (see msa_speed_factor()): D1's two tones move together and keep their 512Hz spacing, by
-198, -154, 0, +220 and +439Hz at Bends 0, 1, 5, 10 and 15, and A5 by the same at 0 and 1; AW's
three formants all move by about 40Hz a step (F3 from 2480 to 2912Hz at Bend 15, where a multiplier
would put it near 4400). that is 44Hz a step from 5 up, and from 1 down; between 1 and 5 it is 22Hz
short, which these Bends can't place, so the shortfall is taken up at 5. it moves oscillators 1 to
3, the formants and tones; SE's hiss (oscillators 4 and 5) doesn't move at all. in the chip's
frequency steps (FREQ_STEP_HZ) a Bend is 10 steps, and the short step from 4 to 5 is 5

## `#define VOICE_TICK_HZ 7986.0`

the voicing frequency for a Pitch setting. the chip counts each voicing period in whole ticks of a
VOICE_TICK_HZ clock: 8000 / Pitch ticks (dividing in whole numbers) and one more. on the second
VoxProbe recording, which plays every Pitch from 0 to 254, the frequency holds flat over runs of
Pitch settings and steps exactly where that count changes (201 to 205 all give 199.7Hz), and at 12
settings the count is one more again, measured consistently on both halves of each test; no simple
arithmetic gives those, so they are listed.  not emulated: at very low pitch the chip also starts
each sound (and each copy of a repeated one) with a pulse, so pitch 0 gives a pulse a sound; but at
pitch 33 and 38 the pulses run on evenly across a repeat, and what decides it isn't known. here
pitch 0 is silent

## `#define SPEED_ZERO 146.0`

the duration multiplier from the Speed control. the manual gives the range and default but not the
curve. measured from a recording of a real AtariVox playing the VoxRecord ROM (TT AW EYIY SE at
speeds 0 to 127): the length falls in a straight line with speed, 2115ms at speed 0 and 955ms at 80,
and the line predicts speeds 20, 40 and 60 to within 2%. it reaches zero at about 146, so relative
to the default speed the multiplier is (146 - speed) / 32: 4.6 at speed 0, 0.59 at 127. pauses are
lengthened by it too (see execute())

## `if (m->fast)`

fast and slow, measured from the VoxProbe recording: fast plays OWIY and AXRR at 0.66 and 0.63 of
their length, and slow at 1.42 and 1.31 times. slow doesn't lengthen the voiceless consonants: the
vox_test recording has a slow SE 1.11 times and a slow TH 0.95 times its usual length, and in Man
Goes Down (both AtariVoxes) Slow SE Slow SE is 200ms, Slow TT's burst 20ms and Slow Slow TH 50ms,
where stretching them as the vowels are made them 290, 60 and 70ms. its vowels, DH and GO stretch as
here. it takes that game's second and fourth phrases from 0.75 to 0.86 and 0.63 to 0.80 (both units,
full band)

## `/* C8's and C9's gate keeps time with Speed (see c_noise()) */`

a tone Bend takes under nothing wraps round: on both units of the fourth VoxProbe recording, at bend
0 A1's third formant, which starts at 120Hz, isn't 197Hz lower but mirrored, a line falling from
2.4kHz as the formant rises, reaching nothing and rising again as the formant passes 2.5kHz, and its
first formant is a steady line near 2.4kHz. both are the tone WRAP_HZ higher, played at the chip's
rate (which folds it under half that rate), and it stays there as the effect goes on. a tone of an
effect quieter than WRAP_MIN_VOL is one its trace couldn't read, its frequency borrowed from a
neighbour (see contour()), and doesn't wrap: wrapping those put a line near 2.4kHz into C0, C3 and
C7 at bend 0 that neither unit has

## `if (!s->speech)`

Bend moves an effect's noise as well: on both units of the fourth VoxProbe recording C8's and C9's
noise moves about 200Hz down at bend 0 and 440Hz up at bend 15, as their tones do. it moves as a
block, its width unchanged: at bend 15 nothing is left under 560Hz within 20dB, where moving the
oscillators, whose spread is in proportion to their frequency, widened it and left the noise
reaching down to 300Hz. so the Bend is added after the spread (see synth_sample()): C8 and C9 at
bend 15 10.0 to 8.8 and 10.0 to 7.5dB, and on the first VoxProbe recording, left out, C8 at bend 0
3.2 to 3.0 and at bend 15 9.8 to 8.6dB. whether the hiss of speech moves too isn't measured, so it
doesn't here

## `#define MIN_CLOSURE_MS 20.0`

a pause before a stop takes the place of that much of its closure: on both AtariVoxes playing Man
Goes Down, UX, pause 1, EK leaves 140ms between the UX and EK's burst, where the pause (107ms) and
EK's closure (105ms) one after the other left 226ms, and TT, pause 1, BE starts BE's voicing as the
pause ends. the closure keeps MIN_CLOSURE_MS, which gives the 140ms. after a sound the closure is
all there (AW TT on the fourth VoxProbe recording). the two phrases go from 0.75 to 0.81 and 0.63 to
0.68 (both units, full band), and with the change to Slow (see msa_expand()) to 0.875

## `#define FRAMED_JOIN 0.7`

the sounds played from the chip's frames (see vf_points) were measured alone, and their first and
last frames are the voicing coming up from silence and dying away. between two sounds the chip
doesn't do that: in Man Goes Down, OWWW ZZ is voiced throughout on both AtariVoxes, where those
frames left a gap of 25 to 35ms. so after a voiced sound the frames before the first at FRAMED_JOIN
of the sound's loudest take its volumes, and before another sound the frames after the last at
FRAMED_JOIN take that one's. the timing is unchanged. "man goes down" 0.824 to 0.833 (both units,
full band); nothing on the VoxProbe or VoxRecord recordings joins these sounds to others, and none
of their scores moves
