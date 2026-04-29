IDF_PATH    ?= $(HOME)/esp-idf/6.0
IDF_EXPORTS  = $(IDF_PATH)/export.sh
SDKCONFIG_QEMU = sdkconfig.defaults;sdkconfig.qemu

# QEMU run / Tailscale-test workflow defaults.
# PROBE_HOST is the Tailscale IP assigned to the QEMU instance and is
# auth-key specific; the test-* / probe-* targets refuse to run unless
# you set it explicitly, e.g.:
#     make test-ping PROBE_HOST=100.x.y.z
# A persistent default can be set in a Makefile.local (gitignored).
QEMU_LOG    ?= /tmp/qemu_live.log
PROBE_HOST  ?=
PROBE_PORT  ?= 8888
QEMU_BOOT_TIMEOUT ?= 25
EVENTS_RE   ?= (inject_wg|HANDSHAKE|TRANSPORT|ts_derp_send|wg_output|rx_data|panic|abort|stack overflow|Guru)
STATUS_RE   ?= (Tailscale IP|DERP connected|Added peer|MapResponse keys|PeersChanged)

# Allow developers to keep their PROBE_HOST etc. in an untracked file.
-include Makefile.local

# Internal guard used by test-* / probe-* targets.
require-probe-host:
	@if [ -z "$(PROBE_HOST)" ]; then \
		echo "error: PROBE_HOST is not set."; \
		echo "       Set it inline (\`make test-ping PROBE_HOST=100.x.y.z\`)"; \
		echo "       or persistently in a Makefile.local."; \
		exit 1; \
	fi

.PHONY: build build-qemu qemu qemu-kill qemu-bg qemu-events qemu-status \
        test-ping test-http test-tcp test-tsping \
        probe-ping probe-http probe-tcp probe-tsping \
        require-probe-host clean \
        build-docker docker-shell docker-clean

# Track which defaults set generated the current sdkconfig, so we
# automatically regenerate it when switching between HW and QEMU builds
# (otherwise stale, mode-specific settings persist in sdkconfig).
BUILD_MODE_FILE = build/.build_mode

# $(call switch-mode,<label>) deletes sdkconfig if the previous build was for
# a different mode. sdkconfig is gitignored and regeneratable from
# sdkconfig.defaults / sdkconfig.qemu / Makefile.local.
define switch-mode
	@mkdir -p build
	@if [ "$$(cat $(BUILD_MODE_FILE) 2>/dev/null)" != "$(1)" ]; then \
		echo "switch-mode: $$(cat $(BUILD_MODE_FILE) 2>/dev/null || echo none) -> $(1); regenerating sdkconfig"; \
		rm -f sdkconfig; \
		echo "$(1)" > $(BUILD_MODE_FILE); \
	fi
endef

build:
	$(call switch-mode,hw)
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.local' build"

build-qemu:
	$(call switch-mode,qemu)
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_QEMU)' build"

# Foreground QEMU (interactive console).
qemu: build-qemu
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_QEMU)' qemu"

# Kill any running QEMU and wrapping idf.py qemu processes (started from `make qemu`).
# Patterns anchored to avoid matching `make qemu-kill` itself.
qemu-kill:
	-pkill -f qemu-system 2>/dev/null || true
	-pkill -f "idf.py.*qemu$$" 2>/dev/null || true
	-pkill -f "make qemu$$" 2>/dev/null || true
	@sleep 1
	@pgrep -af "qemu-system" >/dev/null && echo "WARNING: some QEMU processes still running" || echo "QEMU stopped."

# Boot QEMU in the background, redirect output to $(QEMU_LOG), and wait until
# the device reaches DERP-connected state (or timeout). Idempotent: kills any
# previous QEMU first and rebuilds.
qemu-bg: qemu-kill build-qemu qemu-bg-norebuild

# Like qemu-bg but skips the rebuild, so NVS / OTA partition persist across
# restarts. Use this to verify config persistence after a settings change.
.PHONY: qemu-bg-norebuild
qemu-bg-norebuild:
	@$(MAKE) -s qemu-kill
	@rm -f $(QEMU_LOG)
	@bash -c "nohup bash -c 'source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS=\"$(SDKCONFIG_QEMU)\" qemu' > $(QEMU_LOG) 2>&1 & disown"
	@echo "QEMU starting in background; log: $(QEMU_LOG)"
	@i=0; while [ $$i -lt $(QEMU_BOOT_TIMEOUT) ]; do \
		sleep 1; i=$$((i+1)); \
		if grep -q "DERP connected\|VPN services disabled" $(QEMU_LOG) 2>/dev/null; then \
			echo "QEMU ready (boot complete after $${i}s)"; \
			grep -E "$(STATUS_RE)|VPN services" $(QEMU_LOG) | head -8; \
			exit 0; \
		fi; \
	done; \
	echo "WARNING: QEMU did not reach a known-ready state within $(QEMU_BOOT_TIMEOUT)s"; \
	tail -10 $(QEMU_LOG); exit 1

# Show ESP32 status / event lines from the running QEMU log.
qemu-status:
	@grep -E "$(STATUS_RE)" $(QEMU_LOG) | head -10 || true

qemu-events:
	@grep -E "$(EVENTS_RE)" $(QEMU_LOG) | tail -30 || true

# --- Combined probe + log inspection targets ----------------------------
# Each target boots QEMU (qemu-bg dependency), runs a probe from the host,
# and prints the matching ESP32 event lines. Override PROBE_HOST / PROBE_PORT
# from the command line, e.g. `make test-tcp PROBE_PORT=80`.

test-ping: require-probe-host qemu-bg
	@echo "--- ICMP ping $(PROBE_HOST) ---"
	-timeout 12 ping -c 5 -W 3 $(PROBE_HOST) || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

test-tsping: require-probe-host qemu-bg
	@echo "--- tailscale ping $(PROBE_HOST) (DERP allowed) ---"
	-timeout 15 tailscale ping --until-direct=false -c 3 $(PROBE_HOST) || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

test-http: require-probe-host qemu-bg
	@echo "--- HTTP GET http://$(PROBE_HOST)/ ---"
	-timeout 15 curl -m 10 -sS -o /tmp/esp_index.html \
		-w "HTTP %{http_code} time=%{time_total}s size=%{size_download}\n" \
		http://$(PROBE_HOST)/ || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

test-tcp: require-probe-host qemu-bg
	@echo "--- TCP connect $(PROBE_HOST):$(PROBE_PORT) ---"
	-timeout 8 bash -c 'echo "hello" | nc -w 5 $(PROBE_HOST) $(PROBE_PORT)' || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

# probe-* targets: re-run a probe AGAINST an existing running QEMU. Use after
# test-ping to test additional protocols on the same WG session (avoids the
# tailscaled-cache stale-state issue we get from a cold reboot).
probe-ping: require-probe-host
	@echo "--- ICMP ping $(PROBE_HOST) ---"
	-timeout 12 ping -c 5 -W 3 $(PROBE_HOST) || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

probe-tsping: require-probe-host
	@echo "--- tailscale ping $(PROBE_HOST) (DERP allowed) ---"
	-timeout 15 tailscale ping --until-direct=false -c 3 $(PROBE_HOST) || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

probe-http: require-probe-host
	@echo "--- HTTP GET http://$(PROBE_HOST)/ ---"
	-timeout 15 curl -m 10 -sS -o /tmp/esp_index.html \
		-w "HTTP %{http_code} time=%{time_total}s size=%{size_download}\n" \
		http://$(PROBE_HOST)/ || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

probe-tcp: require-probe-host
	@echo "--- TCP connect $(PROBE_HOST):$(PROBE_PORT) ---"
	-timeout 8 bash -c 'echo "hello" | nc -w 5 $(PROBE_HOST) $(PROBE_PORT)' || true
	@echo "--- ESP32 events ---"
	@$(MAKE) -s qemu-events

clean:
	bash -c "source $(IDF_EXPORTS) && idf.py fullclean"

# --- Docker build (mirrors CI) -----------------------------------------
# Builds the firmware inside the same image GitHub Actions uses
# (espressif/idf:release-v6.0). Runs as the calling user/group so build
# artifacts are not owned by root, and uses an isolated build dir +
# sdkconfig so it does not clash with host-side `make build` / `make qemu`.
DOCKER_IMAGE         ?= espressif/idf:release-v6.0
DOCKER_WORKDIR       ?= /work
DOCKER_BUILD_DIR     ?= build-docker
DOCKER_SDKCONFIG     ?= sdkconfig.docker
DOCKER_TARGET_CHIP   ?= esp32s3

# Run a command inside the IDF Docker image.
# - --user UID:GID: own the artifacts on the host
# - HOME=/tmp: idf-component-manager / pip caches need a writable HOME
#   and the user we map in has no entry under /home in the image
# - safe.directory: avoid git's "dubious ownership" refusal under bind mounts
define docker-run
	docker run --rm -t \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		-e CI=true \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			$(1)'
endef

build-docker:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
		       set-target $(DOCKER_TARGET_CHIP) && \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build)

docker-clean:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       fullclean)
	@rm -f $(DOCKER_SDKCONFIG)

# Drop into an interactive shell in the IDF container with the IDF env set up.
# Useful for ad-hoc invocation (e.g. `idf.py menuconfig`, `idf.py size`).
docker-shell:
	docker run --rm -it \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			exec bash'
