# Makefile for the Mini Pops library (mp7).
#
# Targets:
#   all     build libpoppy.a and mp7_test
#   check   build and run the test harness
#   play    build the OpenAL rhythm player example
#   clean   remove the build products
#
# The archive is libpoppy.a (-lpoppy) while the API prefix is mp7_/MP7_.
# That mismatch is intentional; see README.md.

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
OPT     ?= -Os
HOT_OPT ?= -O2

CSTD     := -std=c99
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude -Isrc
CFLAGS   ?=

# Size first: keep the per-sample audio path (-O2 via HOT_SRCS below) fast and
# build the rest of the library for size. Function/data sections are
# deliberately not used: they shrink the linked application but inflate the
# archive itself, which is the artefact measured here.

# The library must not use floating point anywhere. Ask the compiler to prove
# it where the warnings are available.
FLOAT_WARN := $(shell $(CC) -Wfloat-conversion -Wdouble-promotion -Werror \
	-E -x c /dev/null >/dev/null 2>&1 && echo "-Wfloat-conversion -Wdouble-promotion")

# The library is plain C with no exceptions or backtraces. Drop the unwind
# tables unless the compiler does not know the flags.
NO_UNWIND := $(shell $(CC) -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Werror -E -x c /dev/null >/dev/null 2>&1 && \
	echo "-fno-asynchronous-unwind-tables -fno-unwind-tables")

# The extended build adds the 20 user pattern slots and the pattern source
# switch. It is on by default, so the shipped library carries them:
#
#   EXTRA_DEF := -DLIBPOPPY_EXTRA     the extended library, the default
#   EXTRA_DEF :=                      the minimal, preset-only library
#
# Empty the EXTRA_DEF line to build the minimal library: the 20 factory
# rhythms, the shared percussion layer, no user pattern slots and no extra
# entry points. `make check` always builds and tests both configurations,
# whichever one is the shipped one, so neither can rot.

EXTRA_DEF := -DLIBPOPPY_EXTRA

# The other configuration, built next to the shipped one for the test run.
ALT_DEF := $(if $(EXTRA_DEF),,-DLIBPOPPY_EXTRA)

BASE_CFLAGS       := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP
BASE_TEST_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP

LIB_CFLAGS       := $(BASE_CFLAGS) $(EXTRA_DEF)
TEST_CFLAGS      := $(BASE_TEST_CFLAGS) $(EXTRA_DEF)
ALT_LIB_CFLAGS   := $(BASE_CFLAGS) $(ALT_DEF)
ALT_TEST_CFLAGS  := $(BASE_TEST_CFLAGS) $(ALT_DEF)

BUILD := build

LIB_SRCS  := $(wildcard src/*.c)
TEST_SRCS := $(wildcard test/*.c)
LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst test/%.c,$(BUILD)/test/%.o,$(TEST_SRCS))

# The other configuration, always built so that `make check` can verify it
# even when the shipped library is the other one.
ALT_LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/alt/src/%.o,$(LIB_SRCS))
ALT_TEST_OBJS := $(patsubst test/%.c,$(BUILD)/alt/test/%.o,$(TEST_SRCS))

# Per-sample audio path: build these at -O2 regardless of the size-first
# default. If profiling shows a regression elsewhere (mp7_voice.c and
# mp7_pattern.c also run per sample), add it here.
HOT_SRCS := src/mp7_engines.c src/mp7_mixer.c
HOT_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(HOT_SRCS))

$(HOT_OBJS): EXTRA_OPT := $(HOT_OPT)

ALT_HOT_OBJS := $(patsubst src/%.c,$(BUILD)/alt/src/%.o,$(HOT_SRCS))

$(ALT_HOT_OBJS): EXTRA_OPT := $(HOT_OPT)

# examples/dinghy_app.c is built inside a Dinghy checkout, not here.
EXAMPLE_SKIP := examples/play_rhythm.c examples/dinghy_app.c
EXAMPLES := $(filter-out $(EXAMPLE_SKIP),$(wildcard examples/*.c))
EXAMPLE_OBJS := $(patsubst examples/%.c,$(BUILD)/examples/%.o,$(EXAMPLES))

OPENAL_CFLAGS := $(shell pkg-config --cflags openal 2>/dev/null)
OPENAL_LIBS   := $(shell pkg-config --libs openal 2>/dev/null || echo -lopenal)

LIB  := libpoppy.a
TEST := mp7_test
PLAY := $(BUILD)/examples/play_rhythm

# The other configuration, built next to the shipped one.
ALT_LIB  := $(BUILD)/libpoppy_alt.a
ALT_TEST := $(BUILD)/mp7_test_alt

.PHONY: all check check-alt clean play

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) -L. -lpoppy -lm

$(ALT_LIB): $(ALT_LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(ALT_TEST): $(ALT_TEST_OBJS) $(ALT_LIB)
	$(CC) $(ALT_TEST_CFLAGS) -o $@ $(ALT_TEST_OBJS) -L$(BUILD) -lpoppy_alt -lm

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(EXTRA_OPT) -c -o $@ $<

$(BUILD)/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILD)/alt/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALT_LIB_CFLAGS) $(EXTRA_OPT) -c -o $@ $<

$(BUILD)/alt/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALT_TEST_CFLAGS) -c -o $@ $<

$(BUILD)/examples/%.o: examples/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

play: $(PLAY)

$(PLAY): $(BUILD)/examples/play_rhythm.o $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $< -L. -lpoppy $(OPENAL_LIBS)

$(BUILD)/examples/play_rhythm.o: examples/play_rhythm.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(OPENAL_CFLAGS) -c -o $@ $<

check: $(TEST) $(EXAMPLE_OBJS) $(ALT_TEST)
	./$(TEST) --init
	./$(TEST) --sine
	./$(TEST) --gain
	./$(TEST) --silence $(BUILD)/silence.wav
	./$(TEST) --instruments $(BUILD)
	./$(TEST) --noise
	./$(TEST) --guiro
	./$(TEST) --quijada
	./$(TEST) --mixer
	./$(TEST) --api
	./$(TEST) --panel
	./$(TEST) --pattern
	./$(TEST) --rhythms $(BUILD)
	./$(TEST) --user
	$(MAKE) --no-print-directory check-alt

# The other configuration runs the same checks as the shipped one. It is a
# separate build so that both configurations stay verified whichever one the
# Makefile ships.
check-alt: $(ALT_TEST) $(EXAMPLE_OBJS)
	./$(ALT_TEST) --init
	./$(ALT_TEST) --sine
	./$(ALT_TEST) --gain
	./$(ALT_TEST) --silence $(BUILD)/silence.wav
	./$(ALT_TEST) --instruments $(BUILD)
	./$(ALT_TEST) --noise
	./$(ALT_TEST) --guiro
	./$(ALT_TEST) --quijada
	./$(ALT_TEST) --mixer
	./$(ALT_TEST) --api
	./$(ALT_TEST) --panel
	./$(ALT_TEST) --pattern
	./$(ALT_TEST) --rhythms $(BUILD)
	./$(ALT_TEST) --user

clean:
	rm -rf $(BUILD) $(LIB) $(TEST) silence.wav

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(EXAMPLE_OBJS:.o=.d) \
	$(ALT_LIB_OBJS:.o=.d) $(ALT_TEST_OBJS:.o=.d) \
	$(BUILD)/examples/play_rhythm.d
