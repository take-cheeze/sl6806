# SL6806 Arduino-like framework
#
#   make SKETCH=examples/Blink                 build a RAM payload (safe)
#   make SKETCH=examples/Blink upload          load it and run it over USB
#   make SKETCH=examples/Blink monitor         watch its Serial output
#   make SKETCH=examples/Blink run             upload, then monitor
#   make SKETCH=examples/Blink MODE=firmware   build a flashable FIRM image
#   make clean
#
# MODE=payload (the default) never writes to flash and cannot brick the
# device. MODE=firmware produces an image you would have to flash yourself;
# read docs/FLASHING.md first.

SKETCH      ?= examples/Blink
BOARD       ?= p20_player
MODE        ?= payload
BUILD_DIR   ?= build
F_CPU       ?= 120000000
RUN_MODE    ?= hook

# Toolchain
PREFIX      ?= arm-none-eabi-
CC          := $(PREFIX)gcc
CXX         := $(PREFIX)g++
OBJCOPY     := $(PREFIX)objcopy
SIZE        := $(PREFIX)size
NM          := $(PREFIX)nm

SKETCH_NAME := $(notdir $(patsubst %/,%,$(SKETCH)))
OUT         := $(BUILD_DIR)/$(SKETCH_NAME)
CORE_DIR    := cores/sl6806
VARIANT_DIR := variants/$(BOARD)

# ---------------------------------------------------------------- sources
CORE_C   := $(CORE_DIR)/wiring_time.c $(CORE_DIR)/wiring_digital.c \
            $(CORE_DIR)/hal_gpio.c $(CORE_DIR)/sl6806_console.c \
            $(CORE_DIR)/syscalls.c
CORE_CXX := $(CORE_DIR)/main.cpp $(CORE_DIR)/Print.cpp $(CORE_DIR)/Stream.cpp \
            $(CORE_DIR)/WString.cpp $(CORE_DIR)/HardwareSerial.cpp \
            $(CORE_DIR)/cxx_support.cpp

ifeq ($(MODE),firmware)
CORE_C  += $(CORE_DIR)/startup_firmware.c
LDSCRIPT := ld/sl6806_firmware.ld
MODE_DEF := -DSL6806_BUILD_FIRMWARE=1 -DSL6806_BUILD_PAYLOAD=0
else
CORE_C  += $(CORE_DIR)/startup_payload.c
LDSCRIPT := ld/sl6806_payload.ld
MODE_DEF := -DSL6806_BUILD_PAYLOAD=1 -DSL6806_BUILD_FIRMWARE=0
endif

ifeq ($(RUN_MODE),takeover)
MODE_DEF += -DSL6806_RUN_MODE=1
else
MODE_DEF += -DSL6806_RUN_MODE=0
endif

VARIANT_C := $(wildcard $(VARIANT_DIR)/*.c)
SKETCH_SRC := $(wildcard $(SKETCH)/*.ino) $(wildcard $(SKETCH)/*.cpp) \
              $(wildcard $(SKETCH)/*.c)

ifeq ($(strip $(SKETCH_SRC)),)
$(error No .ino/.cpp/.c found in "$(SKETCH)")
endif

# ----------------------------------------------------------------- flags
ARCH   := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
WARN   := -Wall -Wextra
OPT    := -Os -ffunction-sections -fdata-sections -fno-common
INCS   := -I$(CORE_DIR) -I$(VARIANT_DIR)
DEFS   := -DF_CPU=$(F_CPU)UL -DSL6806=1 $(MODE_DEF)

CFLAGS   := $(ARCH) $(WARN) $(OPT) $(INCS) $(DEFS) -std=gnu11
CXXFLAGS := $(ARCH) $(WARN) $(OPT) $(INCS) $(DEFS) -std=gnu++17 \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections \
            -Wl,-Map=$(OUT).map -specs=nano.specs -u _printf_float

OBJS := $(addprefix $(BUILD_DIR)/obj/,$(CORE_C:.c=.c.o) $(CORE_CXX:.cpp=.cpp.o) \
                                      $(VARIANT_C:.c=.c.o))
OBJS += $(addprefix $(BUILD_DIR)/obj/,$(addsuffix .o,$(SKETCH_SRC)))

# ----------------------------------------------------------------- rules
.PHONY: all clean upload monitor run size
all: $(OUT).bin size

$(BUILD_DIR)/obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# .ino is C++ with Arduino.h already included, as on Arduino. Unlike the
# Arduino IDE we do not synthesise forward declarations, so define your own
# helpers before you call them.
$(BUILD_DIR)/obj/%.ino.o: %.ino
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -x c++ -include Arduino.h -MMD -MP -c $< -o $@

$(OUT).elf: $(OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@ -lc -lm -lgcc

# The monitor finds the console ring by symbol, so export it next to the
# binary instead of hardcoding an address anywhere.
$(OUT).bin: $(OUT).elf
	$(OBJCOPY) -O binary -R .bss -R .heap $< $@
	@$(NM) $< | grep -i ' _sl6806_console$$' > $(OUT).sym || \
		echo "warning: console symbol not found" >&2
	@echo "built $@  (console: $$(cut -d' ' -f1 < $(OUT).sym))"

size: $(OUT).elf
	@$(SIZE) $<

clean:
	rm -rf $(BUILD_DIR)

upload: $(OUT).bin
	tools/sl6806-upload $(OUT).bin

monitor: $(OUT).bin
	tools/sl6806-monitor $(OUT).sym

run: upload
	tools/sl6806-monitor $(OUT).sym

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
