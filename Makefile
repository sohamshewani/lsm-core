CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -MMD -MP
LDFLAGS = -pthread

TARGET_LIB = liblsm-core.a
TARGET_TEST = lsm-core-test
TARGET_BENCH = lsm-core-bench

SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BENCH_DIR = bench
BUILD_DIR = build

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SOURCES))
DEPS = $(OBJECTS:.o=.d)

TEST_SOURCES = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/tests/%.o, $(TEST_SOURCES))

BENCH_SOURCES = $(wildcard $(BENCH_DIR)/*.cpp)
BENCH_OBJECTS = $(patsubst $(BENCH_DIR)/%.cpp, $(BUILD_DIR)/bench/%.o, $(BENCH_SOURCES))

INCLUDES = -I$(INC_DIR)

.PHONY: all build test clean benchmark directories

all: directories build $(TARGET_TEST) $(TARGET_BENCH)

directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/tests
	@mkdir -p $(BUILD_DIR)/bench

build: directories $(BUILD_DIR)/$(TARGET_LIB)

$(BUILD_DIR)/$(TARGET_LIB): $(OBJECTS)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/bench/%.o: $(BENCH_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET_TEST): $(TEST_OBJECTS) $(BUILD_DIR)/$(TARGET_LIB)
	if [ -n "$(TEST_OBJECTS)" ]; then \
		$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS); \
	else \
		touch $@; \
	fi

$(TARGET_BENCH): $(BENCH_OBJECTS) $(BUILD_DIR)/$(TARGET_LIB)
	if [ -n "$(BENCH_OBJECTS)" ]; then \
		$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS); \
	else \
		touch $@; \
	fi

test: $(TARGET_TEST)
	@if [ -x "$(TARGET_TEST)" ] && [ -s "$(TARGET_TEST)" ]; then \
		./$(TARGET_TEST); \
	else \
		echo "No tests found or test binary is empty."; \
	fi

benchmark: $(TARGET_BENCH)
	@if [ -x "$(TARGET_BENCH)" ] && [ -s "$(TARGET_BENCH)" ]; then \
		./$(TARGET_BENCH); \
	else \
		echo "No benchmarks found or benchmark binary is empty."; \
	fi

clean:
	rm -rf $(BUILD_DIR) $(TARGET_TEST) $(TARGET_BENCH)

-include $(DEPS)