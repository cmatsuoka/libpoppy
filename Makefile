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
OPT     ?= -O2

CSTD     := -std=c99
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude -Isrc
CFLAGS   ?=

# The library must not use floating point anywhere. Ask the compiler to prove
# it where the warnings are available.
FLOAT_WARN := $(shell $(CC) -Wfloat-conversion -Wdouble-promotion -Werror \
	-E -x c /dev/null >/dev/null 2>&1 && echo "-Wfloat-conversion -Wdouble-promotion")

# The library is plain C with no exceptions or backtraces. Drop the unwind
# tables unless the compiler does not know the flags.
NO_UNWIND := $(shell $(CC) -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Werror -E -x c /dev/null >/dev/null 2>&1 && \
	echo "-fno-asynchronous-unwind-tables -fno-unwind-tables")

LIB_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP
TEST_CFLAGS := $(CSTD) $(WARN) $(OPT) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP

BUILD := build

LIB_SRCS  := $(wildcard src/*.c)
TEST_SRCS := $(wildcard test/*.c)
LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst test/%.c,$(BUILD)/test/%.o,$(TEST_SRCS))
# examples/dinghy_app.c is built inside a Dinghy checkout, not here.
EXAMPLE_SKIP := examples/play_rhythm.c examples/dinghy_app.c
EXAMPLES := $(filter-out $(EXAMPLE_SKIP),$(wildcard examples/*.c))
EXAMPLE_OBJS := $(patsubst examples/%.c,$(BUILD)/examples/%.o,$(EXAMPLES))

OPENAL_CFLAGS := $(shell pkg-config --cflags openal 2>/dev/null)
OPENAL_LIBS   := $(shell pkg-config --libs openal 2>/dev/null || echo -lopenal)

LIB  := libpoppy.a
TEST := mp7_test
PLAY := $(BUILD)/examples/play_rhythm

.PHONY: all check clean play

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) -L. -lpoppy -lm

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILD)/examples/%.o: examples/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

play: $(PLAY)

$(PLAY): $(BUILD)/examples/play_rhythm.o $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $< -L. -lpoppy $(OPENAL_LIBS)

$(BUILD)/examples/play_rhythm.o: examples/play_rhythm.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(OPENAL_CFLAGS) -c -o $@ $<

check: $(TEST) $(EXAMPLE_OBJS)
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

clean:
	rm -rf $(BUILD) $(LIB) $(TEST) silence.wav

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(EXAMPLE_OBJS:.o=.d) \
	$(BUILD)/examples/play_rhythm.d
