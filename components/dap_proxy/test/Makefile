# Host unit test for the B1 USBIP pipeline pure logic (components/dap_proxy).
# Usage: make -C components/dap_proxy/test test [SANITIZE=1]
ROOT  := $(abspath ../../../..)
CC    ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -fno-omit-frame-pointer

SANITIZE ?= 0
ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined
endif

BUILD  := build
TARGET := $(BUILD)/test_usbip_pipeline
SRCS   := test_usbip_pipeline.c ../usbip_pipeline_logic.c
HDRS   := ../usbip_pipeline_logic.h

.PHONY: all test clean
all: $(TARGET)
$(TARGET): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -I.. $(SRCS) -o $@
$(BUILD):
	mkdir -p $(BUILD)
test: $(TARGET)
	./$(TARGET)
clean:
	rm -rf $(BUILD)
