#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$PWD/.tools/rustup/toolchains/1.97.1-x86_64-unknown-linux-gnu/bin:/usr/local/bin:/usr/bin:/bin"
export CARGO_HOME="$PWD/.tools/cargo" CARGO_NET_OFFLINE=true CARGO_TARGET_DIR="$PWD/build/portable-target"
export CC=gcc-10 CXX=g++-10 CORDGEIST_RELOCATABLE=1
prefix="$PWD/build/portable-prefix"
cd build/portable-libusb-src
./autogen.sh --prefix="$prefix" --disable-udev --disable-static --enable-shared
make -j2
make install
cd "$CARGO_HOME/../.."
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig" LD_LIBRARY_PATH="$prefix/lib"
cmake -S vendor/libaoahid -B build/portable-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DAOAHID_BUILD_SHARED=ON -DAOAHID_BUILD_STATIC=OFF -DAOAHID_USE_FAKE_LIBUSB=OFF -DAOAHID_BUILD_TESTS=OFF -DCMAKE_INSTALL_RPATH='$ORIGIN'
cmake --build build/portable-native --parallel 2
cmake --install build/portable-native
export AOAHID_LIB_DIR="$prefix/lib"
cargo build --offline --locked --release --features native-aoa --bin cordgeist
