# Third-party notices

`libaoahid` uses libusb 1.0 at runtime. libusb is distributed separately under
LGPL-2.1-or-later. Windows release archives that contain `libusb-1.0.dll`, and
macOS archives that contain `libusb-1.0.0.dylib`, also contain the corresponding
libusb license and notices. Static libusb linkage, if
selected by a distributor, carries the LGPL relinking/source obligations; the
project release pipeline keeps libusb dynamically linked in both its shared and
static-libaoahid distributions (the `static` label describes libaoahid itself).

Every release asset that ships a libusb runtime binary carries libusb's license
and corresponding source in the same distribution unit. That rule covers the
complete `*-shared` and `*-static` archives and the smaller `*-runtime`
bundles, which contain the shared libraries, `LICENSE`, `NOTICE`, this file,
`third-party/libusb-copyright`, and `third-party/source/libusb-1.0.30.tar.bz2`.
The release pipeline publishes no standalone `.so`, `.dylib`, or `.dll` asset, because a
single loose binary cannot carry those obligations; `collect_release.py`
refuses to assemble a release whose runtime bundle is missing any of them.

Each binary archive contains the official `libusb-1.0.30.tar.bz2` source
archive (SHA-256
`fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`).
Linux and macOS archives build that source unmodified (macOS also sets the
install name to `@rpath/libusb-1.0.0.dylib` and signs it ad hoc); the exact `configure` options
are recorded as `libusb_build` in `share/doc/libaoahid/build-metadata.json`.
Windows archives also contain the libusb port's exact `vcpkg.json` and
`portfile.cmake` from pinned vcpkg commit
`ddd0023b0eee70986e42ed49d9d4afb8098f212e`. No port patches exist at that
revision. The vcpkg-authored build material is MIT licensed by Microsoft; each
Windows archive includes that revision's exact 1,073-byte `LICENSE.txt` as
`share/doc/libaoahid/third-party/vcpkg-LICENSE.txt` (SHA-256
`1ee376fc340e0aa6ad6a3581c94126e741468705096ac92263048a21daa86460`).
These files are distribution materials; this notice does not replace the
license text shipped with libusb.

The deterministic test-only libusb substitute under `tests/fake_libusb/` and
the Windows macro fixture under `tests/cmake/windows_libusb_minmax_fixture/`
are independently written test doubles. They declare the subset of the libusb
1.0 interface the tests exercise; no libusb implementation source is copied
into this repository, and neither file is compiled into a release build.

Per-file licensing is machine-readable in two places. Every first-party source
file carries an `SPDX-License-Identifier: MIT` tag and copyright line, enforced
by `tools/check_license_headers.py`. The SPDX 2.3 document shipped with and
alongside each release states `licenseConcluded`, `licenseInfoInFiles`, and
`copyrightText` for every packaged file individually: `MIT` for first-party
output, `LGPL-2.1-or-later` for every file installed from libusb, and MIT with
Microsoft's copyright for the pinned vcpkg material. The libaoahid package
record therefore concludes `MIT AND LGPL-2.1-or-later` for the archive as a
whole while still declaring `MIT` for libaoahid itself.

No cited USB-IF, Android, Linux, Microsoft, or GitHub protocol/platform source
text is incorporated into the compiled libaoahid binary. Windows distributions
do include the separately identified Microsoft/vcpkg build-material files and
license under the MIT terms stated above. The remaining official documents and
source repositories are cited as evidence in `docs/`.
