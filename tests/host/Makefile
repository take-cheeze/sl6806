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

TESTS := test_console test_gfx test_panel

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

run-test_console: test_console
	./$<

run-test_gfx: test_gfx
	./$<

run-test_panel: test_panel
	./$<

test_console: test_console.c $(CORE)/sl6806_console.c
	$(CC) $(CFLAGS) $^ -o $@

test_gfx: test_gfx.c $(CORE)/gfx/Framebuffer.c $(CORE)/gfx/font5x7.c
	$(CC) $(CFLAGS) $^ -o $@

# Links the real variant panel tables, so regenerating them with
# tools/sl6806-panelseq is checked against the assertions rather than trusted.
test_panel: test_panel.c $(CORE)/gfx/LcdBus.c $(VARIANT)/panel.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(TESTS) *.ppm
