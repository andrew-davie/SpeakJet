CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
PYTHON ?= python3

all: libspeakjet.a sjwav

libspeakjet.a: speakjet.o
	$(AR) rcs $@ $^

speakjet.o: speakjet.c speakjet.h
	$(CC) $(CFLAGS) -c speakjet.c -o $@

# render SpeakJet codes to a WAV file
sjwav: tools/sjwav.c tools/render.c tools/render.h libspeakjet.a
	$(CC) $(CFLAGS) tools/sjwav.c tools/render.c libspeakjet.a -lm -o $@

tests/speakjet_test: tests/speakjet_test.c speakjet.c speakjet.h
	$(CC) $(CFLAGS) tests/speakjet_test.c -lm -o $@

tests/reference_test: tests/reference_test.c tools/render.c tools/render.h libspeakjet.a
	$(CC) $(CFLAGS) tests/reference_test.c tools/render.c libspeakjet.a -lm -o $@

# the library's unit tests, the reference samples, and text2speakjet's tests
test: tests/speakjet_test tests/reference_test
	./tests/speakjet_test
	./tests/reference_test samples
	$(PYTHON) tools/text2speakjet/test_text2speakjet.py

# regenerate samples/codes.txt and the WAVs from samples/phrases.txt. the WAVs
# are the test reference, so only do this when a change to the sound is meant
samples: sjwav
	$(PYTHON) samples/generate.py ./sjwav

clean:
	rm -f speakjet.o libspeakjet.a sjwav tests/speakjet_test tests/reference_test

.PHONY: all test samples clean
