# Thin wrapper over CMake, so the common workflows stay one word long.
#
#   make          configure + build
#   make test     build and run the test suite
#   make bench    QSBR vs atomic<shared_ptr> measurement
#   make asan     rebuild under AddressSanitizer + UBSan and run tests
#   make tsan     rebuild under ThreadSanitizer and run tests
#   make clean

BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || echo 2)

.PHONY: all configure test bench asan tsan clean help

all: configure
	@cmake --build $(BUILD_DIR) -j $(JOBS)

configure:
	@cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null

test: all
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: all
	@$(BUILD_DIR)/bench_snapshot

asan:
	@cmake -S . -B $(BUILD_DIR)-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON > /dev/null
	@cmake --build $(BUILD_DIR)-asan -j $(JOBS)
	@ctest --test-dir $(BUILD_DIR)-asan --output-on-failure

tsan:
	@cmake -S . -B $(BUILD_DIR)-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON > /dev/null
	@cmake --build $(BUILD_DIR)-tsan -j $(JOBS)
	@ctest --test-dir $(BUILD_DIR)-tsan --output-on-failure

clean:
	@rm -rf $(BUILD_DIR) $(BUILD_DIR)-asan $(BUILD_DIR)-tsan

help:
	@sed -n '3,10p' Makefile
