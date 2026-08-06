# Unit tests for the Python tools.
#
#   make -C tests/tools
#
# Pure logic only - nothing here touches USB or needs a device. These cover
# the parts of the tools that interpret what hardware says back, which is
# exactly what `--help` in CI cannot check.

PYTHON ?= python3
TESTS  := test_inquiry.py test_clockcal.py

.PHONY: all clean
all:
	@for t in $(TESTS); do $(PYTHON) $$t || exit 1; done

clean:
	rm -rf __pycache__
