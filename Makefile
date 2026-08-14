# SL6806 Arduino-like framework
#
#   make SKETCH=examples/Blink                 build a RAM payload (safe)
#   make SKETCH=examples/Blink upload          load it and run it over USB
#   make SKETCH=examples/Blink monitor         watch its Serial output
#   make SKETCH=examples/Blink run             upload, then monitor
#   make SKETCH=examples/Blink calibrate       measure the real CPU clock
#   make SKETCH=examples/Blink RUN_MODE=poll   drive loop() from USB polls
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
# [V] MEASURED, 2026-08-06, on one P20 Player: 64,000,071 Hz over 94 samples
# in 20 s, bracket 63,961,008..64,039,063, which contains exactly one whole
# MHz. Measured against the host clock with `make calibrate`, not derived from
# a PLL register - the PLL has still not been found. Re-measure on a different
# unit rather than assuming; see cores/sl6806/sl6806_stat.h.
F_CPU       ?= 64000000
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
            $(CORE_DIR)/wiring_extra.c \
            $(CORE_DIR)/hal_gpio.c $(CORE_DIR)/sl6806_console.c \
            $(CORE_DIR)/sl6806_padctl.c $(CORE_DIR)/sl6806_lcdc.c \
            $(CORE_DIR)/sl6806_adc.c $(CORE_DIR)/sl6806_module.c \
            $(CORE_DIR)/sl6806_pwm.c $(CORE_DIR)/sl6806_regfile.c \
            $(CORE_DIR)/sl6806_audio.c $(CORE_DIR)/sl6806_bt.c \
            $(CORE_DIR)/sl6806_sd.c $(CORE_DIR)/sl6806_fat.c \
            $(CORE_DIR)/syscalls.c \
            $(CORE_DIR)/gfx/Framebuffer.c $(CORE_DIR)/gfx/font5x7.c \
            $(CORE_DIR)/gfx/LcdBus.c
CORE_CXX := $(CORE_DIR)/main.cpp $(CORE_DIR)/Print.cpp $(CORE_DIR)/Stream.cpp \
            $(CORE_DIR)/WString.cpp $(CORE_DIR)/HardwareSerial.cpp \
            $(CORE_DIR)/cxx_support.cpp $(CORE_DIR)/gfx/Display.cpp

ifeq ($(MODE),firmware)
CORE_C  += $(CORE_DIR)/startup_firmware.c
LDSCRIPT := ld/sl6806_firmware.ld
MODE_DEF := -DSL6806_BUILD_FIRMWARE=1 -DSL6806_BUILD_PAYLOAD=0
else
CORE_C  += $(CORE_DIR)/startup_payload.c
LDSCRIPT := ld/sl6806_payload.ld
MODE_DEF := -DSL6806_BUILD_PAYLOAD=1 -DSL6806_BUILD_FIRMWARE=0
endif

# hook (default) = ROM idle callback, poll = the SCSI handler, takeover = spin.
# See the note at the top of cores/sl6806/startup_payload.c: if a sketch prints
# setup()'s output and then never ticks, the ROM's idle callback is not
# periodic on that unit and RUN_MODE=poll is the fix.
ifeq ($(RUN_MODE),takeover)
MODE_DEF += -DSL6806_RUN_MODE=1
else ifeq ($(RUN_MODE),poll)
MODE_DEF += -DSL6806_RUN_MODE=2
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

# examples/PadSweep takes one GPIO bank per build, so a bank that wedges the
# device cannot take the others with it.
ifneq ($(SWEEP_BANK),)
DEFS   += -DSWEEP_BANK=$(SWEEP_BANK)
endif
ifneq ($(SWEEP_LEVEL),)
DEFS   += -DSWEEP_LEVEL=$(SWEEP_LEVEL)
endif

# Appended to every compile. CI passes -Werror here rather than baking it in,
# so a warning stops the build there without making local experiments painful.
EXTRA_FLAGS ?=

CFLAGS   := $(ARCH) $(WARN) $(OPT) $(INCS) $(DEFS) $(EXTRA_FLAGS) -std=gnu11
CXXFLAGS := $(ARCH) $(WARN) $(OPT) $(INCS) $(DEFS) $(EXTRA_FLAGS) -std=gnu++17 \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections \
            -Wl,-Map=$(OUT).map -specs=nano.specs -u _printf_float

OBJS := $(addprefix $(BUILD_DIR)/obj/,$(CORE_C:.c=.c.o) $(CORE_CXX:.cpp=.cpp.o) \
                                      $(VARIANT_C:.c=.c.o))
OBJS += $(addprefix $(BUILD_DIR)/obj/,$(addsuffix .o,$(SKETCH_SRC)))

# ------------------------------------------------------------ flag tracking
#
# WHY THIS EXISTS. RUN_MODE, MODE, F_CPU and EXTRA_FLAGS all reach the
# compiler as -D. None of them is a file, so make cannot see one change: after
# `make RUN_MODE=hook`, a `make RUN_MODE=poll` into the same build directory
# rebuilds *nothing* and re-uploads the previous mode's binary, byte for byte.
#
# That is not a tidiness problem. It silently invalidates bench measurements -
# you believe you are testing poll mode and you are running the hook image -
# and it cost a debugging session before it was noticed. So the flags are
# written to a file, and every object depends on that file.
FLAGSTAMP := $(BUILD_DIR)/.buildflags
BUILD_ID  := $(CFLAGS) :: $(CXXFLAGS) :: $(LDFLAGS) :: $(LDSCRIPT)

# Evaluated while the makefile is read, so the stamp is already correct by the
# time any rule looks at it. Rewritten only when it actually differs, or every
# build would appear out of date.
$(shell mkdir -p $(BUILD_DIR); \
        printf '%s\n' '$(BUILD_ID)' > $(FLAGSTAMP).new; \
        cmp -s $(FLAGSTAMP).new $(FLAGSTAMP) || cp $(FLAGSTAMP).new $(FLAGSTAMP); \
        rm -f $(FLAGSTAMP).new)

# ----------------------------------------------------------------- rules
.PHONY: all clean upload monitor run calibrate size test
all: $(OUT).bin size

# All three suites. tests/emu needs the toolchain and Unicorn and says so if
# they are missing, so this stays useful with only gcc installed.
test:
	$(MAKE) -C tests/host
	$(MAKE) -C tests/tools
	$(MAKE) -C tests/emu

$(BUILD_DIR)/obj/%.c.o: %.c $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/%.cpp.o: %.cpp $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# .ino is C++ with Arduino.h already included, as on Arduino. Unlike the
# Arduino IDE we do not synthesise forward declarations, so define your own
# helpers before you call them.
$(BUILD_DIR)/obj/%.ino.o: %.ino $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -x c++ -include Arduino.h -MMD -MP -c $< -o $@

$(OUT).elf: $(OBJS) $(LDSCRIPT) $(FLAGSTAMP)
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

# Measures the real CPU clock and prints the F_CPU to rebuild with. Any sketch
# will do - the device answers this from the core, not from loop() - so the
# one already uploaded is fine if you skip the dependency.
calibrate: upload
	tools/sl6806-calibrate

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
