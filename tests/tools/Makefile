# Unit tests for the Python tools.
#
#   make -C tests/tools
#
# Nothing here touches USB or needs a device - test_calibrate drives the tool
# against a fake smtlink_dump that simulates one. These cover the parts of the
# tools that interpret what hardware says back, which is exactly what `--help`
# in CI cannot check.

PYTHON ?= python3
TESTS  := test_inquiry.py test_calibrate.py test_actions_cardreader.py \
          test_actions_try_subcmd.py

.PHONY: all clean
all:
	@for t in $(TESTS); do $(PYTHON) $$t || exit 1; done

clean:
	rm -rf __pycache__
