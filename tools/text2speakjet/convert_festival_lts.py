#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""Convert Festival's CMU letter-to-sound trees for text2speakjet.

    python3 convert_festival_lts.py path/to/festival/lib/dicts/cmu/cmu_lts_rules.scm

Writes lts_cmu.json.gz beside this script. Only needed to rebuild that file;
text2speakjet reads the result, not Festival.

Festival's rules are one decision tree per letter, trained on CMUdict 0.4.
Each node asks whether the letter 1 to 3 places before or after is a given
letter ('#' is the word boundary, '0' beyond it); each leaf gives the phones
that letter makes (none, one, or several joined with '-'). In the converted
form a node is [offset, letter, yes, no] and a leaf is a string of ARPAbet
phones with stress digits, as in CMUdict. Festival's 'ax' becomes AH0, and the
probability distributions at the leaves, which aren't used to predict, are
dropped. See LICENSE.festival for the original's terms.
"""

import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "lts_cmu.json.gz")

OFFSETS = {"p.p.p.name": -3, "p.p.name": -2, "p.name": -1,
           "n.name": 1, "n.n.name": 2, "n.n.n.name": 3}


def parse(text):
    """A minimal S-expression reader: lists and atoms."""
    stack = [[]]
    for tok in re.findall(r"\(|\)|[^\s()]+", text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(tok)
    return stack[0]


def phones(prediction):
    if prediction == "_epsilon_":
        return ""
    out = []
    for p in prediction.split("-"):
        m = re.fullmatch(r"([a-z]+)([012]?)", p)
        name, stress = m.group(1).upper(), m.group(2)
        if name == "AX":
            name, stress = "AH", "0"
        out.append(name + stress)
    return " ".join(out)


def convert_tree(node):
    # a question node is ((feature is letter) yes no); a leaf is
    # (((phone p) ... prediction)): the distribution, then the prediction
    head = node[0]
    if isinstance(head, list) and len(head) == 3 and isinstance(head[0], str):
        feature, op, letter = head
        if op != "is" or feature not in OFFSETS:
            raise SystemExit("unexpected question %s" % head)
        return [OFFSETS[feature], letter, convert_tree(node[1]), convert_tree(node[2])]
    return phones(head[-1])


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    text = open(sys.argv[1], encoding="latin-1").read()
    text = text[text.index("(set! cmu_lts_rules"):]
    form = parse(text)[0]               # (set! cmu_lts_rules ' ( ... ))
    rules = next(x for x in form[2:] if isinstance(x, list))
    trees = {}
    for entry in rules:
        letter, tree = entry[0], entry[1]
        trees[letter] = convert_tree(tree)
    data = {
        "source": "Festival 2.5, lib/dicts/cmu/cmu_lts_rules.scm (CSTR, University of "
                  "Edinburgh; trained on CMUdict 0.4). Modified: converted to this "
                  "form, leaf probabilities removed, phones written as CMUdict "
                  "ARPAbet. See LICENSE.festival.",
        "trees": trees,
    }
    with gzip.open(OUT, "wt", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
    print("%s: %d letters" % (OUT, len(trees)))


if __name__ == "__main__":
    main()
