#!/bin/sh
set -e

cmake -S ./client -B ./client/build
cmake --build ./client/build

cmake -S ./launcher -B ./launcher/build
cmake --build ./launcher/build

cargo build
