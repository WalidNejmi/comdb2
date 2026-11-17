CMAKE ?= cmake
BUILD_DIR ?= build

.PHONY: all check clean
all:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=RelWithDebInfo $(CMAKE_ARGS)
	+$(CMAKE) --build "$(BUILD_DIR)" --target comdb2_microbenchmarks

check: all
	+$(CMAKE) --build "$(BUILD_DIR)" --target benchmark_failure_test
	$(CMAKE) --build "$(BUILD_DIR)" --target test

clean:
	$(CMAKE) --build "$(BUILD_DIR)" --target clean
