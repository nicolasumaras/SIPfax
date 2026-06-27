# sipfax-slmodem-bridge — native (64-bit ok); only slmodemd must be 32-bit.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
BIN := sipfax-slmodem-bridge

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

$(BIN): bridge.c
	$(CC) $(CFLAGS) -o $@ bridge.c -lm

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

# Offline resampler quality test (round-trip SNR + frequency response).
# How the FIR cutoff/taps in bridge.c were chosen; run: make test && ./test_resamp
test: test_resamp
test_resamp: test_resamp.c
	$(CC) $(CFLAGS) -o $@ test_resamp.c -lm

clean:
	rm -f $(BIN) test_resamp

.PHONY: install clean test
