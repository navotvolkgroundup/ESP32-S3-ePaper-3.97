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

.PHONY: test clean icons buttons
test: $(BUILD)/run_tests $(BUILD)/link_check icons buttons
	@$(BUILD)/link_check
	@$(BUILD)/run_tests

# Cross-artifact check: every icon stem wmo_icon() can return must exist as a
# bitmap. A unit test cannot catch this -- it asserts the STRING is "leiyu",
# not that leiyu.bmp ships -- and the failure is a silently blank zone on the
# panel. Cheap enough to run on every `make test`.
icons:
	@missing=0; \
	for n in $$(sed -n '/^const char \*wmo_icon/,/^}/p' main/page_riddle/weather.c \
	            | grep -oE 'return "[a-z]+";' | sed 's/return "//;s/";//' | sort -u); do \
	  if [ ! -f "main/page_weather/Weather_img/$$n.bmp" ]; then \
	    echo "MISSING ICON: $$n.bmp referenced by wmo_icon()"; missing=1; \
	  fi; \
	done; \
	[ $$missing -eq 0 ] && echo "icon check OK: every wmo_icon() name has a bitmap"

# Cross-artifact check: the button classifier against the BSP that feeds it.
# Catches dead arms (a code nothing emits) and ambiguous ones (a code several
# events share). Both shipped; see the script header.
buttons:
	@python3 tests/check_buttons.py

# cJSON comes from the IDF checkout but is a vendored library, not IDF itself,
# so weather.c stays host-compilable and its PARSING is testable, not just the
# arithmetic around it.
CJSON_DIR ?= $(HOME)/esp/esp-idf/components/json/cJSON

SRC := tests/run_tests.c main/page_riddle/riddle_decide.c main/page_riddle/wake_log.c \
       main/page_riddle/kids.c main/page_riddle/weather.c \
       main/page_riddle/schedule.c main/page_riddle/sd_json.c \
       main/page_riddle/daily_layout.c $(CJSON_DIR)/cJSON.c
HDR := main/page_riddle/riddle_decide.h main/page_riddle/wake_log.h \
       main/page_riddle/kids.h main/page_riddle/weather.h \
       main/page_riddle/schedule.h main/page_riddle/sd_json.h \
       main/page_riddle/daily_layout.h

$(BUILD)/run_tests: $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Imain/page_riddle -I$(CJSON_DIR) -o $@ $(SRC) -lm

# C++ linkage guard. The firmware's --gc-sections hides undefined references
# in any module app_main does not call yet, so a missing extern "C" in a pure
# unit's header survives a clean `idf.py build`. This catches it in a second.
CXX     ?= c++
PURE    := main/page_riddle/riddle_decide.c main/page_riddle/wake_log.c \
           main/page_riddle/kids.c main/page_riddle/weather.c \
           main/page_riddle/schedule.c main/page_riddle/sd_json.c \
           main/page_riddle/daily_layout.c

# The .c files must be compiled AS C (cc), then linked with the C++ object.
# Letting c++ compile them would make everything C++ and mask the very
# mismatch this target exists to find.
PURE_O  := $(BUILD)/riddle_decide.o $(BUILD)/wake_log.o $(BUILD)/kids.o \
           $(BUILD)/weather.o $(BUILD)/schedule.o $(BUILD)/sd_json.o \
           $(BUILD)/daily_layout.o $(BUILD)/cJSON.o

$(BUILD)/%.o: main/page_riddle/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Imain/page_riddle -I$(CJSON_DIR) -c -o $@ $<

$(BUILD)/cJSON.o: $(CJSON_DIR)/cJSON.c | $(BUILD)
	$(CC) -std=c99 -O1 -I$(CJSON_DIR) -c -o $@ $<

$(BUILD)/link_check: tests/link_check.cc $(PURE_O) $(HDR) | $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O1 -Imain/page_riddle -I$(CJSON_DIR) \
	    -o $@ tests/link_check.cc $(PURE_O) -lm

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
