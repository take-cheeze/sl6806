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

TESTS := test_console test_gfx test_panel test_lcdc test_adc test_pwm

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

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

test_console: test_console.c $(CORE)/sl6806_console.c
	$(CC) $(CFLAGS) $^ -o $@

test_gfx: test_gfx.c $(CORE)/gfx/Framebuffer.c $(CORE)/gfx/font5x7.c
	$(CC) $(CFLAGS) $^ -o $@

# Links the real variant panel tables, so regenerating them with
# tools/sl6806-panelseq is checked against the assertions rather than trusted.
test_panel: test_panel.c $(CORE)/gfx/LcdBus.c $(VARIANT)/panel.c
	$(CC) $(CFLAGS) $^ -o $@

# The LCD controller driver against a model of the controller.
# SL6806_MMIO_HOOKED replaces the volatile load and store in sl6806_mmio.h
# with the test's own, so the driver's real register sequence is what runs.
test_lcdc: test_lcdc.c $(CORE)/sl6806_lcdc.c $(CORE)/sl6806_padctl.c \
           $(CORE)/gfx/LcdBus.c $(VARIANT)/panel.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $^ -o $@

# The ADC driver against a model that only opens its gate for the mask ROM's
# write order, and lies plausibly until it does - which is what the real block
# does and why this was wrong on hardware for four runs.
test_adc: test_adc.c $(CORE)/sl6806_adc.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $^ -o $@

# The backlight driver against a model that only ungates for the mask ROM's
# write order. Guards the pair-register clock enable above all - nothing in the
# vendor's firmware writes it, so it looks removable and is not.
test_pwm: test_pwm.c $(CORE)/sl6806_pwm.c $(CORE)/sl6806_module.c
	$(CC) $(CFLAGS) -DSL6806_MMIO_HOOKED $^ -o $@

clean:
	rm -f $(TESTS) *.ppm
