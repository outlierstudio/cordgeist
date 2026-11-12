# Local glibc 2.31 build

The measured build uses a locally prepared Ubuntu 20.04 image named
`cordgeist-v1-glibc231`, GCC/G++ 10.5, CMake 3.31.10, Ninja, pkg-config,
autoconf/automake/libtool and the installed Rust 1.97.1 toolchain. Dependency
acquisition is a separate explicit step. The build script never fetches.

libusb source: https://github.com/libusb/libusb at
87a55632db62c9bdc58cd31d3ccfa673f1bb017f (v1.0.30), LGPL-2.1-or-later.
Extract its complete source into build/portable-libusb-src before running:

    docker run --rm --user "$(id -u):$(id -g)" \
      -v "$PWD:$PWD:ro" -v "$PWD/build:$PWD/build" \
      -v "$PWD/.tools/cargo:$PWD/.tools/cargo" -w "$PWD" \
      cordgeist-v1-glibc231 bash scripts/portable-build-inner.sh

Output is build/portable-target/release/cordgeist and build/portable-prefix/lib.
libusb is built shared, without libudev; libaoahid remains shared and pinned.
Ship corresponding libusb source, COPYING, copyright notices and this build
recipe with bundled libusb. Replace lib/libusb-1.0.so.0 with a compatible modified
shared library to use modifications; no license enforcement prevents replacement
or debugging modifications. Source for pinned libaoahid remains in vendor/.
Do not bundle host libc or silently lower libusb's >=1.0.30 requirement.

Inspect every executable/library with objdump, and actually run the staged
wrapper in Ubuntu 22.04. A no-phone doctor exit 2 proves loader/diagnostic behavior,
not phone compatibility. Earlier measurement scope is summarized in docs/measurements.md.

The portable build remaps checkout/cache locations in compiled source paths.
Release tar headers use numeric root ownership rather than the build user's
name. Check both file contents and ELF strings in each newly extracted archive;
inspect nested corresponding-source archives too. These privacy controls do not
replace license notices or source obligations.
