# Host unit tests for the pure C logic (Wave 3 / WP-302).
#
# The only C unit compiled on the host is the ESP-IDF-free
# project_components/wifi_manager/wifi_cred_logic.c; see docs/host-test-report.md.
#
# Usage:
#   make -C tests/host test              # build + run the C test (TAP output)
#   make -C tests/host test SANITIZE=1   # same with ASan/UBSan
#   make -C tests/host clean

ROOT       := $(abspath ../..)
WIFI_DIR   := $(ROOT)/project_components/wifi_manager
BUILD      := build
CC         ?= cc
CFLAGS     ?= -std=c11 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer
SANITIZE   ?= 0

ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined
endif

TARGET := $(BUILD)/test_wifi_cred_logic
SRCS   := test_wifi_cred_logic.c $(WIFI_DIR)/wifi_cred_logic.c
HDRS   := $(WIFI_DIR)/wifi_credential.h

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -I$(WIFI_DIR) $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)
