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
test: $(BUILD)/run_tests $(BUILD)/link_check
	@$(BUILD)/link_check
	@$(BUILD)/run_tests

SRC := tests/run_tests.c main/page_riddle/riddle_decide.c main/page_riddle/wake_log.c main/page_riddle/kids.c
HDR := main/page_riddle/riddle_decide.h main/page_riddle/wake_log.h main/page_riddle/kids.h

$(BUILD)/run_tests: $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Imain/page_riddle -o $@ $(SRC)

# C++ linkage guard. The firmware's --gc-sections hides undefined references
# in any module app_main does not call yet, so a missing extern "C" in a pure
# unit's header survives a clean `idf.py build`. This catches it in a second.
CXX     ?= c++
PURE    := main/page_riddle/riddle_decide.c main/page_riddle/wake_log.c \
           main/page_riddle/kids.c

# The .c files must be compiled AS C (cc), then linked with the C++ object.
# Letting c++ compile them would make everything C++ and mask the very
# mismatch this target exists to find.
PURE_O  := $(BUILD)/riddle_decide.o $(BUILD)/wake_log.o $(BUILD)/kids.o

$(BUILD)/%.o: main/page_riddle/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Imain/page_riddle -c -o $@ $<

$(BUILD)/link_check: tests/link_check.cc $(PURE_O) $(HDR) | $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -Imain/page_riddle -o $@ \
	    tests/link_check.cc $(PURE_O)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
