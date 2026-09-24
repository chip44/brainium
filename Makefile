all: build

configure:
	cmake -S ./dr -B ./dr/build

build: configure
	cmake --build ./dr/build
	cargo build

run: build
	./dr/build/launcher ./dr/build/libclient.so "${PWD}/target/debug/brainium" /bin/ls

clean:
	rm -rf ./dr/build ./target

.PHONY: all configure build run clean
