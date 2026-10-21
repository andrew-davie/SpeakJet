# Changelog

## Unreleased

Measurements of the real chip, where two independent sources agree, replace
some guesses. The sound changes, so the reference samples are regenerated.

- Formants: 23 formant frequencies in 9 allophones (OW, AX, RR, UX, IH, MM,
  EY, EH, AW) are the mean of the two measurements, where they are within 10%
  or 88 Hz of each other. The biggest corrections are RR (350/1100/1450 to
  439/1318/1720 Hz) and IH's F2 (1990 to 1774 Hz). Volumes are unchanged.
- Speed: duration is exp(-0.02 (Speed - 114)) instead of 114 / Speed.
- Output filter: a Butterworth low pass at 2200 Hz instead of 3400 Hz.

`docs/design.md` describes the sources and the method.

## 1.0.0

First release.

- The speakjet library: the SpeakJet's input buffer, MSA sequencer and
  synthesizer, with the serial byte-stream framing, status lines and save
  state support an emulator needs.
- sjwav, which renders SpeakJet codes to a WAV file.
- text2speakjet, which turns English text into SpeakJet codes.
- Sample phrases, which are also the reference for the tests.
