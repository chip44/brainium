DR_LIB := /opt/dynamorio-git/lib64/release

# cmake configure
dr/build/debug/CMakeCache.txt: dr/CMakeLists.txt
	cmake -S ./dr -B ./dr/build/debug

dr/build/release/CMakeCache.txt: dr/CMakeLists.txt
	cmake -S ./dr -B ./dr/build/release -DCMAKE_BUILD_TYPE=RelWithDebInfo

# build
build-debug: dr/build/debug/CMakeCache.txt
	cmake --build ./dr/build/debug --verbose
	cargo build

build-release: dr/build/release/CMakeCache.txt
	cmake --build ./dr/build/release --verbose
	cargo build --release

# run
run-debug: build-debug
	./dr/build/debug/launcher ./dr/build/debug/libclient.so "${CURDIR}/target/debug/brainium" /bin/foot

run-release: build-release
	LD_LIBRARY_PATH=$(DR_LIB) ./dr/build/release/launcher ./dr/build/release/libclient.so "${CURDIR}/target/release/brainium" /bin/foot

# test
test/main: test/main.c
	cc ./test/main.c -o ./test/main

test-debug: build-debug test/main
	./dr/build/debug/launcher ./dr/build/debug/libclient.so "${CURDIR}/target/debug/brainium" "${CURDIR}/test/main"

test-release: build-release test/main
	LD_LIBRARY_PATH=$(DR_LIB) ./dr/build/release/launcher ./dr/build/release/libclient.so "${CURDIR}/target/release/brainium" "${CURDIR}/test/main"

# clean
clean:
	rm -rf ./dr/build ./test/main
	cargo clean

# shortcuts
all: build
build: build-debug
run: run-debug
test: test-debug

.PHONY: all build build-debug build-release run run-debug run-release test test-debug test-release clean
