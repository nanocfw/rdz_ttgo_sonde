# Makefile — convenience wrappers around PlatformIO for the rdz_ttgo_sonde firmware.
#
# PlatformIO is not on PATH on this machine: the system Python (3.9) is too old for
# the pinned Tasmota espressif32 platform, so pio runs from a Python 3.13 venv.
# Override PIO=... on the command line to use a different PlatformIO.
#
#   make            # compile firmware (default)
#   make upload     # flash firmware over USB
#   make image      # build + merge into a single firmware-image.bin (full 4MB image)
#   make help       # list all targets

# PlatformIO executable (Python 3.13 venv created during setup).
PIO ?= /home/dev-python/.pio-venv/bin/pio

# PlatformIO's managed penv — holds esptool + deps and a `python` on PATH.
# The repo's merge step (scripts/pio-build-extension.py) shells out to `esptool.py`,
# whose `#!/usr/bin/env python` shebang needs this dir on PATH to resolve.
PENV_BIN ?= /home/dev-python/.platformio/penv/bin
MERGE_PATH := PATH=$(PENV_BIN):$$PATH

.DEFAULT_GOAL := build
.PHONY: build upload uploadfs buildfs uploadfonts monitor image clean help

build: ## Compile firmware
	$(PIO) run

upload: ## Compile + flash firmware over USB
	$(PIO) run --target upload

uploadfs: ## Build + flash the LittleFS data partition (RX_FSK/data/)
	$(PIO) run --target uploadfs

buildfs: ## Build the LittleFS data image (littlefs.bin) without flashing
	$(PIO) run --target buildfs

uploadfonts: ## Flash the fonts partition (only when a separate fonts partition is enabled)
	$(PIO) run --target uploadfonts

monitor: ## Open the serial monitor (115200 baud)
	$(PIO) run --target monitor

image: buildfs ## Build the merged single-file firmware-image.bin (bootloader+partitions+app+fonts+fs)
	$(MERGE_PATH) $(PIO) run --target firmware
	@echo "Merged image: .pio/build/ttgo-lora32/firmware-image.bin"

clean: ## Remove build artifacts
	$(PIO) run --target clean

help: ## List available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
