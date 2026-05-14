# Makefile for minicontainer - Minimal Container Runtime
# Phase 7a: Execution-Core Consolidation
# (builds on Phase 6 network, Phase 5 cgroups, 4c IPC, 4b User, 4 UTS)

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
INCLUDES = -I./include
LDFLAGS  =
DEPFLAGS = -MMD -MP

# Directories
SRC_DIR   = src
INC_DIR   = include
TEST_DIR  = tests
BUILD_DIR = build

# Source and object files (auto-discovered)
SRC_FILES  = $(wildcard $(SRC_DIR)/*.c)
TEST_FILES = $(wildcard $(TEST_DIR)/*.c)
SRC_OBJS   = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_OBJS  = $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%.o,$(TEST_FILES))
DEPS       = $(SRC_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Phase 7a: every test links against the same helper chain as $(MINICONTAINER)
# minus main.o, plus the test's own .o. Capture once for reuse.
HELPER_OBJS = $(BUILD_DIR)/core.o $(BUILD_DIR)/env.o \
              $(BUILD_DIR)/net.o $(BUILD_DIR)/cgroup.o \
              $(BUILD_DIR)/uts.o $(BUILD_DIR)/overlay.o \
              $(BUILD_DIR)/mount.o

# Executables
MINICONTAINER  = minicontainer
TEST_CORE      = test_core
TEST_MOUNT     = test_mount
TEST_OVERLAY   = test_overlay
TEST_UTS       = test_uts
TEST_CGROUP    = test_cgroup
TEST_NET       = test_net

# Default target
.PHONY: all
all: $(MINICONTAINER)

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Pattern rules: compile src/*.c and tests/*.c -> build/*.o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: $(TEST_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# Phase 7a: link main.o + the unified helper chain.
$(MINICONTAINER): $(BUILD_DIR)/main.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(MINICONTAINER) successfully!"

# Phase 7a: every test links against the same helper chain. This is the
# core change Phase 7a's consolidation enables — one link set instead of
# six per-phase variations.

# Link test_core (Phase 7a: bare_exec + pid_only, replaces test_spawn + test_namespace)
$(TEST_CORE): $(BUILD_DIR)/test_core.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_CORE) successfully!"

# Link test_mount (Phase 2 retained, calls container_exec since Phase 7a)
$(TEST_MOUNT): $(BUILD_DIR)/test_mount.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_MOUNT) successfully!"

# Link test_overlay (Phase 3 retained, calls container_exec since Phase 7a)
$(TEST_OVERLAY): $(BUILD_DIR)/test_overlay.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_OVERLAY) successfully!"

# Link test_uts (Phase 4/4b/4c retained, calls container_exec since Phase 7a)
$(TEST_UTS): $(BUILD_DIR)/test_uts.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_UTS) successfully!"

# Link test_cgroup (Phase 5 retained, calls container_exec since Phase 7a)
$(TEST_CGROUP): $(BUILD_DIR)/test_cgroup.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_CGROUP) successfully!"

# Link test_net (Phase 6 retained, calls container_exec since Phase 7a)
$(TEST_NET): $(BUILD_DIR)/test_net.o $(HELPER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_NET) successfully!"

# Build and run all tests
.PHONY: test
test: $(TEST_CORE) $(TEST_MOUNT) $(TEST_OVERLAY) $(TEST_UTS) $(TEST_CGROUP) $(TEST_NET)
	@echo "=== Running Phase 7a core tests (bare_exec + pid_only, requires root) ==="
	sudo ./$(TEST_CORE)
	@echo ""
	@echo "=== Running Phase 2 mount tests (requires root + rootfs) ==="
	sudo ./$(TEST_MOUNT)
	@echo ""
	@echo "=== Running Phase 3 overlay tests (requires root + rootfs) ==="
	sudo ./$(TEST_OVERLAY)
	@echo ""
	@echo "=== Running Phase 4 UTS tests (hostname isolation, requires root) ==="
	sudo ./$(TEST_UTS)
	@echo ""
	@echo "=== Running Phase 4b tests (user namespace, runs unprivileged subset too) ==="
	./$(TEST_UTS)
	@echo ""
	@echo "=== Phase 4c IPC namespace tests are included in the test_uts suite above ==="
	@echo ""
	@echo "=== Running Phase 5 cgroup tests (requires root) ==="
	sudo ./$(TEST_CGROUP)
	@echo ""
	@echo "=== Running Phase 6 network tests (requires root + iproute2) ==="
	sudo ./$(TEST_NET)

# Build with debug symbols
.PHONY: debug
debug: CFLAGS += -g -DDEBUG
debug: clean all

# Run with valgrind for memory leak detection
.PHONY: valgrind
valgrind: debug
	@echo "Running valgrind memory check..."
	valgrind --leak-check=full --show-leak-kinds=all \
		--track-origins=yes ./$(MINICONTAINER) /bin/echo "Memory test"

# AppArmor profile (Ubuntu 24.04+ rootless)
# ----------------------------------------
# Ubuntu 24.04 sets kernel.apparmor_restrict_unprivileged_userns=1, which
# auto-attaches a restrictive default profile to unprivileged user
# namespaces created by unconfined processes. That profile masks
# CAP_SYS_ADMIN inside the new userns and breaks the rootless flow
# (sethostname: Operation not permitted, etc.). Shipping a per-binary
# profile that declares `userns,` is what real container runtimes
# (Docker, Podman, Chromium, Firefox) do — see decisions.md Error #19.
APPARMOR_PROFILE_NAME = minicontainer
APPARMOR_TEMPLATE     = scripts/apparmor/$(APPARMOR_PROFILE_NAME).profile.in
APPARMOR_RENDERED     = $(BUILD_DIR)/$(APPARMOR_PROFILE_NAME)
APPARMOR_INSTALLED    = /etc/apparmor.d/$(APPARMOR_PROFILE_NAME)

.PHONY: install-apparmor
install-apparmor: $(MINICONTAINER) | $(BUILD_DIR)
	@if [ ! -f $(APPARMOR_TEMPLATE) ]; then \
		echo "Error: $(APPARMOR_TEMPLATE) not found"; exit 1; \
	fi
	@if ! command -v apparmor_parser >/dev/null 2>&1; then \
		echo "Error: apparmor_parser not found."; \
		echo "On Ubuntu/Debian: sudo apt-get install apparmor-utils"; \
		exit 1; \
	fi
	@MC_PATH="$$(readlink -f $(MINICONTAINER))"; \
	echo "Rendering profile for binary at: $$MC_PATH"; \
	sed "s|@MINICONTAINER_PATH@|$$MC_PATH|g" $(APPARMOR_TEMPLATE) > $(APPARMOR_RENDERED)
	@echo "Installing $(APPARMOR_RENDERED) -> $(APPARMOR_INSTALLED) (requires sudo)"
	sudo install -m 0644 $(APPARMOR_RENDERED) $(APPARMOR_INSTALLED)
	sudo apparmor_parser -r $(APPARMOR_INSTALLED)
	@echo ""
	@echo "AppArmor profile installed. Rootless minicontainer should now"
	@echo "work in any shell. Verify with:"
	@echo "  ./$(MINICONTAINER) --user --pid --hostname test /bin/sh -c 'hostname'"
	@echo ""
	@echo "If you move or rename the binary, re-run 'make install-apparmor'"
	@echo "so the profile's path matches."

.PHONY: uninstall-apparmor
uninstall-apparmor:
	@if [ -f $(APPARMOR_INSTALLED) ]; then \
		echo "Unloading and removing $(APPARMOR_INSTALLED) (requires sudo)"; \
		sudo apparmor_parser -R $(APPARMOR_INSTALLED) || true; \
		sudo rm -f $(APPARMOR_INSTALLED); \
		echo "Profile removed."; \
	else \
		echo "No installed profile at $(APPARMOR_INSTALLED)"; \
	fi

# Clean build artifacts
.PHONY: clean
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f $(MINICONTAINER) $(TEST_CORE) $(TEST_MOUNT) $(TEST_OVERLAY) $(TEST_UTS) $(TEST_CGROUP) $(TEST_NET)
	@echo "Cleaned build artifacts"

# Run example commands
.PHONY: examples
examples: $(MINICONTAINER)
	@echo "=== Example 1: Basic execution ==="
	./$(MINICONTAINER) /bin/ls -la
	@echo ""
	@echo "=== Example 2: Debug mode ==="
	./$(MINICONTAINER) --debug /bin/echo "Hello, minicontainer!"
	@echo ""
	@echo "=== Example 3: PID namespace (requires root) ==="
	sudo ./$(MINICONTAINER) --pid /bin/sh -c 'echo PID: $$$$'
	@echo ""
	@echo "=== Example 4: Rootfs isolation (requires root + rootfs) ==="
	sudo ./$(MINICONTAINER) --rootfs ./rootfs /bin/ls /
	@echo ""
	@echo "=== Example 5: Full isolation (requires root + rootfs) ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs /bin/sh -c 'echo PID: $$$$ && ls /'
	@echo ""
	@echo "=== Example 6: Debug with full isolation ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --debug /bin/sh -c 'echo PID: $$$$'
	@echo ""
	@echo "=== Example 7: Overlay filesystem (requires root + rootfs) ==="
	sudo ./$(MINICONTAINER) --rootfs ./rootfs --overlay /bin/sh -c 'echo hello > /tmp/test && cat /tmp/test'
	@echo ""
	@echo "=== Example 8: Overlay with debug ==="
	sudo ./$(MINICONTAINER) --rootfs ./rootfs --overlay --debug /bin/ls /
	@echo ""
	@echo "=== Example 9: Hostname isolation (requires root + rootfs) ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --hostname mycontainer /bin/sh -c 'hostname'
	@echo ""
	@echo "=== Example 10: Full isolation with hostname ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --overlay --hostname mycontainer /bin/sh -c 'hostname && echo PID: $$$$'
	@echo ""
	@echo "=== Example 11: Rootless container — no sudo (Phase 4b) ==="
	./$(MINICONTAINER) --user --pid --hostname mycontainer /bin/sh -c 'id && hostname'
	@echo ""
	@echo "=== Example 12: IPC namespace isolation (Phase 4c, requires root) ==="
	sudo ./$(MINICONTAINER) --pid --ipc /bin/sh -c 'ipcs'
	@echo ""
	@echo "=== Example 13: Rootless with IPC isolation (Phase 4b + 4c) ==="
	./$(MINICONTAINER) --user --pid --ipc --hostname test /bin/sh -c 'ipcs; id'
	@echo ""
	@echo "=== Example 14: Memory limit (Phase 5, requires root) ==="
	sudo ./$(MINICONTAINER) --pid --memory 100M /bin/sh -c 'head -c 50M /dev/zero | wc -c'
	@echo ""
	@echo "=== Example 15: CPU + PID limits (Phase 5, requires root) ==="
	sudo ./$(MINICONTAINER) --pid --cpus 0.5 --pids 20 /bin/sh -c 'echo limited'
	@echo ""
	@echo "=== Example 16: Full isolation with cgroups ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --overlay --hostname web --ipc \
	    --memory 100M --cpus 0.5 --pids 20 /bin/sh -c 'hostname && id'
	@echo ""
	@echo "=== Example 17: Network namespace (Phase 6, requires root) ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --net /bin/sh -c 'ip addr show'
	@echo ""
	@echo "=== Example 18: Network with custom subnet (Phase 6) ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --net \
	    --net-host-ip 10.42.0.1 --net-container-ip 10.42.0.2 --net-netmask 30 \
	    /bin/sh -c 'ip addr show && ip route'
	@echo ""
	@echo "=== Example 19: Network without NAT (Phase 6) ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --net --no-nat \
	    /bin/sh -c 'ping -c 1 10.0.0.1'
	@echo ""
	@echo "=== Example 20: Full stack — every isolation + network ==="
	sudo ./$(MINICONTAINER) --pid --rootfs ./rootfs --overlay --hostname web --ipc \
	    --memory 100M --cpus 0.5 --pids 20 --net \
	    /bin/sh -c 'hostname && id && ip addr show && ipcs'

# Help target
.PHONY: help
help:
	@echo "Makefile for minicontainer - Minimal Container Runtime (Phase 6)"
	@echo ""
	@echo "Available targets:"
	@echo "  all                 - Build minicontainer (default)"
	@echo "  test                - Build and run all tests (Phase 0 through Phase 6)"
	@echo "  debug               - Build with debug symbols (-g)"
	@echo "  valgrind            - Run with valgrind memory checker"
	@echo "  examples            - Run example commands"
	@echo "  install-apparmor    - Install AppArmor profile (Ubuntu 24.04+ rootless)"
	@echo "  uninstall-apparmor  - Remove the AppArmor profile"
	@echo "  clean               - Remove build artifacts"
	@echo "  help                - Show this help message"
	@echo ""
	@echo "Usage examples:"
	@echo "  make                  # Build minicontainer"
	@echo "  make test             # Run all tests"
	@echo "  make clean all        # Clean and rebuild"
	@echo "  sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh"
	@echo "  sudo ./minicontainer --rootfs ./rootfs --overlay /bin/sh"
	@echo "  sudo ./minicontainer --pid --rootfs ./rootfs --hostname mycontainer /bin/sh"
	@echo "  ./minicontainer --user --pid --hostname test /bin/sh                # Rootless"
	@echo "  sudo ./minicontainer --pid --ipc /bin/sh -c 'ipcs'                  # IPC isolated"
	@echo "  ./minicontainer --user --pid --ipc --hostname test /bin/sh          # Rootless + IPC"
	@echo "  sudo ./minicontainer --pid --memory 100M --cpus 0.5 /bin/sh         # cgroup limits (Phase 5)"
	@echo "  sudo ./minicontainer --pid --rootfs ./rootfs --net /bin/sh          # Network namespace (Phase 6)"

# Include auto-generated header dependencies
-include $(DEPS)

# Prevent make from deleting intermediate files
.PRECIOUS: $(BUILD_DIR)/%.o
