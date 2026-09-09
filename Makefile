# Plain Makefile. No CMake, no downloads, no surprises.
#
#   make test          build and run every C unit test
#   make test-asan     the same under AddressSanitizer + UBSan
#   make station       build the host (POSIX) station binary
#   make fuzz          libFuzzer run (needs clang); make fuzz-corpus replays the
#                      seed corpus with any compiler
#   make pytest        Python unit tests for the server and tools
#   make integration   end-to-end run through the network simulator
#   make check         everything above that needs no clang
#   make lint          cppcheck over the firmware
#   make clean

CC      ?= gcc
CFLAGS  := -std=c11 -Ifirmware/include -Itests \
           -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wstrict-prototypes -Wcast-align -Wpointer-arith \
           -Wwrite-strings -Wswitch-enum -Wundef \
           -Werror -O1 -g
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD := build

# Portable firmware core: no OS calls, no sockets, no clock. Everything here
# compiles unchanged for the host, for Zephyr, and for a bare-metal target.
CORE_SRC := $(wildcard firmware/src/*.c)

# One test binary per tests/test_*.c file, each linked against the core and,
# where it needs one, the mock HAL.
UNIT_SRC  := $(wildcard tests/test_*.c)
MOCK_HAL  := $(wildcard tests/mock/*.c)
UNIT_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(UNIT_SRC))
ASAN_BINS := $(patsubst tests/%.c,$(BUILD)/asan/%,$(UNIT_SRC))

# Host port of the station: the POSIX HAL plus a command-line front end.
STATION_SRC := $(wildcard firmware/hal/posix/*.c) $(wildcard firmware/app/*.c)

.PHONY: all test test-asan station fuzz fuzz-corpus pytest integration \
        check lint format clean help

all: test station

help:
	@sed -n '2,15p' Makefile

$(BUILD) $(BUILD)/asan $(BUILD)/fuzz:
	@mkdir -p $@

$(BUILD)/%: tests/%.c $(CORE_SRC) $(MOCK_HAL) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(CORE_SRC) $(MOCK_HAL) $< -o $@

$(BUILD)/asan/%: tests/%.c $(CORE_SRC) $(MOCK_HAL) tests/test.h | $(BUILD)/asan
	$(CC) $(CFLAGS) $(SANFLAGS) $(CORE_SRC) $(MOCK_HAL) $< -o $@

test: $(UNIT_BINS)
	@echo
	@for t in $(UNIT_BINS); do $$t || exit 1; echo; done

test-asan: $(ASAN_BINS)
	@echo
	@for t in $(ASAN_BINS); do $$t || exit 1; echo; done

station: $(BUILD)/station

$(BUILD)/station: $(CORE_SRC) $(STATION_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L \
	      $(CORE_SRC) $(STATION_SRC) -o $@

# libFuzzer: clang only. Runs each harness for a bounded time against the
# committed seed corpus. Crashes are written to the corpus directory.
FUZZ_SRC  := $(wildcard tests/fuzz/fuzz_*.c)
FUZZ_BINS := $(patsubst tests/fuzz/%.c,$(BUILD)/fuzz/%,$(FUZZ_SRC))
FUZZ_TIME ?= 30

$(BUILD)/fuzz/%: tests/fuzz/%.c $(CORE_SRC) | $(BUILD)/fuzz
	clang $(CFLAGS) -fsanitize=fuzzer,address,undefined $(CORE_SRC) $< -o $@

fuzz: $(FUZZ_BINS)
	@for f in $(FUZZ_BINS); do \
	    name=$$(basename $$f); \
	    $$f -max_total_time=$(FUZZ_TIME) -max_len=512 tests/fuzz/corpus/$$name || exit 1; \
	done

# Replay the seed corpus through the same harnesses with a plain driver, so
# the corpus is exercised even where clang is not installed.
$(BUILD)/fuzz/corpus_%: tests/fuzz/fuzz_%.c tests/fuzz/driver.c $(CORE_SRC) | $(BUILD)/fuzz
	$(CC) $(CFLAGS) $(CORE_SRC) tests/fuzz/driver.c $< -o $@

fuzz-corpus: $(patsubst tests/fuzz/fuzz_%.c,$(BUILD)/fuzz/corpus_%,$(FUZZ_SRC))
	@for f in $^; do \
	    name=fuzz_$${f#$(BUILD)/fuzz/corpus_}; \
	    $$f tests/fuzz/corpus/$$name || exit 1; \
	done

pytest:
	PYTHONPATH=server:tools python3 -m unittest discover -s tests/python -p 'test_*.py' -v

integration: station
	PYTHONPATH=server:tools python3 -m unittest discover -s tests/integration -p 'test_*.py' -v

check: test test-asan fuzz-corpus pytest integration

lint:
	cppcheck --enable=warning,style,performance,portability --error-exitcode=1 \
	         --inline-suppr --std=c11 -I firmware/include -I firmware/hal \
	         firmware/src firmware/hal firmware/app

format:
	clang-format -i firmware/include/*.h firmware/include/hal/*.h \
	             firmware/src/*.c firmware/hal/posix/*.c firmware/app/*.c \
	             tests/*.c tests/*.h tests/mock/*.c tests/fuzz/*.c

clean:
	rm -rf $(BUILD)
