# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make                 build/libfernft8.a and build/fern-ft8
#   make test            unit tests, golden decodes and command line tests
#   make test-asan       the unit tests under ASan and UBSan
#   make bench           CPU per busy and per quiet slot, single thread
#   make package [ARCH=x86_64|aarch64|armhf]
#                        dist/ft8-VERSION-linux-ARCH.fernmod, static, for
#                        FernSDR's Modules page or fernsdr --install-module
#   make test-cross CROSS_ARCH=aarch64|armhf
#                        the unit tests built with a cross compiler and run
#                        under qemu-user (qemu-aarch64 or qemu-arm)
#
# Nothing but the C++17 standard library is needed.

VERSION := 0.1.0
.DEFAULT_GOAL := all

OPT ?= -O2
CXXFLAGS_BASE := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -fcx-limited-range -ffp-contract=off \
	-DFERN_FT8_VERSION='"$(VERSION)"' -Isrc

LIB_SRCS := src/protocol.cpp src/ft4.cpp src/protocol_tables.cpp src/callsign_hash.cpp src/message.cpp src/fft.cpp \
	src/gfsk.cpp src/resampler.cpp src/ldpc.cpp src/wav.cpp src/decoder.cpp src/ft4_decoder.cpp src/slot_dsp.cpp src/channel.cpp src/simd.cpp \
	src/json.cpp
PROGRAM_SRCS := src/main.cpp src/module.cpp
TEST_SRCS := tests/test_main.cpp tests/test_protocol.cpp tests/test_message.cpp tests/test_callsign_hash.cpp \
	tests/test_dsp.cpp tests/test_decoder.cpp tests/test_module.cpp tests/test_ft4.cpp

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

.PHONY: all test test-asan test-cross bench clean print-version check-tables static package

all: $(BUILD)/libfernft8.a $(BUILD)/fern-ft8

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

test: $(TEST_DIR)/fern-ft8-tests $(BUILD)/fern-ft8 check-tables
	$(TEST_DIR)/fern-ft8-tests

test-asan: $(ASAN_DIR)/fern-ft8-tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 FERN_TEST_QUICK=1 \
		$(ASAN_DIR)/fern-ft8-tests

CROSS_ARCH ?= aarch64
CROSS_CXX_aarch64 := aarch64-linux-gnu-g++
CROSS_CXX_armhf := arm-linux-gnueabihf-g++
# NEON, as FernSDR's own armhf build: every ARMv7 board it runs on has it
# (the Raspberry Pi from the Pi 2 on), and without it the vector code falls
# back to plain loops.
CROSS_FLAGS_armhf := -march=armv7-a -mfpu=neon -mfloat-abi=hard
CROSS_QEMU_aarch64 := qemu-aarch64
CROSS_QEMU_armhf := qemu-arm
CROSS_TESTS := build/cross-$(CROSS_ARCH)/fern-ft8-tests

$(CROSS_TESTS): $(LIB_SRCS) $(TEST_SRCS) $(wildcard src/*.h tests/*.h)
	@mkdir -p $(@D)
	$(CROSS_CXX_$(CROSS_ARCH)) $(filter-out -g,$(CXXFLAGS_BASE)) $(CROSS_FLAGS_$(CROSS_ARCH)) -static \
		$(LIB_SRCS) $(TEST_SRCS) -o $@

test-cross: $(CROSS_TESTS)
	FERN_TEST_QUICK=1 $(CROSS_QEMU_$(CROSS_ARCH)) $(CROSS_TESTS)

# Packages, one per platform, as FernSDR installs them: one static
# executable and the manifest --describe gives, in a .fernmod.
HOST_ARCH := $(shell uname -m | sed -e 's/^armv7.*/armhf/' -e 's/^arm64$$/aarch64/')
ARCH ?= $(HOST_ARCH)
PACKAGE_CXX_x86_64 := $(if $(filter x86_64,$(HOST_ARCH)),$(CXX),x86_64-linux-gnu-g++)
PACKAGE_CXX_aarch64 := $(if $(filter aarch64,$(HOST_ARCH)),$(CXX),aarch64-linux-gnu-g++)
PACKAGE_CXX_armhf := $(if $(filter armhf,$(HOST_ARCH)),$(CXX),arm-linux-gnueabihf-g++)
STATIC_BIN := build/static-$(ARCH)/fern-ft8
PACKAGE := dist/ft8-$(VERSION)-linux-$(ARCH).fernmod

$(STATIC_BIN): $(LIB_SRCS) $(PROGRAM_SRCS) $(wildcard src/*.h)
	@mkdir -p $(@D)
	$(PACKAGE_CXX_$(ARCH)) $(filter-out -g,$(CXXFLAGS_BASE)) $(CROSS_FLAGS_$(ARCH)) -static -s \
		-ffunction-sections -fdata-sections -Wl,--gc-sections $(LIB_SRCS) $(PROGRAM_SRCS) -o $@

static: $(STATIC_BIN)

$(PACKAGE): $(STATIC_BIN) $(BUILD)/fern-ft8 tools/mkfernmod.py tools/check_fernmod.py
	@mkdir -p $(@D)
	python3 tools/mkfernmod.py --executable $(STATIC_BIN) --describe-with $(BUILD)/fern-ft8 \
		--platform linux-$(ARCH) --version $(VERSION) --output $@
	python3 tools/check_fernmod.py $@

package: $(PACKAGE)

bench: $(BUILD)/fern-ft8
	$(BUILD)/fern-ft8 bench

print-version:
	@echo $(VERSION)

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)
