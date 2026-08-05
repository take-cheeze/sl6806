# Emulator smoke tests.
#
#   make -C tests/emu
#
# Builds real payload images with the project's own Makefile and runs them
# under Unicorn. Needs the ARM toolchain (to build) and `pip install unicorn`
# (to run); `make -C tests/emu check` reports what is missing instead of
# failing with an import error.

PYTHON ?= python3

.PHONY: all check clean
all: check
	$(PYTHON) test_smoke.py

check:
	@command -v arm-none-eabi-gcc >/dev/null || { \
		echo "error: arm-none-eabi-gcc not found - these tests build real images"; \
		echo "       sudo apt install gcc-arm-none-eabi"; exit 1; }
	@$(PYTHON) -c "import unicorn" 2>/dev/null || { \
		echo "error: the unicorn python module is not installed"; \
		echo "       pip install unicorn"; exit 1; }

clean:
	rm -rf ../../build/emu
