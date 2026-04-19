SHELL := /bin/bash

.DEFAULT_GOAL := help

CMAKE ?= $(or $(shell command -v cmake 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/cmake)
CTEST ?= $(or $(shell command -v ctest 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/ctest)
NINJA ?= $(or $(shell command -v ninja 2>/dev/null),$(HOME)/.local/venvs/build-tools/bin/ninja)

CMAKE_GENERATOR ?= Ninja
CMAKE_BUILD_TYPE ?= Debug

TS3_MANAGED_DIR ?= third_party/teamspeak/managed
TS3_DEPS_MK ?= $(TS3_MANAGED_DIR)/deps.mk
TS3_DEPS_ENV ?= $(TS3_MANAGED_DIR)/deps.env

-include $(TS3_DEPS_MK)

BUILD_DIR ?= build
BUILT_TEST_BUILD_DIR ?= build-built-test

TS_BIN ?= $(BUILD_DIR)/ts
PLUGIN_SO ?= $(BUILD_DIR)/ts3cli_plugin.so
BUILT_TEST_TS_BIN ?= $(BUILT_TEST_BUILD_DIR)/ts

TS3_PLUGIN_SDK_DIR ?=
TS3_PLUGIN_SDK_INCLUDE_DIR ?=
TS3_PLUGIN_SDK_URL ?=
TS3_PLUGIN_SDK_REF ?=
TS3_CLIENT_DIR ?=
TS3_CLIENT_VERSION ?=
TS3_CLIENT_URL ?=
TS3_CLIENT_SHA256 ?=
TS3_XDOTOOL ?=
TS3_XDOTOOL_LIBRARY_PATH ?=

ENV_STATE_REF ?= $(TS3_MANAGED_DIR)/env.state
ENV_OUTPUT_REF ?= $(TS3_MANAGED_DIR)/env.output

CMAKE_BASE_ARGS := -G $(CMAKE_GENERATOR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
ifeq ($(CMAKE_GENERATOR),Ninja)
  CMAKE_BASE_ARGS += -DCMAKE_MAKE_PROGRAM=$(NINJA)
endif

define bootstrap_runtime_deps
	@mkdir -p "$(TS3_MANAGED_DIR)"
	@TS3_MANAGED_DIR="$(TS3_MANAGED_DIR)" \
	TS3_DEPS_MK="$(TS3_DEPS_MK)" \
	TS3_DEPS_ENV="$(TS3_DEPS_ENV)" \
	TS3_PLUGIN_SDK_DIR="$(TS3_PLUGIN_SDK_DIR)" \
	TS3_PLUGIN_SDK_INCLUDE_DIR="$(TS3_PLUGIN_SDK_INCLUDE_DIR)" \
	TS3_PLUGIN_SDK_URL="$(TS3_PLUGIN_SDK_URL)" \
	TS3_PLUGIN_SDK_REF="$(TS3_PLUGIN_SDK_REF)" \
	TS3_CLIENT_DIR="$(TS3_CLIENT_DIR)" \
	TS3_CLIENT_VERSION="$(TS3_CLIENT_VERSION)" \
	TS3_CLIENT_URL="$(TS3_CLIENT_URL)" \
	TS3_CLIENT_SHA256="$(TS3_CLIENT_SHA256)" \
	TS3_XDOTOOL="$(TS3_XDOTOOL)" \
	TS3_XDOTOOL_LIBRARY_PATH="$(TS3_XDOTOOL_LIBRARY_PATH)" \
	./tests/e2e/bootstrap_runtime_deps.sh
endef

define load_runtime_deps_env
	set -euo pipefail; \
	if [[ -f "$(TS3_DEPS_ENV)" ]]; then \
		source "$(TS3_DEPS_ENV)"; \
	fi; \
	if [[ -n "$(TS3_PLUGIN_SDK_DIR)" ]]; then export TS3_PLUGIN_SDK_DIR="$(TS3_PLUGIN_SDK_DIR)"; fi; \
	if [[ -n "$(TS3_PLUGIN_SDK_INCLUDE_DIR)" ]]; then export TS3_PLUGIN_SDK_INCLUDE_DIR="$(TS3_PLUGIN_SDK_INCLUDE_DIR)"; fi; \
	if [[ -n "$(TS3_CLIENT_DIR)" ]]; then export TS3_CLIENT_DIR="$(TS3_CLIENT_DIR)"; fi; \
	if [[ -n "$(TS3_CLIENT_VERSION)" ]]; then export TS3_CLIENT_VERSION="$(TS3_CLIENT_VERSION)"; fi; \
	if [[ -n "$(TS3_CLIENT_URL)" ]]; then export TS3_CLIENT_URL="$(TS3_CLIENT_URL)"; fi; \
	if [[ -n "$(TS3_CLIENT_SHA256)" ]]; then export TS3_CLIENT_SHA256="$(TS3_CLIENT_SHA256)"; fi; \
	if [[ -n "$(TS3_XDOTOOL)" ]]; then export TS3_XDOTOOL="$(TS3_XDOTOOL)"; fi; \
	if [[ -n "$(TS3_XDOTOOL_LIBRARY_PATH)" ]]; then export TS3_XDOTOOL_LIBRARY_PATH="$(TS3_XDOTOOL_LIBRARY_PATH)"; fi
endef

define require_env_state
	@if [[ ! -f "$(ENV_STATE_REF)" ]]; then \
		echo "No tracked TeamSpeak environment. Run 'make env-up' first."; \
		exit 1; \
	fi
	@state_file="$$(cat "$(ENV_STATE_REF)")"; \
	if [[ -z "$$state_file" || ! -f "$$state_file" ]]; then \
		echo "Tracked TeamSpeak environment state is missing. Run 'make env-up' again."; \
		exit 1; \
	fi
endef

.PHONY: help configure build test test-e2e test-all ts clean deps package-release \
	configure-built-test build-built-test test-built-test ts-built-test \
	env-up env-info env-ts env-down

help: ## Show available development commands
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {printf "%-22s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

deps: ## Download or refresh the managed TeamSpeak dependencies under ./third_party/teamspeak/managed
	$(bootstrap_runtime_deps)

configure: deps ## Configure the default TeamSpeak-backed build in ./build
	@$(load_runtime_deps_env); \
	"$(CMAKE)" -S . -B "$(BUILD_DIR)" $(CMAKE_BASE_ARGS) \
		-DTS_ENABLE_TS3_PLUGIN=ON \
		-DTS_ENABLE_TS3_E2E=ON \
		-DTS3_MANAGED_DIR="$(TS3_MANAGED_DIR)" \
		$${TS3_PLUGIN_SDK_DIR:+-DTS3_PLUGIN_SDK_DIR=$${TS3_PLUGIN_SDK_DIR}} \
		$${TS3_PLUGIN_SDK_INCLUDE_DIR:+-DTS3_PLUGIN_SDK_INCLUDE_DIR=$${TS3_PLUGIN_SDK_INCLUDE_DIR}} \
		$${TS3_CLIENT_DIR:+-DTS3_CLIENT_DIR=$${TS3_CLIENT_DIR}} \
		$${TS3_CLIENT_VERSION:+-DTS3_CLIENT_VERSION=$${TS3_CLIENT_VERSION}} \
		$${TS3_CLIENT_URL:+-DTS3_CLIENT_URL=$${TS3_CLIENT_URL}} \
		$${TS3_CLIENT_SHA256:+-DTS3_CLIENT_SHA256=$${TS3_CLIENT_SHA256}} \
		$${TS3_XDOTOOL:+-DTS3_XDOTOOL=$${TS3_XDOTOOL}} \
		$${TS3_XDOTOOL_LIBRARY_PATH:+-DTS3_XDOTOOL_LIBRARY_PATH=$${TS3_XDOTOOL_LIBRARY_PATH}}

build: configure ## Build the default TeamSpeak-backed tree
	$(CMAKE) --build $(BUILD_DIR)

test: build ## Run the default automated suite without the Docker/Xvfb E2E case
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -E ts_plugin_server_e2e_test

test-e2e: build ## Run the TeamSpeak-backed Docker/Xvfb end-to-end test
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -R ts_plugin_server_e2e_test

test-all: build ## Run the full default build tree test suite, including the TeamSpeak-backed E2E case
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

package-release: ## Build a release tarball under ./dist (set PACKAGE_VERSION=vX.Y.Z to control the asset name)
	PACKAGE_VERSION="$(PACKAGE_VERSION)" ./scripts/package-release.sh

ts: build ## Run the CLI from ./build with ARGS='...'
	"$(TS_BIN)" $(ARGS)

configure-built-test: ## Configure the fake/offline built-test tree in ./build-built-test
	$(CMAKE) -S . -B $(BUILT_TEST_BUILD_DIR) $(CMAKE_BASE_ARGS)

build-built-test: configure-built-test ## Build the fake/offline built-test tree
	$(CMAKE) --build $(BUILT_TEST_BUILD_DIR)

test-built-test: build-built-test ## Run the fake/offline built-test suite
	$(CTEST) --test-dir $(BUILT_TEST_BUILD_DIR) --output-on-failure

ts-built-test: build-built-test ## Run the CLI from ./build-built-test with ARGS='...'
	"$(BUILT_TEST_TS_BIN)" $(ARGS)

env-up: build ## Start the TeamSpeak-backed environment and track its state
	@mkdir -p "$(TS3_MANAGED_DIR)"
	@rm -f "$(ENV_STATE_REF)" "$(ENV_OUTPUT_REF)"
	@set -euo pipefail; \
	./tests/e2e/run_plugin_server_env.sh "$(TS_BIN)" "$(PLUGIN_SO)" | tee "$(ENV_OUTPUT_REF)"; \
	state_file="$$(sed -n 's/^State file:[[:space:]]*//p' "$(ENV_OUTPUT_REF)" | head -n 1)"; \
	if [[ -z "$$state_file" ]]; then \
		echo "Failed to capture the TeamSpeak environment state file path"; \
		exit 1; \
	fi; \
	printf '%s\n' "$$state_file" > "$(ENV_STATE_REF)"; \
	echo "Tracked state file: $$state_file"

env-info: ## Show the tracked TeamSpeak environment details
	$(require_env_state)
	@source "$$(cat "$(ENV_STATE_REF)")"; \
	echo "State file: $$(cat "$(ENV_STATE_REF)")"; \
	echo "Config: $$config_path"; \
	echo "Socket: $$socket_path"; \
	echo "Server: $$server_host:$$server_port"; \
	echo "Display: $$display"; \
	echo "Client runtime: $$client_runtime_dir"; \
	echo "CLI: $$ts_bin"

env-ts: ## Run the built CLI against the tracked TeamSpeak environment with ARGS='...'
	$(require_env_state)
	@source "$$(cat "$(ENV_STATE_REF)")"; \
	"$$ts_bin" --config "$$config_path" $(ARGS)

env-down: ## Stop the tracked TeamSpeak environment
	$(require_env_state)
	@state_file="$$(cat "$(ENV_STATE_REF)")"; \
	./tests/e2e/stop_plugin_server_env.sh "$$state_file"; \
	rm -f "$(ENV_STATE_REF)" "$(ENV_OUTPUT_REF)"

clean: ## Remove generated build directories, tracked TeamSpeak environment pointers, and managed dependencies
	rm -rf $(BUILD_DIR) $(BUILT_TEST_BUILD_DIR) $(TS3_MANAGED_DIR)
