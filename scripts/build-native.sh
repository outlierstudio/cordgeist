#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
prefix=${CORDGEIST_NATIVE_PREFIX:-"$PWD/build/native-prefix"}
[[ $prefix = /* ]] || { echo 'CORDGEIST_NATIVE_PREFIX must be absolute' >&2; exit 2; }
pkg-config --atleast-version=1.0.30 libusb-1.0 || {
  echo 'Matching libusb >=1.0.30 development files required; use PKG_CONFIG_PATH for a user prefix.' >&2; exit 1;
}
mkdir -p build
# Probe headers AND the loaded runtime before building production artifacts.
cc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libusb-1.0) scripts/probe-libusb.c $(pkg-config --libs libusb-1.0) -o build/probe-libusb
./build/probe-libusb
cmake -S vendor/libaoahid -B build/native-real -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib -DAOAHID_BUILD_SHARED=ON -DAOAHID_BUILD_STATIC=OFF \
  -DAOAHID_USE_FAKE_LIBUSB=OFF -DAOAHID_BUILD_TESTS=OFF
cmake --build build/native-real --parallel 2
cmake --install build/native-real
