# Plain Makefile. No CMake, no downloads, no surprises.
# Read it — it is 30 lines and you should understand your own build.

CC      ?= gcc
CFLAGS  := -std=c11 -Ifirmware/include -Itests \
           -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wstrict-prototypes -Wcast-align -Wpointer-arith \
           -Wwrite-strings -Wswitch-enum -Wundef \
           -Werror -O1 -g

SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

SRC   := firmware/src/ringbuf.c
TESTS := tests/test_ringbuf.c
BUILD := build

.PHONY: all test test-asan clean help

help:
	@echo "make test       build and run the unit tests"
	@echo "make test-asan  same, with AddressSanitizer + UBSan"
	@echo "make clean      remove build artefacts"

all: test

$(BUILD):
	@mkdir -p $(BUILD)

test: | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(TESTS) -o $(BUILD)/test_ringbuf
	@echo
	@$(BUILD)/test_ringbuf

test-asan: | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(SRC) $(TESTS) -o $(BUILD)/test_ringbuf_asan
	@echo
	@$(BUILD)/test_ringbuf_asan

clean:
	rm -rf $(BUILD)
