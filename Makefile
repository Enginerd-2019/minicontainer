# Makefile for minicontainer - Minimal Container Runtime
# Phase 2: Mount Namespace & Filesystem Isolation

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

# Executables
MINICONTAINER  = minicontainer
TEST_SPAWN     = test_spawn
TEST_NAMESPACE = test_namespace
TEST_MOUNT     = test_mount

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

# Link minicontainer (Phase 2: main + mount)
$(MINICONTAINER): $(BUILD_DIR)/main.o $(BUILD_DIR)/mount.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(MINICONTAINER) successfully!"

# Link test_spawn (Phase 0)
$(TEST_SPAWN): $(BUILD_DIR)/test_spawn.o $(BUILD_DIR)/spawn.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_SPAWN) successfully!"

# Link test_namespace (Phase 1)
$(TEST_NAMESPACE): $(BUILD_DIR)/test_namespace.o $(BUILD_DIR)/namespace.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_NAMESPACE) successfully!"

# Link test_mount (Phase 2)
$(TEST_MOUNT): $(BUILD_DIR)/test_mount.o $(BUILD_DIR)/mount.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_MOUNT) successfully!"

# Build and run all tests
.PHONY: test
test: $(TEST_SPAWN) $(TEST_NAMESPACE) $(TEST_MOUNT)
	@echo "=== Running Phase 0 spawn tests ==="
	./$(TEST_SPAWN)
	@echo ""
	@echo "=== Running Phase 1 namespace tests (requires root) ==="
	sudo ./$(TEST_NAMESPACE)
	@echo ""
	@echo "=== Running Phase 2 mount tests (requires root + rootfs) ==="
	sudo ./$(TEST_MOUNT)

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

# Clean build artifacts
.PHONY: clean
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f $(MINICONTAINER) $(TEST_SPAWN) $(TEST_NAMESPACE) $(TEST_MOUNT)
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

# Help target
.PHONY: help
help:
	@echo "Makefile for minicontainer - Minimal Container Runtime (Phase 2)"
	@echo ""
	@echo "Available targets:"
	@echo "  all        - Build minicontainer (default)"
	@echo "  test       - Build and run all tests (Phase 0 + 1 + 2)"
	@echo "  debug      - Build with debug symbols (-g)"
	@echo "  valgrind   - Run with valgrind memory checker"
	@echo "  examples   - Run example commands"
	@echo "  clean      - Remove build artifacts"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Usage examples:"
	@echo "  make                  # Build minicontainer"
	@echo "  make test             # Run all tests"
	@echo "  make clean all        # Clean and rebuild"
	@echo "  sudo ./minicontainer --pid --rootfs ./rootfs /bin/sh"

# Include auto-generated header dependencies
-include $(DEPS)

# Prevent make from deleting intermediate files
.PRECIOUS: $(BUILD_DIR)/%.o
