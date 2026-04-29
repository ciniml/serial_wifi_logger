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
        require-probe-host clean

build:
	bash -c "source $(IDF_EXPORTS) && idf.py build"

build-qemu:
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
qemu-bg: qemu-kill build-qemu
	@rm -f $(QEMU_LOG)
	@bash -c "nohup bash -c 'source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS=\"$(SDKCONFIG_QEMU)\" qemu' > $(QEMU_LOG) 2>&1 & disown"
	@echo "QEMU starting in background; log: $(QEMU_LOG)"
	@i=0; while [ $$i -lt $(QEMU_BOOT_TIMEOUT) ]; do \
		sleep 1; i=$$((i+1)); \
		if grep -q "DERP connected" $(QEMU_LOG) 2>/dev/null; then \
			echo "QEMU ready ($${i}s, DERP connected)"; \
			grep -E "$(STATUS_RE)" $(QEMU_LOG) | head -8; \
			exit 0; \
		fi; \
	done; \
	echo "WARNING: QEMU did not reach DERP-connected within $(QEMU_BOOT_TIMEOUT)s"; \
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
