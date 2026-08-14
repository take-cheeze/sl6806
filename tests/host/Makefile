# Native tests for the parts of the core that are pure logic.
#
#   make -C tests/host
#
# The stub sl6806.h in this directory comes first in the include path, so core
# sources compile natively without being modified for testing.

CORE    := ../../cores/sl6806
VARIANT := ../../variants/p20_player
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
          -I. -I$(CORE) -I$(VARIANT) -include host_stub.h

TESTS := test_console test_gfx test_panel test_lcdc test_adc test_pwm \
         test_regfile test_audio test_bt test_sd test_fat test_uart test_firm test_hwinit \
         test_dvp

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

# Every test binary depends on every core header.
#
# WHY THIS LINE EXISTS. The rules below list only .c files, so a change to a
# header rebuilt nothing and the stale binary ran. That is not hypothetical:
# raising SL6806_CONSOLE_SIZE from 2048 to 65536 passed locally against a
# binary compiled with the old value, and failed in CI, which builds from
# scratch. A suite that does not notice a header change is worse than no
# suite, because it reports success.
HEADERS := $(wildcard $(CORE)/*.h) $(wildcard $(CORE)/gfx/*.h) \
           $(wildcard $(VARIANT)/*.h) host_stub.h
$(TESTS): $(HEADERS)

# ...and because of that the recipes below compile $(filter %.c,$^) rather
# than $^, or cc would be handed a header to compile as a translation unit.

run-test_console: test_console
	./$<

run-test_gfx: test_gfx
	./$<

run-test_panel: test_panel
	./$<

run-test_lcdc: test_lcdc
	./$<

run-test_adc: test_adc
	./$<

run-test_pwm: test_pwm
	./$<

run-test_regfile: test_regfile
	./$<

run-test_audio: test_audio
	./$<

run-test_bt: test_bt
	./$<

run-test_sd: test_sd
	./$<

run-test_fat: test_fat
	./$<

run-test_uart: test_uart
	./$<

run-test_firm: test_firm
	./$<

run-test_hwinit: test_hwinit
	./$<

run-test_dvp: test_dvp
	./$<

test_console: test_console.c $(CORE)/sl6806_console.c
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

test_gfx: test_gfx.c $(CORE)/gfx/Framebuffer.c $(CORE)/gfx/font5x7.c
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# Links the real variant panel tables, so regenerating them with
# tools/sl6806-panelseq is checked against the assertions rather than trusted.
test_panel: test_panel.c $(CORE)/gfx/LcdBus.c $(VARIANT)/panel.c
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# The LCD controller driver against a model of the controller.
# SL6806_MMIO_HOOKED replaces the volatile load and store in sl6806_mmio.h
# with the test's own, so the driver's real register sequence is what runs.
test_lcdc: test_lcdc.c $(CORE)/sl6806_lcdc.c $(CORE)/sl6806_padctl.c \
           $(CORE)/gfx/LcdBus.c $(VARIANT)/panel.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The ADC driver against a model that only opens its gate for the mask ROM's
# write order, and lies plausibly until it does - which is what the real block
# does and why this was wrong on hardware for four runs.
test_adc: test_adc.c $(CORE)/sl6806_adc.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The backlight driver against a model that only ungates for the mask ROM's
# write order. Guards the pair-register clock enable above all - nothing in the
# vendor's firmware writes it, so it looks removable and is not.
test_pwm: test_pwm.c $(CORE)/sl6806_pwm.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The indexed register file against a model of the mailbox and the chip on
# the far side of it. This driver has never run on hardware, so what these
# check is that the transcription performs the vendor's sequence - the
# transfer order, the direction byte, the bounded poll, and the two voltage
# scales, in both directions.
test_regfile: test_regfile.c $(CORE)/sl6806_regfile.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The audio controller against a model of the block, its module gate and the
# CRU in front of both. Like test_regfile this driver has never run, so what
# these check is that the transcription performs the vendor's sequence: the
# descriptor order, the three-quarter watermark, the 8000 Hz rate split, and
# a gate test that writes rather than trusting a plausible read.
test_audio: test_audio.c $(CORE)/sl6806_audio.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The Bluetooth gate. This is the one sequence in the tree that starts a PLL,
# so the order is the whole content and every poll in it has to be bounded -
# the PLL's lock bit is known not to set in bootloader mode.
test_bt: test_bt.c $(CORE)/sl6806_bt.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The SD host against a model of the controller, a card, and the gate pair in
# front of both. Nothing in this driver has run on hardware, so these check
# that it performs the mask ROM's sequence: the command-word table, the order
# of identification, the two clock configurations, eight words per FIFO
# acknowledgement, and byte addressing for everything but SDHC.
test_sd: test_sd.c $(CORE)/sl6806_sd.c $(CORE)/sl6806_module.c \
         $(CORE)/sl6806_padctl.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The FAT reader against FAT16 and FAT32 volumes built in memory. It takes a
# block-reader callback and knows nothing about SD, which is the whole reason
# it can be tested at all - the SD host has never completed a command.
test_fat: test_fat.c $(CORE)/sl6806_fat.c
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# The serial port at 0x40091000 against a model of the block and its gates.
# Nobody has seen a character out of it, so these check that the driver
# performs the mask ROM's sequence: wait then write, bounded, nine bits, CR
# expansion, and gates before registers.
test_uart: test_uart.c $(CORE)/sl6806_uart.c $(CORE)/sl6806_module.c \
           $(CORE)/sl6806_padctl.c $(CORE)/sl6806_console.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The FIRM header parse. The jump cannot be tested - it ends with the
# application running and USB gone - so what these cover is everything that
# decides whether to jump, which is where the damage would be.
# SL6806_FIRM_HOSTED drops the two device-only functions - the XIP read and
# the jump, which is Cortex-M inline assembly - and leaves the parse.
test_firm: test_firm.c $(CORE)/sl6806_firm.c
	$(CC) $(CFLAGS) -DSL6806_FIRM_HOSTED=1 $(filter %.c,$^) -o $@

# The PLL rate decode and the two masked writes. Both would be silently
# wrong on hardware: a bad decode gives a plausible number, and a widened
# mask clears a field nothing here knows the meaning of.
test_hwinit: test_hwinit.c $(CORE)/sl6806_hwinit.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

# The DVP DMA cluster against a model of just those four registers and a
# gate flag. Nothing in this cluster is in the vendor's own code - DvpProbe
# found it by writing and reading back, not by reading a routine - so there
# is no sequence to hold the driver to beyond the one it states for itself:
# rejections before any register is touched, address before length before
# start, and a write-and-read-back proof that a gated block cannot fake.
test_dvp: test_dvp.c $(CORE)/sl6806_dvp.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $(filter %.c,$^) -o $@

clean:
	rm -f $(TESTS) *.ppm
