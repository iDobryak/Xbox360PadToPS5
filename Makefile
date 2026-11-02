# PS5 payload build. Host-side protocol tests use CMake.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

# Homebrew versioned LLVM is keg-only on macOS, so prospero-llvm-config does
# not see it on PATH. Prefer an explicit caller value, then common keg paths.
LLVM_CONFIG ?= $(firstword $(wildcard \
    /opt/homebrew/opt/llvm/bin/llvm-config \
    /opt/homebrew/opt/llvm@18/bin/llvm-config \
    /usr/local/opt/llvm/bin/llvm-config \
    /usr/local/opt/llvm@18/bin/llvm-config))
export LLVM_CONFIG

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined; point it at ps5-payload-sdk)
endif

ELF := ps5-xb360-receiver.elf
PS5_BUILD := build-ps5
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -Iinclude
LDLIBS += -lScePad -lSceUserService -ldl
SRCS := src/ps5_main.c src/ps5_receiver.c src/ps5_vpad.c src/ps5_log.c src/ps5_web.c \
        src/ps5_telemetry.c \
        src/x360_receiver.c src/dualsense_mapper.c src/receiver_usb.c src/bridge.c
OBJS := $(patsubst %.c,$(PS5_BUILD)/%.o,$(SRCS))

.PHONY: all send clean
all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PS5_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -rf $(PS5_BUILD) $(ELF)
