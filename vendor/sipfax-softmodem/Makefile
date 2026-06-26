CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=600
SPANDSP_CFLAGS := $(shell $(PKG_CONFIG) --cflags spandsp 2>/dev/null)
SPANDSP_LIBS := $(shell $(PKG_CONFIG) --libs spandsp 2>/dev/null)
LDLIBS += $(if $(SPANDSP_LIBS),$(SPANDSP_LIBS),-lspandsp) -lm

TARGET := sipfax-softmodem
SOURCES := sipfax-softmodem.c

.PHONY: all clean install bench-v21 bench-v22bis bench-hdlc-pty

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SPANDSP_CFLAGS) -o $@ $^ $(LDLIBS)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

bench-v21: $(TARGET)
	python3 bench-v21.py --worker ./$(TARGET)

bench-v22bis: $(TARGET)
	python3 bench-v22bis.py --worker ./$(TARGET)

bench-hdlc-pty: $(TARGET)
	python3 test/hdlc-pty-loopback.py ./$(TARGET)

clean:
	rm -f $(TARGET)
