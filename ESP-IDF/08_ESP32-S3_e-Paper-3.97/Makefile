# Host tests for the parts of this firmware that are pure logic.
#
# Nothing here builds firmware -- that is idf.py. This exists so the decision
# core and the schedule math can be exercised on a Mac in milliseconds instead
# of on a board that has to be caught awake. If `make test` stops compiling,
# something pulled an ESP-IDF header into riddle_decide.c; that is the bug, not
# the Makefile.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O1 -g
BUILD   := build-host

.PHONY: test clean
test: $(BUILD)/run_tests
	@$(BUILD)/run_tests

$(BUILD)/run_tests: tests/run_tests.c main/page_riddle/riddle_decide.c \
                    main/page_riddle/riddle_decide.h | $(BUILD)
	$(CC) $(CFLAGS) -Imain/page_riddle -o $@ \
	    tests/run_tests.c main/page_riddle/riddle_decide.c

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
