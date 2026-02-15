# Makefile for minicontainer - Minimal Container Runtime
# Phase 1: PID Namespace Isolation

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
INCLUDES = -I./include
LDFLAGS =

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BUILD_DIR = build

# Source files
MAIN_SRC = $(SRC_DIR)/main.c
NAMESPACE_SRC = $(SRC_DIR)/namespace.c
SPAWN_SRC = $(SRC_DIR)/spawn.c
TEST_SPAWN_SRC = $(TEST_DIR)/test_spawn.c
TEST_NAMESPACE_SRC = $(TEST_DIR)/test_namespace.c

# Object files
MAIN_OBJ = $(BUILD_DIR)/main.o
NAMESPACE_OBJ = $(BUILD_DIR)/namespace.o
SPAWN_OBJ = $(BUILD_DIR)/spawn.o
TEST_SPAWN_OBJ = $(BUILD_DIR)/test_spawn.o
TEST_NAMESPACE_OBJ = $(BUILD_DIR)/test_namespace.o

# Executables
MINICONTAINER = minicontainer
TEST_SPAWN = test_spawn
TEST_NAMESPACE = test_namespace

# Default target
.PHONY: all
all: $(MINICONTAINER)

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Compile main.o
$(MAIN_OBJ): $(MAIN_SRC) $(INC_DIR)/namespace.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile namespace.o
$(NAMESPACE_OBJ): $(NAMESPACE_SRC) $(INC_DIR)/namespace.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile spawn.o
$(SPAWN_OBJ): $(SPAWN_SRC) $(INC_DIR)/spawn.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile test_spawn.o
$(TEST_SPAWN_OBJ): $(TEST_SPAWN_SRC) $(INC_DIR)/spawn.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile test_namespace.o
$(TEST_NAMESPACE_OBJ): $(TEST_NAMESPACE_SRC) $(INC_DIR)/namespace.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Link minicontainer executable
$(MINICONTAINER): $(MAIN_OBJ) $(NAMESPACE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(MINICONTAINER) successfully!"

# Link test_spawn executable (Phase 0)
$(TEST_SPAWN): $(TEST_SPAWN_OBJ) $(SPAWN_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_SPAWN) successfully!"

# Link test_namespace executable (Phase 1)
$(TEST_NAMESPACE): $(TEST_NAMESPACE_OBJ) $(NAMESPACE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $(TEST_NAMESPACE) successfully!"

# Build and run all tests
.PHONY: test
test: $(TEST_SPAWN) $(TEST_NAMESPACE)
	@echo "=== Running Phase 0 spawn tests ==="
	./$(TEST_SPAWN)
	@echo ""
	@echo "=== Running Phase 1 namespace tests (requires root) ==="
	sudo ./$(TEST_NAMESPACE)

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
	@rm -f $(MINICONTAINER) $(TEST_SPAWN) $(TEST_NAMESPACE)
	@echo "Cleaned build artifacts"

# Run example commands
.PHONY: examples
examples: $(MINICONTAINER)
	@echo "=== Example 1: Basic execution ==="
	./$(MINICONTAINER) /bin/ls -la
	@echo "\n=== Example 2: Debug mode ==="
	./$(MINICONTAINER) --debug /bin/echo "Hello, minicontainer!"
	@echo "\n=== Example 3: Exit code test ==="
	./$(MINICONTAINER) /bin/false || echo "Exit code: $$?"
	@echo "\n=== Example 4: Environment variables ==="
	./$(MINICONTAINER) --env FOO=bar /bin/sh -c 'echo $$FOO'
	@echo "\n=== Example 5: PID namespace (requires root) ==="
	sudo ./$(MINICONTAINER) --pid /bin/sh -c 'echo $$$$'
	@echo "\n=== Example 6: PID namespace with debug ==="
	sudo ./$(MINICONTAINER) --pid --debug /bin/sh -c 'echo $$$$'

# Help target
.PHONY: help
help:
	@echo "Makefile for minicontainer - Minimal Container Runtime"
	@echo ""
	@echo "Available targets:"
	@echo "  all        - Build minicontainer (default)"
	@echo "  test       - Build and run all tests (Phase 0 + Phase 1)"
	@echo "  debug      - Build with debug symbols (-g)"
	@echo "  valgrind   - Run with valgrind memory checker"
	@echo "  examples   - Run example commands"
	@echo "  clean      - Remove build artifacts"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Usage examples:"
	@echo "  make                  # Build minicontainer"
	@echo "  make test             # Run tests"
	@echo "  make clean all        # Clean and rebuild"
	@echo "  ./minicontainer /bin/ls -la"

# Prevent make from deleting intermediate files
.PRECIOUS: $(BUILD_DIR)/%.o
