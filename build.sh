#!/bin/sh
set -e

cmake -S ./dr -B ./dr/build
cmake --build ./dr/build

cargo build
