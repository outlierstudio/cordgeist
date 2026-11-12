# Dependencies

Original Cordgeist source is MIT. Vendored libaoahid 2.0.3 and its Rust bindings
are MIT; retain its LICENSE, NOTICE, and THIRD_PARTY_NOTICES.md unchanged.
The real native build dynamically links the installed libusb 1.0.30
(LGPL-2.1-or-later). This bootstrap does not redistribute the system libusb.
Upstream's shared-library build statically links GCC C++ runtime components;
release packaging must account for the GCC runtime exception and supplied notices.

Official rmcp 3.8.0 declares Apache-2.0 in its manifest; its supplied LICENSE describes the Apache-2.0 transition and retained MIT contributions. Its supplied
LICENSE is retained under vendor/notices; Cargo.lock records its artifact checksum.
Cargo downloads and the toolchain live in ignored .tools and are not vendored.
Cargo.lock is the complete resolved Rust inventory, including transitive licenses
which must be collected and reviewed before a distributable P12 release.
This file is an initial inventory, not a completed release license audit.

## Private Linux beta archive material

The private archive collector uses offline locked Linux-target Cargo metadata
and retains supplied licenses/notices for every resolved dependency under
third-party-notices/rust, indexed by DEPENDENCIES.json. This includes transitive
Unicode, Zlib, MIT/Apache and other declared permissive alternatives. Pinned
libaoahid/binding root notices are retained separately, as are the installed
Rust 1.97.1 library copyright/licenses and GCC GPLv3 runtime exception texts.
System libusb remains dynamically linked and external, not redistributed by
this Linux archive. Source and binary hashes identify each candidate. None of
this changes existing first-party MIT grants or completes hardware acceptance.

## mobile-mcp Rust adaptations

Reference: mobile-next/mobile-mcp, inspected commit
3b51952bcd83955f3a743f731f5a9beaebecdab7, Apache-2.0. Reference files
src/android.ts (UI filtering/bounds, app launch/list concepts) and
src/coordinate-mapping.ts (orientation/coordinate descriptions), and
src/server.ts (selected tool names/argument shapes). Independently
written Rust adaptations add bounded parsing, privacy, cancellation and daemon
provenance; no TypeScript copied. Full license and adaptation notice are in
vendor/notices/mobile-mcp/. Upstream contained no NOTICE file. Root Cordgeist
license remains MIT; adapted portions retain applicable Apache-2.0 terms.

quick-xml 0.42.0 is MIT, copyright2016 Johann Tuffe; supplied registry license
is collected by the existing packaging notice collector.

Portable Linux archives additionally bundle unmodified shared libusb 1.0.30,
commit 87a55632db62c9bdc58cd31d3ccfa673f1bb017f, LGPL-2.1-or-later.
Actual COPYING and source-header terms were inspected. See vendor/notices/libusb,
packaging/PORTABLE-BUILD.md and the corresponding source included with the archive.
The shared library remains replaceable; no license enforcement restricts debugging
modifications. This does not change Cordgeist's MIT license.
