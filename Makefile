# bytebeat -- terminal live-coding noise instrument
#
# Dependencies: libasound2-dev libncurses-dev
#   sudo apt install libasound2-dev libncurses-dev

CC      ?= gcc
CFLAGS  ?= -O2 -g

# -std=c11, not c99, and that is a forced choice rather than a preference:
#   <stdatomic.h> does not exist in C99. It was added in C11. Since the
#   lock-free program swap is built on atomics, C11 is the floor. The source
#   itself is otherwise plain C99 -- no _Generic, no anonymous unions, no
#   C11-only library calls -- so dropping to -std=c99 -Wno-pedantic still
#   compiles under gcc, which accepts _Atomic as an extension. If you ever
#   port this, the only C11 dependency is stdatomic.h.
CFLAGS  += -std=c11 -Wall -Wextra -pedantic

# -fwrapv: bytebeat overflows signed integers on essentially every sample and
#   that overflow IS the instrument. The VM already routes arithmetic through
#   uint32_t so the behaviour is defined; this is a second line of defence and
#   it stops the optimiser from reasoning "signed overflow cannot happen".
# -D_GNU_SOURCE: clock_nanosleep, pthread scheduling attributes, MSG_NOSIGNAL.
CFLAGS  += -fwrapv -D_GNU_SOURCE

LDLIBS   = -lasound -lncurses -lpthread -lm

OBJS = main.o expr.o engine.o audio.o dsp.o ui.o sink.o gen.o knob.o rack.o
BIN  = bytebeat

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

main.o:  main.c  bytebeat.h expr.h audio.h engine.h sink.h ui.h examples.h rack.h gen.h
expr.o:  expr.c  expr.h
engine.o: engine.c bytebeat.h expr.h engine.h dsp.h rack.h gen.h
audio.o: audio.c bytebeat.h expr.h audio.h dsp.h engine.h
dsp.o:   dsp.c   dsp.h
ui.o:    ui.c    bytebeat.h expr.h ui.h audio.h sink.h examples.h knob.h rack.h gen.h
sink.o:  sink.c  bytebeat.h expr.h sink.h
gen.o:   gen.c   gen.h rack.h knob.h bytebeat.h expr.h dsp.h
knob.o:  knob.c  knob.h expr.h
rack.o:  rack.c  rack.h knob.h bytebeat.h expr.h

clean:
	rm -f $(OBJS) $(BIN)

test: $(BIN)
	./$(BIN) --self-test

# Grant the binary the ability to get SCHED_FIFO and to mlockall without
# being root. Optional -- the program degrades gracefully without it and
# tells you so on screen.
caps: $(BIN)
	sudo setcap cap_sys_nice,cap_ipc_lock=eip ./$(BIN)

.PHONY: all clean test caps
