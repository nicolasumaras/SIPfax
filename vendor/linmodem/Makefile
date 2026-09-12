# uncomment to use X11 debug interface
#USE_X11=y

CFLAGS= -O2 -Wall -g
CPPFLAGS += -I../spandsp-v42/src -DHAVE_STDBOOL_H -DHAVE_MALLOC_H -DHAVE_POSIX_MEMALIGN
LDFLAGS= -g
V42_OBJS= ../spandsp-v42/src/v42.o ../spandsp-v42/src/hdlc.o \
          ../spandsp-v42/src/crc.o ../spandsp-v42/src/bit_operations.o \
          ../spandsp-v42/src/logging.o ../spandsp-v42/src/alloc.o \
          ../spandsp-v42/src/async.o
OBJS= lm.o lmsim.o lmreal.o lmsoundcard.o serial.o atparser.o linpipe.o v90lineecho.o v90echodelay.o v34_phase2.o v34info1c.o v34sdetect.o \
      dsp.o fsk.o v8.o v8cj.o v21.o v23.o dtmf.o \
      v34.o v34fig9.o v34table.o v22.o v34eq.o \
      v90.o v90echo.o v90startup.o v90upstream.o v90lapmselect.o v90lapmlink.o v42detect.o v90odp.o v90trellis.o v90qam8.o v90equalizer.o v90shell.o v90mapping.o v90phase4.o v90pcm.o v90cp.o v90dil.o v90training.o v90train_tx.o v90table.o $(V42_OBJS)
INCLUDES= display.h   fsk.h       v21.h       v34priv.h   v34sdetect.h   v90priv.h \
          dsp.h       lm.h        v23.h       v8.h v8cj.h \
          dtmf.h      lmstates.h  v34.h       v90.h v90echo.h v90startup.h v90upstream.h v90lapmselect.h v90lapmlink.h v42detect.h v90odp.h v90trellis.h v90qam8.h v90equalizer.h v90shell.h v90mapping.h v90phase4.h v90pcm.h v90cp.h v90dil.h v90training.h v90train_tx.h
PROG= lm

ifdef USE_X11
OBJS += display.o
LDFLAGS += -L/usr/X11R6/lib -lX11
else
OBJS += nodisplay.o
endif

all: $(PROG) 

# Include compiler-discovered headers, including bundled SpanDSP internals.
DEPS = $(OBJS:.o=.d) v34gen.d v90gen.d
-include $(DEPS)

$(PROG): $(OBJS)
	gcc -o $(PROG) $(OBJS) -lm $(LDFLAGS)

v34gen: v34gen.o dsp.o
	gcc -o $@ v34gen.o dsp.o -lm $(LDFLAGS)

v34table.c: v34gen
	./v34gen > $@

v90gen: v90gen.o
	gcc -o $@ $< -lm $(LDFLAGS)

v90table.c: v90gen
	./v90gen > $@

linmodem.dvi: linmodem.tex
	latex2e linmodem.tex

clean:
	rm -f *.o $(V42_OBJS) $(DEPS) *~ *.dat core gmon.out *.sw v34table.c v90table.c $(PROG) v34gen v90gen *.aux *.dvi *.log

tar:
	( cd .. ; tar zcvf linmodem.tgz linmodem --exclude CVS )

%.o: %.c $(INCLUDES) Makefile
	gcc $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
