PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CMAKE_BUILD_DIR := $(PROJECT_ROOT)/build/standard
UF2_OUTPUT_DIR := $(PROJECT_ROOT)/uf2Build
# Stamped once per make invocation: ds5-bridge-DDMMYY-HHMM.uf2
BUILD_STAMP := $(shell date +%d%m%y-%H%M)
UF2_OUTPUT := $(UF2_OUTPUT_DIR)/ds5-bridge-$(BUILD_STAMP).uf2

PICO_SDK_PATH ?= $(PROJECT_ROOT)/.deps/pico-sdk
PICO_TOOLCHAIN_PATH ?=
CMAKE_TOOLCHAIN_ARG = $(if $(strip $(PICO_TOOLCHAIN_PATH)),-DPICO_TOOLCHAIN_PATH="$(PICO_TOOLCHAIN_PATH)",)

.PHONY: build-uf2

build-uf2:
	@test -f "$(PICO_SDK_PATH)/pico_sdk_init.cmake" || \
		(echo "Pico SDK not found at $(PICO_SDK_PATH)"; exit 1)
	cmake -S "$(PROJECT_ROOT)" -B "$(CMAKE_BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DPICO_BOARD=pico2_w \
		-DPICO_SDK_PATH="$(PICO_SDK_PATH)" $(CMAKE_TOOLCHAIN_ARG)
	cmake --build "$(CMAKE_BUILD_DIR)" --target ds5-bridge
	@mkdir -p "$(UF2_OUTPUT_DIR)"
	@cp "$(CMAKE_BUILD_DIR)/ds5-bridge.uf2" "$(UF2_OUTPUT)"
	@printf '\nUF2 built successfully:\n  %s\n' "$(UF2_OUTPUT)"
