# Makefile for the Mini Pops library (mp7).
#
# Targets:
#   all     build libpoppy.a and mp7_test
#   check   build and run the test harness
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

LIB_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(INCLUDES) $(CFLAGS) -MMD -MP
TEST_CFLAGS := $(CSTD) $(WARN) $(OPT) $(INCLUDES) $(CFLAGS) -MMD -MP

BUILD := build

LIB_SRCS  := $(wildcard src/*.c)
TEST_SRCS := $(wildcard test/*.c)
LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst test/%.c,$(BUILD)/test/%.o,$(TEST_SRCS))

LIB  := libpoppy.a
TEST := mp7_test

.PHONY: all check clean

all: $(LIB) $(TEST)

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

check: $(TEST)
	./$(TEST) --init
	./$(TEST) --sine
	./$(TEST) --gain
	./$(TEST) --silence $(BUILD)/silence.wav
	./$(TEST) --instruments $(BUILD)
	./$(TEST) --noise
	./$(TEST) --guiro
	./$(TEST) --quijada
	./$(TEST) --pattern
	./$(TEST) --rhythms $(BUILD)

clean:
	rm -rf $(BUILD) $(LIB) $(TEST) silence.wav

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
