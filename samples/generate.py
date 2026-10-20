#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""Regenerate the samples from phrases.txt.

    python3 samples/generate.py [path/to/sjwav]

text2speakjet turns each phrase into codes, written to codes.txt; sjwav then
renders each one to <name>.wav. `make samples` builds sjwav and runs this.

The WAVs are the reference the tests compare the library against, so only
regenerate them when a change to the sound is intended.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "tools", "text2speakjet", "text2speakjet.py")


def main():
    sjwav = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "sjwav")

    phrases = []
    with open(os.path.join(HERE, "phrases.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                name, text = line.split(":", 1)
                phrases.append((name.strip(), text.strip()))

    lines = ["# Generated from phrases.txt by generate.py -- do not edit.",
             "# name: codes, as text2speakjet --format dec --terminator none",
             ""]
    for name, text in phrases:
        codes = subprocess.run(
            [sys.executable, TOOL, "--format", "dec", "--terminator", "none", text],
            check=True, capture_output=True, text=True).stdout.strip()
        lines.append("# " + text)
        lines.append("%s: %s" % (name, codes))
        subprocess.run([sjwav, "-o", os.path.join(HERE, name + ".wav"), codes], check=True)

    with open(os.path.join(HERE, "codes.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
