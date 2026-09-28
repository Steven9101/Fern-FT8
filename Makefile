# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make                 build/libfernft8.a and build/fern-ft8
#   make test            unit tests, golden decodes and command line tests
#   make test-asan       the unit tests under ASan and UBSan
#   make bench           CPU per busy and per quiet slot, single thread
#
# Nothing but the C++17 standard library is needed.

VERSION := 0.1.0
.DEFAULT_GOAL := all

OPT ?= -O2
CXXFLAGS_BASE := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	-DFERN_FT8_VERSION='"$(VERSION)"' -Isrc

LIB_SRCS := src/protocol.cpp src/protocol_tables.cpp src/callsign_hash.cpp src/message.cpp
PROGRAM_SRCS := src/main.cpp
TEST_SRCS := tests/test_main.cpp tests/test_protocol.cpp tests/test_message.cpp tests/test_callsign_hash.cpp

BUILD := build
TEST_DIR := build/test
ASAN_DIR := build/asan
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

# $(1) directory, $(2) extra flags
define compile_rules
$(1)/obj/src/%.o: src/%.cpp
	@mkdir -p $$(@D)
	$(CXX) $$(CXXFLAGS_BASE) $(2) -MMD -MP -c $$< -o $$@
$(1)/obj/tests/%.o: tests/%.cpp
	@mkdir -p $$(@D)
	$(CXX) $$(CXXFLAGS_BASE) $(2) -MMD -MP -c $$< -o $$@
endef

$(eval $(call compile_rules,$(BUILD),))
$(eval $(call compile_rules,$(ASAN_DIR),$(SANITIZE)))

objs = $(patsubst %.cpp,$(1)/obj/%.o,$(2))

LIB_OBJS := $(call objs,$(BUILD),$(LIB_SRCS))
PROGRAM_OBJS := $(call objs,$(BUILD),$(PROGRAM_SRCS))
TEST_OBJS := $(call objs,$(BUILD),$(TEST_SRCS))
ASAN_OBJS := $(call objs,$(ASAN_DIR),$(LIB_SRCS) $(TEST_SRCS))

.PHONY: all test test-asan bench clean print-version check-tables

all: $(BUILD)/libfernft8.a

$(BUILD)/libfernft8.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/fern-ft8: $(PROGRAM_OBJS) $(BUILD)/libfernft8.a
	$(CXX) -pthread -o $@ $^

$(TEST_DIR)/fern-ft8-tests: $(TEST_OBJS) $(BUILD)/libfernft8.a
	@mkdir -p $(@D)
	$(CXX) -pthread -o $@ $^

$(ASAN_DIR)/fern-ft8-tests: $(ASAN_OBJS)
	$(CXX) $(SANITIZE) -pthread -o $@ $^

check-tables:
	python3 tools/gen_tables.py --check

test: $(TEST_DIR)/fern-ft8-tests check-tables
	$(TEST_DIR)/fern-ft8-tests

test-asan: $(ASAN_DIR)/fern-ft8-tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 FERN_TEST_QUICK=1 \
		$(ASAN_DIR)/fern-ft8-tests

bench: $(BUILD)/fern-ft8
	$(BUILD)/fern-ft8 bench

print-version:
	@echo $(VERSION)

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)
