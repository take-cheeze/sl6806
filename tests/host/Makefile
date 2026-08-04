# Native tests for the parts of the core that are pure logic.
#
#   make -C tests/host
#
# The stub sl6806.h in this directory comes first in the include path, so core
# sources compile natively without being modified for testing.

CORE  := ../../cores/sl6806
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
          -I. -I$(CORE) -include host_stub.h

TESTS := test_console test_gfx

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

run-test_console: test_console
	./$<

run-test_gfx: test_gfx
	./$<

test_console: test_console.c $(CORE)/sl6806_console.c
	$(CC) $(CFLAGS) $^ -o $@

test_gfx: test_gfx.c $(CORE)/gfx/Framebuffer.c $(CORE)/gfx/font5x7.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(TESTS) *.ppm
