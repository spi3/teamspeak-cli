SHELL := /bin/bash

.DEFAULT_GOAL := help

CMAKE ?= $(or $(shell command -v cmake 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/cmake)
CTEST ?= $(or $(shell command -v ctest 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/ctest)
NINJA ?= $(or $(shell command -v ninja 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/ninja)

CMAKE_GENERATOR ?= Ninja
CMAKE_BUILD_TYPE ?= Debug

TS3_REAL_E2E_CACHE_DIR ?= .cache/ts3-real-e2e
TS3_REAL_E2E_DEPS_MK ?= $(TS3_REAL_E2E_CACHE_DIR)/deps.mk
TS3_REAL_E2E_DEPS_ENV ?= $(TS3_REAL_E2E_CACHE_DIR)/deps.env

-include $(TS3_REAL_E2E_DEPS_MK)

BUILD_DIR ?= build
PLUGIN_BUILD_DIR ?= build-plugin
REAL_BUILD_DIR ?= build-real

TS_BIN ?= $(BUILD_DIR)/ts
REAL_TS_BIN ?= $(REAL_BUILD_DIR)/ts
REAL_PLUGIN_SO ?= $(REAL_BUILD_DIR)/ts3cli_plugin.so

TS3_PLUGIN_SDK_DIR ?=
TS3_PLUGIN_SDK_INCLUDE_DIR ?=
TS3_PLUGIN_SDK_URL ?=
TS3_PLUGIN_SDK_REF ?=
TS3_REAL_E2E_CLIENT_DIR ?=
TS3_REAL_E2E_CLIENT_VERSION ?=
TS3_REAL_E2E_CLIENT_URL ?=
TS3_REAL_E2E_CLIENT_SHA256 ?=
TS3_REAL_E2E_XDOTOOL ?=
TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH ?=

REAL_ENV_STATE_REF ?= $(TS3_REAL_E2E_CACHE_DIR)/manual-env.state
REAL_ENV_OUTPUT_REF ?= $(TS3_REAL_E2E_CACHE_DIR)/manual-env.output

CMAKE_BASE_ARGS := -G $(CMAKE_GENERATOR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
ifeq ($(CMAKE_GENERATOR),Ninja)
  CMAKE_BASE_ARGS += -DCMAKE_MAKE_PROGRAM=$(NINJA)
endif

PLUGIN_CMAKE_ARGS := -DTS_ENABLE_TS3_PLUGIN=ON
ifneq ($(strip $(TS3_PLUGIN_SDK_DIR)),)
  PLUGIN_CMAKE_ARGS += -DTS3_PLUGIN_SDK_DIR=$(TS3_PLUGIN_SDK_DIR)
endif
ifneq ($(strip $(TS3_PLUGIN_SDK_INCLUDE_DIR)),)
  PLUGIN_CMAKE_ARGS += -DTS3_PLUGIN_SDK_INCLUDE_DIR=$(TS3_PLUGIN_SDK_INCLUDE_DIR)
endif

REAL_CMAKE_ARGS := $(PLUGIN_CMAKE_ARGS) -DTS_ENABLE_REAL_TS3_E2E=ON -DTS3_REAL_E2E_CACHE_DIR=$(TS3_REAL_E2E_CACHE_DIR)
ifneq ($(strip $(TS3_REAL_E2E_CLIENT_DIR)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_CLIENT_DIR=$(TS3_REAL_E2E_CLIENT_DIR)
endif
ifneq ($(strip $(TS3_REAL_E2E_CLIENT_VERSION)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_CLIENT_VERSION=$(TS3_REAL_E2E_CLIENT_VERSION)
endif
ifneq ($(strip $(TS3_REAL_E2E_CLIENT_URL)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_CLIENT_URL=$(TS3_REAL_E2E_CLIENT_URL)
endif
ifneq ($(strip $(TS3_REAL_E2E_CLIENT_SHA256)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_CLIENT_SHA256=$(TS3_REAL_E2E_CLIENT_SHA256)
endif
ifneq ($(strip $(TS3_REAL_E2E_XDOTOOL)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_XDOTOOL=$(TS3_REAL_E2E_XDOTOOL)
endif
ifneq ($(strip $(TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH)),)
  REAL_CMAKE_ARGS += -DTS3_REAL_E2E_XDOTOOL_LIBRARY_PATH=$(TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH)
endif

define require_plugin_sdk
	@if [[ -z "$(TS3_PLUGIN_SDK_DIR)" && -z "$(TS3_PLUGIN_SDK_INCLUDE_DIR)" ]]; then \
		echo "Set TS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk or TS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/include"; \
		exit 1; \
	fi
endef

define require_real_env_state
	@if [[ ! -f "$(REAL_ENV_STATE_REF)" ]]; then \
		echo "No tracked real test environment. Run 'make env-up' first."; \
		exit 1; \
	fi
	@state_file="$$(cat "$(REAL_ENV_STATE_REF)")"; \
	if [[ -z "$$state_file" || ! -f "$$state_file" ]]; then \
		echo "Tracked real test environment state is missing. Run 'make env-up' again."; \
		exit 1; \
	fi
endef

.PHONY: help configure build test ts clean \
	deps-real \
	configure-plugin build-plugin \
	configure-real build-real test-real test-real-all \
	env-up env-info env-ts env-down

help: ## Show available development commands
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {printf "%-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

configure: ## Configure the default CLI/test build in ./build
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_BASE_ARGS)

build: configure ## Build the default CLI/test tree
	$(CMAKE) --build $(BUILD_DIR)

test: build ## Run the default test suite
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

ts: build ## Run the CLI from ./build with ARGS='...'
	"$(TS_BIN)" $(ARGS)

clean: ## Remove generated build directories and tracked env pointers
	rm -rf $(BUILD_DIR) $(PLUGIN_BUILD_DIR) $(REAL_BUILD_DIR)
	rm -f $(REAL_ENV_STATE_REF) $(REAL_ENV_OUTPUT_REF)

deps-real: ## Download/cache the real runtime dependencies without starting the environment
	@mkdir -p "$(TS3_REAL_E2E_CACHE_DIR)"
	@TS3_REAL_E2E_CACHE_DIR="$(TS3_REAL_E2E_CACHE_DIR)" \
	TS3_REAL_E2E_DEPS_MK="$(TS3_REAL_E2E_DEPS_MK)" \
	TS3_REAL_E2E_DEPS_ENV="$(TS3_REAL_E2E_DEPS_ENV)" \
	TS3_PLUGIN_SDK_DIR="$(TS3_PLUGIN_SDK_DIR)" \
	TS3_PLUGIN_SDK_INCLUDE_DIR="$(TS3_PLUGIN_SDK_INCLUDE_DIR)" \
	TS3_PLUGIN_SDK_URL="$(TS3_PLUGIN_SDK_URL)" \
	TS3_PLUGIN_SDK_REF="$(TS3_PLUGIN_SDK_REF)" \
	TS3_REAL_E2E_CLIENT_DIR="$(TS3_REAL_E2E_CLIENT_DIR)" \
	TS3_REAL_E2E_CLIENT_VERSION="$(TS3_REAL_E2E_CLIENT_VERSION)" \
	TS3_REAL_E2E_CLIENT_URL="$(TS3_REAL_E2E_CLIENT_URL)" \
	TS3_REAL_E2E_CLIENT_SHA256="$(TS3_REAL_E2E_CLIENT_SHA256)" \
	TS3_REAL_E2E_XDOTOOL="$(TS3_REAL_E2E_XDOTOOL)" \
	TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH="$(TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH)" \
	./tests/e2e/bootstrap_real_runtime_deps.sh

configure-plugin: ## Configure the plugin build in ./build-plugin
	$(require_plugin_sdk)
	$(CMAKE) -S . -B $(PLUGIN_BUILD_DIR) $(CMAKE_BASE_ARGS) $(PLUGIN_CMAKE_ARGS)

build-plugin: configure-plugin ## Build ts3cli_plugin in ./build-plugin
	$(CMAKE) --build $(PLUGIN_BUILD_DIR) --target ts3cli_plugin

configure-real: ## Configure the live plugin+server build in ./build-real
	$(require_plugin_sdk)
	$(CMAKE) -S . -B $(REAL_BUILD_DIR) $(CMAKE_BASE_ARGS) $(REAL_CMAKE_ARGS)

build-real: configure-real ## Build the live plugin+server tree in ./build-real
	$(CMAKE) --build $(REAL_BUILD_DIR)

test-real: build-real ## Run the live TeamSpeak client+server E2E test
	$(CTEST) --test-dir $(REAL_BUILD_DIR) --output-on-failure -R ts_real_plugin_server_e2e_test

test-real-all: build-real ## Run the full test suite from the live ./build-real tree
	$(CTEST) --test-dir $(REAL_BUILD_DIR) --output-on-failure

env-up: build-real ## Start the real TeamSpeak test environment and track its state
	@mkdir -p "$(TS3_REAL_E2E_CACHE_DIR)"
	@rm -f "$(REAL_ENV_STATE_REF)" "$(REAL_ENV_OUTPUT_REF)"
	@set -euo pipefail; \
	./tests/e2e/run_real_plugin_server_env.sh "$(REAL_TS_BIN)" "$(REAL_PLUGIN_SO)" | tee "$(REAL_ENV_OUTPUT_REF)"; \
	state_file="$$(sed -n 's/^State file:[[:space:]]*//p' "$(REAL_ENV_OUTPUT_REF)" | head -n 1)"; \
	if [[ -z "$$state_file" ]]; then \
		echo "Failed to capture the real environment state file path"; \
		exit 1; \
	fi; \
	printf '%s\n' "$$state_file" > "$(REAL_ENV_STATE_REF)"; \
	echo "Tracked state file: $$state_file"

env-info: ## Show the tracked real TeamSpeak test environment details
	$(require_real_env_state)
	@source "$$(cat "$(REAL_ENV_STATE_REF)")"; \
	echo "State file: $$(cat "$(REAL_ENV_STATE_REF)")"; \
	echo "Config: $$config_path"; \
	echo "Socket: $$socket_path"; \
	echo "Server: $$server_host:$$server_port"; \
	echo "Display: $$display"; \
	echo "Client runtime: $$client_runtime_dir"; \
	echo "CLI: $$ts_bin"

env-ts: ## Run the built CLI against the tracked real environment with ARGS='...'
	$(require_real_env_state)
	@source "$$(cat "$(REAL_ENV_STATE_REF)")"; \
	"$$ts_bin" --config "$$config_path" $(ARGS)

env-down: ## Stop the tracked real TeamSpeak test environment
	$(require_real_env_state)
	@state_file="$$(cat "$(REAL_ENV_STATE_REF)")"; \
	./tests/e2e/stop_real_plugin_server_env.sh "$$state_file"; \
	rm -f "$(REAL_ENV_STATE_REF)" "$(REAL_ENV_OUTPUT_REF)"
