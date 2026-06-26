CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O2 -g
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
SPANDSP_CFLAGS := $(shell $(PKG_CONFIG) --cflags spandsp 2>/dev/null)
SPANDSP_LIBS := $(shell $(PKG_CONFIG) --libs spandsp 2>/dev/null)
LDLIBS += $(if $(SPANDSP_LIBS),$(SPANDSP_LIBS),-lspandsp) -lm

TARGET := sipfax-softmodem
SOURCES := sipfax-softmodem.c

.PHONY: all clean install bench-v21

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SPANDSP_CFLAGS) -o $@ $^ $(LDLIBS)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

bench-v21: $(TARGET)
	python3 bench-v21.py --worker ./$(TARGET)

clean:
	rm -f $(TARGET)
