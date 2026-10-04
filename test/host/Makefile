# Host unit tests for BirdBox's pure firmware logic.
#   make -C test/host test
# Needs only a C compiler (gcc or clang). The sanitizers turn a buffer
# overrun or undefined behaviour into a test failure, not a silent pass.

CC      ?= cc
MAIN    := ../../main
CFLAGS  ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -O1 \
           -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

SRC   := $(MAIN)/csv_field.c $(MAIN)/stats_core.c $(MAIN)/release_core.c \
         $(MAIN)/capture_name.c $(MAIN)/count_table.c
TESTS := test_main.c test_csv_field.c test_stats_core.c test_release_core.c \
         test_capture_name.c test_count_table.c

run_tests: $(SRC) $(TESTS) test.h $(wildcard $(MAIN)/*_core.h) $(MAIN)/csv_field.h \
           $(MAIN)/capture_name.h $(MAIN)/count_table.h
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ $(TESTS) $(SRC)

test: run_tests
	./run_tests

clean:
	rm -f run_tests

.PHONY: test clean
